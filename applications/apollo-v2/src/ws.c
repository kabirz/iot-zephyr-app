// SPDX-License-Identifier: Apache-2.0
/* WebSocket 服务 (/ws): 实时状态推送 + 固件升级/字库安装通道。
 * 参考 io-edge-hub ws_io 架构: 每个 socket 只有一个线程, 所有发送
 * (状态推送 + 命令响应 + 进度) 都在该线程串行化 —— 并发发送同一
 * socket 会因 net buffer 争用卡死 (实测教训)。
 *
 * 服务端 -> 客户端: 1s 状态推送 (t:"info"); 字库擦除进度 (t:"font")
 * 客户端 -> 服务端 (TEXT): {"cmd":"fw_start","size":n} / {"cmd":"fw_end"}
 *          {"cmd":"font_start","size":n} / {"cmd":"font_end"}
 * 客户端 -> 服务端 (BINARY): 固件 -> slot1 (flash_img); 字库 -> font 分区直写
 *
 * 注意 (V1 修复 #2): 服务器->客户端帧禁止掩码。
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/websocket.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/base64.h>

LOG_MODULE_REGISTER(apollo_ws, LOG_LEVEL_INF);

#define WS_RX_BUF_SIZE	2048
#define WS_TX_BUF_SIZE	1024
#define WS_STACK_SIZE	2048
#define WS_PUSH_MS	1000

#define FONT_AREA_ID	PARTITION_ID(font_partition)
#define FONT_MAX_SIZE	PARTITION_SIZE(font_partition)

extern int web_build_info_json(char *json, size_t bufsz);

static struct k_mutex ws_lock;	/* 保护 fw/ft 状态 */

/* ---------------- 固件升级状态 (slot1, flash_img) ---------------- */
static struct {
	struct flash_img_context fic;
	uint32_t total;
	uint32_t received;
	bool active;
	bool failed;
} fw;

/* ---------------- 字库安装状态 (font 分区直写) ----------------
 * phase 由 ws 线程推进: ERASE 每轮擦 1 扇区, DATA 每帧写一块 */
enum font_phase {
	FONT_IDLE = 0,
	FONT_ERASE,
	FONT_DATA,
};

static struct {
	const struct flash_area *fa;
	uint32_t total;
	uint32_t off;
	uint32_t erase_off;
	uint32_t erase_len;
	enum font_phase phase;
	bool failed;
} ft;

static void fw_reset(void)
{
	k_mutex_lock(&ws_lock, K_FOREVER);
	fw.active = false;
	fw.failed = false;
	fw.total = 0;
	fw.received = 0;
	k_mutex_unlock(&ws_lock);
}

static void font_reset(void)
{
	k_mutex_lock(&ws_lock, K_FOREVER);
	if (ft.fa != NULL) {
		flash_area_close(ft.fa);
		ft.fa = NULL;
	}
	ft.phase = FONT_IDLE;
	ft.failed = false;
	ft.off = 0;
	k_mutex_unlock(&ws_lock);
}

/* ---------------- 连接槽 (单连接) ---------------- */
struct ws_slot {
	struct k_thread thread;
	int sock;
	bool in_use;
	uint8_t rx_buf[WS_RX_BUF_SIZE];
	uint8_t tx_buf[WS_TX_BUF_SIZE];
};

static struct ws_slot ws_slot;
static K_THREAD_STACK_DEFINE(ws_stack, WS_STACK_SIZE);

static bool json_get_int(const char *js, size_t len, const char *key, int32_t *out)
{
	char pat[20];
	int n = snprintf(pat, sizeof(pat), "\"%s\":", key);

	if (n <= 0 || (size_t)n >= sizeof(pat)) {
		return false;
	}
	for (size_t i = 0; i + (size_t)n < len; i++) {
		if (memcmp(js + i, pat, n) == 0) {
			*out = (int32_t)strtol(js + i + n, NULL, 10);
			return true;
		}
	}
	return false;
}

/* ---------------- WS 线程 ---------------- */
static void ws_thread(void *p1, void *p2, void *p3)
{
	struct ws_slot *s = p1;
	int64_t last_push = 0;
	bool fw_reboot_pending = false;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	LOG_INF("ws connected");
	k_msleep(300);	/* 避开 101 握手竞态 */
	last_push = k_uptime_get() - WS_PUSH_MS;

	while (true) {
		bool continue_loop = false;
		uint32_t type = 0;
		uint64_t remaining = 0;
		int len = websocket_recv_msg(s->sock, s->rx_buf,
					     sizeof(s->rx_buf) - 1, &type,
					     &remaining, 200);
		if (len == -EAGAIN) {
			;	/* 超时: 推进状态机/推送 */
		} else if (len < 0) {
			LOG_INF("ws closed (%d)", len);
			break;
		} else if (len > 0 && (type & WEBSOCKET_FLAG_TEXT)) {
			s->rx_buf[len] = '\0';
			const char *cmd = strstr((const char *)s->rx_buf, "\"cmd\":");
			int32_t v = 0;

			/* 字库数据帧: "B64:<base64>" -> 解码直写 font 分区 */
			if (ft.phase == FONT_DATA && !ft.failed &&
			    strncmp((const char *)s->rx_buf, "B64:", 4) == 0) {
				size_t b64_len = len - 4;
				static uint8_t dec[1024];
				size_t dec_len = sizeof(dec);
				size_t real = 0;

				if (b64_len % 4 != 0) {
					b64_len -= b64_len % 4;
				}
				if (base64_decode(dec, sizeof(dec), &real,
						  (const char *)s->rx_buf + 4,
						  b64_len) != 0) {
					printk("FONT B64 decode failed\n");
					ft.failed = true;
					continue_loop = true;
					goto text_done;
				}
				k_mutex_lock(&ws_lock, K_FOREVER);
				uint32_t space = ft.total - ft.off;
				size_t wlen = MIN((size_t)real, space);
				int rc = flash_area_write(ft.fa, ft.off, dec, wlen);
				k_mutex_unlock(&ws_lock);
				if (rc != 0) {
					printk("FONT write failed @%u rc=%d\n",
					       ft.off, rc);
					ft.failed = true;
				} else {
					ft.off += wlen;
				}
			}

			if (cmd == NULL) {
				;
			} else if (strstr(cmd, "\"fw_start\"") != NULL) {
				if (json_get_int(cmd, len, "size", &v) && v > 0) {
					fw_reset();
					if (flash_img_init(&fw.fic) == 0) {
						k_mutex_lock(&ws_lock, K_FOREVER);
						fw.total = v;
						fw.active = true;
						k_mutex_unlock(&ws_lock);
						LOG_INF("fw start: %u", v);
					}
				}
			} else if (strstr(cmd, "\"fw_end\"") != NULL) {
				k_mutex_lock(&ws_lock, K_FOREVER);
				bool done_ok = fw.active && !fw.failed &&
					       fw.received == fw.total;
				k_mutex_unlock(&ws_lock);
				if (done_ok &&
				    flash_img_buffered_write(&fw.fic, NULL, 0,
							     true) == 0 &&
				    boot_request_upgrade(1) == 0) {
					LOG_INF("fw done, reboot for swap");
					fw_reboot_pending = true;
				}
				fw_reset();
			} else if (strstr(cmd, "\"font_start\"") != NULL) {
				if (json_get_int(cmd, len, "size", &v) && v > 0 &&
				    (uint32_t)v <= FONT_MAX_SIZE) {
					k_mutex_lock(&ws_lock, K_FOREVER);
					if (ft.fa == NULL &&
					    flash_area_open(FONT_AREA_ID,
							    &ft.fa) == 0) {
						ft.total = v;
						ft.off = 0;
						ft.erase_off = 0;
						ft.erase_len = ROUND_UP((uint32_t)v, 4096);
						ft.phase = FONT_ERASE;
						ft.failed = false;
						LOG_INF("font start: %u", v);
					}
					k_mutex_unlock(&ws_lock);
				}
			} else if (strstr(cmd, "\"font_end\"") != NULL) {
				k_mutex_lock(&ws_lock, K_FOREVER);
				bool font_done = ft.phase == FONT_DATA &&
						 ft.off == ft.total;
				printk("FONT_END: phase=%d off=%u/%u failed=%d\n",
				       ft.phase, ft.off, ft.total, ft.failed);
				k_mutex_unlock(&ws_lock);
				if (font_done) {
					printk("FONT: reboot to apply\n");
					font_reset();
					fw_reboot_pending = true;
				}
			}
		} else if (len > 0 && (type & WEBSOCKET_FLAG_BINARY)) {
			k_mutex_lock(&ws_lock, K_FOREVER);
			bool font_active = ft.phase == FONT_DATA && !ft.failed;
			bool fw_active = fw.active && !fw.failed;
			k_mutex_unlock(&ws_lock);

			if (font_active) {
				k_mutex_lock(&ws_lock, K_FOREVER);
				uint32_t space = ft.total - ft.off;
				size_t wlen = MIN((size_t)len, space);
				int rc = flash_area_write(ft.fa, ft.off,
							  (const uint8_t *)s->rx_buf,
							  wlen);
				if (rc == 0) {
					ft.off += wlen;
				} else {
					printk("FONT WRITE FAILED @%u rc=%d\n",
					       ft.off, rc);
					ft.failed = true;
				}
				k_mutex_unlock(&ws_lock);
			} else if (fw_active) {
				k_mutex_lock(&ws_lock, K_FOREVER);
				int rc = flash_img_buffered_write(&fw.fic,
								  (const uint8_t *)s->rx_buf,
								  len, false);
				if (rc == 0) {
					fw.received += len;
				} else {
					LOG_ERR("fw write failed");
					fw.failed = true;
				}
				k_mutex_unlock(&ws_lock);
			}
		}

text_done:
		if (continue_loop) {
			continue;
		}

		/* 字库擦除状态机: 每轮 1 扇区, 占空比 ~20% (电流突发摊薄) */
		bool erase_tick = false;
		uint32_t erase_off = 0, erase_len = 0;

		k_mutex_lock(&ws_lock, K_FOREVER);
		if (ft.phase == FONT_ERASE && ft.fa != NULL) {
			if (flash_area_erase(ft.fa, ft.erase_off, 4096) != 0) {
				LOG_ERR("font erase failed @%u", ft.erase_off);
				ft.failed = true;
				ft.phase = FONT_IDLE;
				flash_area_close(ft.fa);
				ft.fa = NULL;
			} else {
				ft.erase_off += 4096;
				if (ft.erase_off >= ft.erase_len) {
					ft.off = 0;
					ft.phase = FONT_DATA;
					LOG_INF("font erase done, send data");
				}
			}
			erase_tick = true;
			erase_off = ft.erase_off;
			erase_len = ft.erase_len;
		}
		k_mutex_unlock(&ws_lock);
		if (erase_tick) {
			k_msleep(80);	/* 擦除间歇 */
		}

		/* 周期推送: 状态帧; 擦除期替换为字库进度帧 */
		if (k_uptime_get() - last_push >= WS_PUSH_MS) {
			int n;

			k_mutex_lock(&ws_lock, K_FOREVER);
			bool erasing = ft.phase == FONT_ERASE;
			uint32_t e_off = ft.erase_off, e_len = ft.erase_len;
			k_mutex_unlock(&ws_lock);

			if (erasing) {
				n = snprintf(s->tx_buf, sizeof(s->tx_buf),
					     "{\"t\":\"font\",\"done\":%u,\"total\":%u}",
					     e_off, e_len);
			} else {
				n = web_build_info_json(s->tx_buf,
							sizeof(s->tx_buf));
			}
			if (n <= 0 ||
			    websocket_send_msg(s->sock, s->tx_buf, n,
					       WEBSOCKET_OPCODE_DATA_TEXT,
					       false, true, 500) < 0) {
				LOG_INF("ws send failed, closing");
				break;
			}
			last_push = k_uptime_get();
		}

		if (fw_reboot_pending) {
			/* 等最后响应送达浏览器 */
			k_sleep(K_SECONDS(1));
			sys_reboot(SYS_REBOOT_WARM);
		}
	}

	websocket_unregister(s->sock);
	s->sock = -1;
	s->in_use = false;
	font_reset();
	fw_reset();
	LOG_INF("ws released");
}

/* ---------------- /ws 资源 ---------------- */
static int ws_setup(int ws_socket, struct http_request_ctx *request_ctx, void *user_data)
{
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (ws_slot.in_use) {
		LOG_WRN("ws busy (single connection limit)");
		return -ENOENT;
	}
	ws_slot.sock = ws_socket;
	ws_slot.in_use = true;
	k_thread_create(&ws_slot.thread, ws_stack,
			K_THREAD_STACK_SIZEOF(ws_stack),
			ws_thread, &ws_slot, NULL, NULL, 8, 0, K_NO_WAIT);
	k_thread_name_set(&ws_slot.thread, "apollo_ws");
	return 0;
}

static uint8_t ws_data_buffer[WS_RX_BUF_SIZE];

struct http_resource_detail_websocket apollo_ws_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_WEBSOCKET,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = ws_setup,
	.data_buffer = ws_data_buffer,
	.data_buffer_len = sizeof(ws_data_buffer),
	.user_data = NULL,
};

static int ws_init(void)
{
	k_mutex_init(&ws_lock);
	ws_slot.sock = -1;
	return 0;
}
SYS_INIT(ws_init, APPLICATION, 60);
