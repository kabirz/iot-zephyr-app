/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebSocket 实时通道 (/ws) — 页面数据全部走这里, HTTP 只留控制/文件操作
 *
 * 推送帧 (JSON 文本, 按 "t" 字段分发):
 *   {"t":"io",...}    IO 快照 (DI/DO/AI + 使能), 1s 周期
 *   {"t":"regs",...}  寄存器全量 (holding/input), 1s 周期
 *   {"t":"info",...}  设备详情 (版本/存储/链路), 10s 周期
 *   连接建立后立即推 io + regs + info 各一帧
 *
 * 接收命令 (与 HTTP POST 共用执行器, 同 Modbus 副作用路径):
 *   {"cmd":"do","index":n,"value":0/1}
 *   {"cmd":"reg","addr":n,"value":v}
 *   {"cmd":"time","ts":unix}
 *   {"cmd":"cfg","ip":"a.b.c.d","rs485":n,"sid":n,"can_bps":n,"can_id":n}
 *   {"cmd":"save"}	参数持久化到 FCB
 *   {"cmd":"factory_reset"}	恢复出厂设置 (擦参数区 + 延迟重启)
 *   {"cmd":"fw_start","size":n}	开始固件升级 (擦 slot1)
 *   <binary frame>		固件数据 (bin 帧)
 *   {"cmd":"fw_end"}		结束升级 (CRC 校验 + boot_request_upgrade)
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/websocket.h>
#include <zephyr/logging/log.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/base64.h>
#include <fw_keyhash.h>

#include "web_json.h"
#include "ws_io.h"
#include "web_cmds.h"
#include "watchdog.h"
#include <init.h>

LOG_MODULE_REGISTER(io_ws, LOG_LEVEL_INF);

#define WS_RX_BUF_SIZE	256
#define WS_TX_BUF_SIZE	640	/* info 帧最大 (~500B) */
#define WS_PUSH_MS	1000	/* io / regs 推送周期 */
#define WS_INFO_MS	10000	/* info 推送周期 */

/* ==================== 固件升级状态 ==================== */

#define SLOT1_PARTITION_ID PARTITION_ID(slot1_partition)
#define IMG_MAGIC		0x96F3B83D
#define IMG_TLV_INFO_MAGIC	0x6907
#define IMG_TLV_KEYHASH		0x0001
#define FW_CRC_CHUNK		64

/* 验证客户端发送的 keyhash 与编译时固件签名密钥匹配 */
static bool fw_upg_verify_client_keyhash(const char *b64_keyhash, size_t b64_len)
{
	uint8_t received_keyhash[FW_KEYHASH_KEY_LEN];
	size_t decoded_len;

	if (b64_keyhash == NULL || b64_len == 0) {
		/* 客户端未发送 keyhash, 跳过验证 (兼容旧客户端) */
		return true;
	}

	if (base64_decode(received_keyhash, sizeof(received_keyhash), &decoded_len,
			  (const uint8_t *)b64_keyhash, b64_len) != 0 ||
	    decoded_len != FW_KEYHASH_KEY_LEN) {
		LOG_WRN("fw_start: bad keyhash (decode failed or length mismatch)");
		return false;
	}

	if (memcmp(received_keyhash, fw_keyhash, FW_KEYHASH_KEY_LEN) != 0) {
		LOG_WRN("fw_start: keyhash mismatch");
		return false;
	}

	LOG_INF("fw_start: keyhash verified OK");
	return true;
}

static struct {
	bool active;
	bool failed;
	uint32_t total;
	uint32_t received;
	uint16_t crc;
	struct flash_img_context fic;
} fw_upg;

/* ==================== IO 快照 JSON (推送帧 / GET /api/io 共用) ==================== */

int ws_io_build_status(char *buf, size_t bufsz)
{
	uint16_t di = get_input_reg(INPUT_DI_IDX);
	uint16_t do_v = get_holding_reg(HOLDING_DO_IDX);
	uint16_t di_en = get_holding_reg(HOLDING_DI_ENABLE_IDX);
	uint16_t ai_en = get_holding_reg(HOLDING_AI_ENABLE_IDX);
	int n = 0;

	n += snprintf(buf + n, bufsz - n, "{\"t\":\"io\",\"di\":[");
	for (int i = 0; i < DI_NUM; i++) {
		n += snprintf(buf + n, bufsz - n, "%s%u", i ? "," : "",
			      (di >> i) & 1);
	}
	n += snprintf(buf + n, bufsz - n, "],\"do\":[");
	for (int i = 0; i < DO_NUM; i++) {
		n += snprintf(buf + n, bufsz - n, "%s%u", i ? "," : "",
			      (do_v >> i) & 1);
	}
	n += snprintf(buf + n, bufsz - n, "],\"ai\":[");
	for (int i = 0; i < AI_NUM; i++) {
		n += snprintf(buf + n, bufsz - n, "%s%u", i ? "," : "",
			      get_input_reg(INPUT_AI0_IDX + i));
	}
	n += snprintf(buf + n, bufsz - n,
		      "],\"di_en\":%u,\"ai_en\":%u,\"ms\":%lld}",
		      di_en, ai_en, (long long)k_uptime_get());
	return n;
}

/* ==================== 连接槽位 + 处理线程 ==================== */

struct ws_slot {
	int sock;
	struct k_thread thread;
	bool in_use;
	char rx_buf[WS_RX_BUF_SIZE];
	char tx_buf[WS_TX_BUF_SIZE];
};

static K_THREAD_STACK_ARRAY_DEFINE(ws_stacks, CONFIG_IO_WEB_WS_HANDLERS,
				   CONFIG_IO_WEB_WS_STACK_SIZE);
static struct ws_slot ws_slots[CONFIG_IO_WEB_WS_HANDLERS];

static int ws_get_free_slot(void)
{
	for (int i = 0; i < CONFIG_IO_WEB_WS_HANDLERS; i++) {
		if (!ws_slots[i].in_use) {
			return i;
		}
	}
	return -1;
}

static void fw_upg_reset(void)
{
	fw_upg.active = false;
	fw_upg.failed = false;
	fw_upg.total = 0;
	fw_upg.received = 0;
	fw_upg.crc = 0;
}

static bool fw_upg_verify_crc(void)
{
	const struct flash_area *fa;
	uint8_t buf[FW_CRC_CHUNK];
	uint16_t calc = 0;
	size_t written = flash_img_bytes_written(&fw_upg.fic);

	if (flash_area_open(SLOT1_PARTITION_ID, &fa) != 0) {
		return false;
	}
	for (size_t off = 0; off < written; off += FW_CRC_CHUNK) {
		size_t len = MIN(written - off, FW_CRC_CHUNK);

		if (flash_area_read(fa, off, buf, len) != 0) {
			flash_area_close(fa);
			return false;
		}
		calc = crc16_ccitt(calc, buf, len);
	}
	flash_area_close(fa);
	if (calc != fw_upg.crc) {
		LOG_ERR("fw CRC mismatch: calc=0x%04x recv=0x%04x",
			calc, fw_upg.crc);
		return false;
	}
	return true;
}

/* 从 slot1 已写镜像的 TLV 区提取 KEYHASH 与编译期 fw_keyhash 比对
 * (与 CAN/UDP 升级通道的 keyhash 预校验对齐; WS 无独立 keyhash 帧,
 * 从写入的镜像自身解析). TLV 偏移 = 头部 hdr_size + img_size —
 * 镜像未填满分区, 不能从分区末尾定位. */
static bool fw_upg_verify_keyhash(void)
{
	const struct flash_area *fa;
	uint8_t hdr[32];
	uint8_t tlv_hdr[4];
	uint8_t kh[FW_KEYHASH_KEY_LEN];
	bool ok = false;

	if (flash_area_open(SLOT1_PARTITION_ID, &fa) != 0) {
		return false;
	}

	/* 镜像头: magic LE32 @0, hdr_size LE16 @8, img_size LE32 @12 */
	if (flash_area_read(fa, 0, hdr, sizeof(hdr)) != 0 ||
	    sys_get_le32(hdr) != IMG_MAGIC) {
		LOG_ERR("fw keyhash: no image magic");
		goto out;
	}
	uint32_t hdr_size = sys_get_le16(hdr + 8);
	uint32_t img_size = sys_get_le32(hdr + 12);
	uint32_t tlv_off = hdr_size + img_size;

	if (hdr_size < 32 || (hdr_size & 0x3) || tlv_off % 4 ||
	    tlv_off + 4 > fw_upg.received) {
		LOG_ERR("fw keyhash: bad image header (hdr=%u img=%u)",
			hdr_size, img_size);
		goto out;
	}
	if (flash_area_read(fa, tlv_off, tlv_hdr, sizeof(tlv_hdr)) != 0 ||
	    sys_get_le16(tlv_hdr) != IMG_TLV_INFO_MAGIC) {
		LOG_ERR("fw keyhash: no TLV info magic");
		goto out;
	}

	/* 遍历 TLV 找 KEYHASH (type=0x0001, len=32). MCUboot 镜像头与 TLV
	 * 字段均为小端 */
	uint32_t off = tlv_off + 4;
	uint16_t type = 0, len = 0;

	for (; off + 4 <= fw_upg.received; off += 4 + len) {
		if (flash_area_read(fa, off, tlv_hdr, sizeof(tlv_hdr)) != 0) {
			goto out;
		}
		type = sys_get_le16(tlv_hdr);
		len = sys_get_le16(tlv_hdr + 2);
		if (type == 0 || len == 0 || off + 4 + len > fw_upg.received) {
			break;
		}
		if (type == IMG_TLV_KEYHASH && len == FW_KEYHASH_KEY_LEN) {
			if (flash_area_read(fa, off + 4, kh, sizeof(kh)) != 0) {
				goto out;
			}
			if (memcmp(kh, fw_keyhash, FW_KEYHASH_KEY_LEN) != 0) {
				LOG_ERR("fw keyhash mismatch");
				goto out;
			}
			ok = true;
			break;
		}
	}
	if (!ok) {
		LOG_ERR("fw keyhash: KEYHASH TLV not found (unsigned image?)");
	}

out:
	flash_area_close(fa);
	return ok;
}

/* 处理一条客户端 JSON 命令, 回 ack (do 命令回改变后的完整快照) */
static void ws_handle_cmd(struct ws_slot *s, const char *cmd, size_t len)
{
	int32_t index = 0, addr = 0, value = 0, ts = 0;
	int n;

	if (strncmp(cmd, "\"do\"", 4) == 0 &&
	    json_get_i32(cmd, len, "index", &index) &&
	    json_get_i32(cmd, len, "value", &value)) {
		int rc = web_cmd_exec_do(index, value);

		if (rc == 0) {
			n = ws_io_build_status(s->tx_buf, sizeof(s->tx_buf));
		} else {
			n = snprintf(s->tx_buf, sizeof(s->tx_buf),
				     "{\"ok\":false,\"err\":\"bad index\"}");
		}
	} else if (strncmp(cmd, "\"reg\"", 5) == 0 &&
		   json_get_i32(cmd, len, "addr", &addr) &&
		   json_get_i32(cmd, len, "value", &value)) {
		n = snprintf(s->tx_buf, sizeof(s->tx_buf), "{\"ok\":%s}",
			     web_cmd_exec_reg(addr, value) == 0 ? "true" : "false");
	} else if (strncmp(cmd, "\"time\"", 6) == 0 &&
		   json_get_i32(cmd, len, "ts", &ts)) {
		n = snprintf(s->tx_buf, sizeof(s->tx_buf), "{\"ok\":%s}",
			     set_timestamp((time_t)ts) ? "true" : "false");
	} else if (strncmp(cmd, "\"cfg\"", 5) == 0) {
		/* 系统配置: 字段可选, 校验失败回具体原因 */
		const char *err = "invalid";

		if (web_cmd_exec_cfg(cmd, len, &err) == 0) {
			n = snprintf(s->tx_buf, sizeof(s->tx_buf), "{\"ok\":true}");
		} else {
			n = snprintf(s->tx_buf, sizeof(s->tx_buf),
				     "{\"ok\":false,\"err\":\"%s\"}", err);
		}
	} else if (strncmp(cmd, "\"save\"", 6) == 0) {
		holding_reg_save();
		n = snprintf(s->tx_buf, sizeof(s->tx_buf), "{\"ok\":true}");
	} else if (strncmp(cmd, "\"factory_reset\"", 14) == 0) {
		/* 恢复出厂: 擦 storage_partition (FCB 参数), 成功后置延迟重启
		 * (main 循环 history_sync + 冷重启, 与 UDP/升级路径一致) */
		if (settings_factory_reset() == 0) {
			LOG_INF("factory reset via ws, rebooting");
			n = snprintf(s->tx_buf, sizeof(s->tx_buf), "{\"ok\":true}");
			set_reboot_status(true);
		} else {
			n = snprintf(s->tx_buf, sizeof(s->tx_buf),
				     "{\"ok\":false,\"err\":\"erase failed\"}");
		}
	} else if (strncmp(cmd, "\"fw_start\"", 10) == 0 && !fw_upg.active) {
		int32_t fw_size = 0;

		json_get_i32(cmd, len, "size", &fw_size);
		if (fw_size <= 0) {
			n = snprintf(s->tx_buf, sizeof(s->tx_buf),
				     "{\"ok\":false,\"err\":\"bad size\"}");
		} else {
			/* 解析客户端发送的 keyhash (可选, Base64 编码) */
			char b64_keyhash[64];
			const char *keyhash_ptr = NULL;
			size_t keyhash_len = 0;

			if (json_get_str(cmd, len, "keyhash", b64_keyhash, sizeof(b64_keyhash))) {
				keyhash_ptr = b64_keyhash;
				keyhash_len = strlen(b64_keyhash);
			}

			/* 在擦除 flash 之前验证 keyhash */
			if (!fw_upg_verify_client_keyhash(keyhash_ptr, keyhash_len)) {
				n = snprintf(s->tx_buf, sizeof(s->tx_buf),
					     "{\"ok\":false,\"err\":\"keyhash mismatch\"}");
			} else {
				const struct flash_area *fa;

				if (flash_area_open(SLOT1_PARTITION_ID, &fa) != 0) {
					n = snprintf(s->tx_buf, sizeof(s->tx_buf),
						     "{\"ok\":false,\"err\":\"flash open\"}");
				} else {
					watchdog_feed();
					int rc = flash_area_erase(fa, 0,
								  ROUND_UP((uint32_t)fw_size, 4096));

					watchdog_feed();
					flash_area_close(fa);
					if (rc != 0 || flash_img_init(&fw_upg.fic) != 0) {
						n = snprintf(s->tx_buf, sizeof(s->tx_buf),
							     "{\"ok\":false,\"err\":\"erase/init\"}");
					} else {
						fw_upg.active = true;
						fw_upg.total = (uint32_t)fw_size;
						LOG_INF("fw upgrade started (size=%d)", fw_size);
						n = snprintf(s->tx_buf, sizeof(s->tx_buf),
							     "{\"ok\":true}");
					}
				}
			}
		}
	} else if (strncmp(cmd, "\"fw_end\"", 8) == 0 && fw_upg.active) {
		flash_img_buffered_write(&fw_upg.fic, NULL, 0, true);
		if (fw_upg.received == 0) {
			n = snprintf(s->tx_buf, sizeof(s->tx_buf),
				     "{\"ok\":false,\"err\":\"no data\"}");
		} else if (fw_upg.received != fw_upg.total) {
			/* 尺寸不符 = 传输缺帧, 残缺镜像不能交给 MCUboot */
			LOG_ERR("fw size mismatch: recv=%u expect=%u",
				fw_upg.received, fw_upg.total);
			n = snprintf(s->tx_buf, sizeof(s->tx_buf),
				     "{\"ok\":false,\"err\":\"size mismatch\"}");
		} else if (!fw_upg_verify_crc()) {
			n = snprintf(s->tx_buf, sizeof(s->tx_buf),
				     "{\"ok\":false,\"err\":\"crc mismatch\"}");
		} else if (!fw_upg_verify_keyhash()) {
			n = snprintf(s->tx_buf, sizeof(s->tx_buf),
				     "{\"ok\":false,\"err\":\"keyhash mismatch\"}");
		} else if (boot_request_upgrade(1) != 0) {
			n = snprintf(s->tx_buf, sizeof(s->tx_buf),
				     "{\"ok\":false,\"err\":\"boot_request\"}");
		} else {
			LOG_INF("fw upgrade verified, rebooting for swap");
			fw_upg_reset();
			n = snprintf(s->tx_buf, sizeof(s->tx_buf), "{\"ok\":true}");
			set_reboot_status(true);
		}
		fw_upg_reset();
	} else {
		n = snprintf(s->tx_buf, sizeof(s->tx_buf),
			     "{\"ok\":false,\"err\":\"unknown cmd\"}");
	}
	(void)websocket_send_msg(s->sock, s->tx_buf, n,
				 WEBSOCKET_OPCODE_DATA_TEXT, false, true,
				 1000);
}

static void ws_thread(void *p1, void *p2, void *p3)
{
	struct ws_slot *s = p1;
	int64_t last_push = 0;
	int64_t last_info = 0;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("ws[%p] connected", (void *)s);

	/* 避开握手竞态窗口: 101 刚发出时浏览器可能尚未完成 WS 建立,
	 * 立即推送首帧会被部分客户端当作非法帧导致 RST */
	k_msleep(300);
	last_push = k_uptime_get() - WS_PUSH_MS;	/* 立即推首帧 */
	last_info = k_uptime_get() - WS_INFO_MS;

	while (true) {
		uint32_t type = 0;
		uint64_t remaining = 0;
		int len = websocket_recv_msg(s->sock, s->rx_buf,
					     sizeof(s->rx_buf) - 1, &type,
					     &remaining, 200);

		if (len == -EAGAIN) {
			/* 超时: 到推送周期则发快照 */
		} else if (len < 0) {
			LOG_INF("ws closed (%d)", len);
			break;
		} else if (len > 0) {
			s->rx_buf[len] = '\0';
			if (type & WEBSOCKET_FLAG_TEXT) {
				const char *cmd = json_find_value(s->rx_buf, len, "cmd");

				if (cmd != NULL) {
					ws_handle_cmd(s, cmd, len - (cmd - s->rx_buf));
				}
			} else if ((type & WEBSOCKET_FLAG_BINARY) &&
				   fw_upg.active && !fw_upg.failed) {
				if (flash_img_buffered_write(&fw_upg.fic,
							     (const uint8_t *)s->rx_buf,
							     len, false) == 0) {
					fw_upg.crc = crc16_ccitt(fw_upg.crc,
								  (const uint8_t *)s->rx_buf,
								  len);
					fw_upg.received += len;
				} else {
					LOG_ERR("fw: flash write failed @%u", fw_upg.received);
					fw_upg.failed = true;
				}
			}
		}

		if (k_uptime_get() - last_push >= WS_PUSH_MS) {
			/* io + regs 帧共用周期, 一次 recv 超时窗口内顺序发出 */
			int n = ws_io_build_status(s->tx_buf, sizeof(s->tx_buf));

			if (websocket_send_msg(s->sock, s->tx_buf, n,
					       WEBSOCKET_OPCODE_DATA_TEXT,
					       false, true, 500) < 0) {
				LOG_INF("ws send failed, closing");
				break;
			}
			n = web_build_regs_json(s->tx_buf, sizeof(s->tx_buf));
			if (websocket_send_msg(s->sock, s->tx_buf, n,
					       WEBSOCKET_OPCODE_DATA_TEXT,
					       false, true, 500) < 0) {
				LOG_INF("ws send failed, closing");
				break;
			}
			last_push = k_uptime_get();
		}

		if (k_uptime_get() - last_info >= WS_INFO_MS) {
			int n = web_build_info_json(s->tx_buf, sizeof(s->tx_buf));

			if (websocket_send_msg(s->sock, s->tx_buf, n,
					       WEBSOCKET_OPCODE_DATA_TEXT,
					       false, true, 500) < 0) {
				LOG_INF("ws send failed, closing");
				break;
			}
			last_info = k_uptime_get();
		}
	}

	websocket_unregister(s->sock);
	s->sock = -1;
	s->in_use = false;
	LOG_INF("ws[%p] released", (void *)s);
}

/* HTTP 服务器 WebSocket 升级回调.
 * 注意: websocket_register 的解析缓冲是资源级共享的, 并发连接会互相
 * 踩踏, 因此同一时刻只接受 1 条连接 (多余的被拒绝, 前端自动降级轮询). */
int ws_io_setup(int ws_socket, struct http_request_ctx *request_ctx,
		void *user_data)
{
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	int slot = ws_get_free_slot();

	if (slot < 0) {
		LOG_WRN("ws busy, rejecting (single connection limit)");
		return -ENOENT;
	}

	struct ws_slot *s = &ws_slots[slot];

	s->sock = ws_socket;
	s->in_use = true;

	k_thread_create(&s->thread, ws_stacks[slot],
			K_THREAD_STACK_SIZEOF(ws_stacks[slot]),
			ws_thread, s, NULL, NULL, 8, 0, K_NO_WAIT);
#ifdef CONFIG_THREAD_NAME
	char name[12];

	snprintf(name, sizeof(name), "ws_%d", slot);
	k_thread_name_set(&s->thread, name);
#endif
	return 0;
}

/* ==================== /ws 资源 (httpd.c 注册) ==================== */

static uint8_t ws_data_buffer[WS_RX_BUF_SIZE];

struct http_resource_detail_websocket ws_io_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_WEBSOCKET,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = ws_io_setup,
	.data_buffer = ws_data_buffer,
	.data_buffer_len = sizeof(ws_data_buffer),
	.user_data = NULL,
};

static int ws_slots_init(void)
{
	for (int i = 0; i < CONFIG_IO_WEB_WS_HANDLERS; i++) {
		ws_slots[i].sock = -1;
	}
	return 0;
}
SYS_INIT(ws_slots_init, APPLICATION, 60);
