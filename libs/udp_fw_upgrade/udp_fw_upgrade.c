/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 固件升级库 - 自包含实现 (参考 can_fw_upgrade)
 *
 * 库通过 SYS_INIT 自动创建配置端口 UDP socket (INADDR_ANY:8600),
 * 自管 RX 线程, 内部处理固件升级命令 (FW_START/DATA/END);
 * 其他配置命令通过应用注册的回调分发.
 */

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>
#include "udp_fw_upgrade.h"
#include <fw_gitver.h>

#ifdef CONFIG_MCUBOOT_SIGNATURE_KEY_FILE
#include <fw_keyhash.h>
#endif

LOG_MODULE_REGISTER(udp_fw_upgrade, LOG_LEVEL_INF);

/* 库内命令码 (配置端口帧首字节, 从 0 开始连续编号, 对齐 can_fw_upgrade).
 * 应用业务命令从 0x10 起, 不会与此区间冲突. */
enum fw_cmd {
	FW_CMD_START       = 1,   /* 开始升级 (擦 slot1 + init) */
	FW_CMD_DATA        = 2,   /* 固件数据写入 (停等, 兼容旧上位机) */
	FW_CMD_END         = 3,   /* 结束升级 (flush + CRC 校验 + boot_request_upgrade) */
	FW_CMD_GET_VERSION = 4,   /* 查询固件版本 */
	FW_CMD_REBOOT      = 5,   /* 重启设备 */
	FW_CMD_DATA_V2     = 6,   /* 带偏移数据帧 [offset 4B][data]: 窗口流水线 + go-back-N */
};

#define SLOT1_PARTITION_ID PARTITION_ID(slot1_partition)

/* FW_START 应答状态字节 (保持旧值时兼容既有上位机) */
enum fw_start_status {
	FW_START_ERR_FAIL     = 0,  /* 启动失败 (擦除/初始化) 或未开始 */
	FW_START_OK           = 1,  /* 校验通过, 已开始升级 (原有语义) */
	FW_START_ERR_KEYHASH  = 2,  /* keyhash 不一致, 已拒绝 */
};

/* ================================================================
 * 全局状态
 * ================================================================ */

/* 已注册的业务命令 handler 列表 (RX 线程按序遍历广播) */
struct udp_fw_handler {
	udp_fw_app_cmd_cb_t cb;
	void *user_data;
};
static struct udp_fw_handler handlers[CONFIG_UDP_FW_UPGRADE_MAX_HANDLERS];

/* 重启前钩子列表 (RX 线程按序遍历) */
#define UDP_FW_MAX_PRE_REBOOT_HOOKS 4
static udp_fw_pre_reboot_hook_t pre_reboot_hooks[UDP_FW_MAX_PRE_REBOOT_HOOKS];
static int pre_reboot_hook_cnt;

void udp_fw_add_pre_reboot_hook(udp_fw_pre_reboot_hook_t hook)
{
	if (hook == NULL || pre_reboot_hook_cnt >= UDP_FW_MAX_PRE_REBOOT_HOOKS) {
		return;
	}
	pre_reboot_hooks[pre_reboot_hook_cnt++] = hook;
}

/* 固件升级开始前钩子 (keyhash 校验通过后、擦 flash 前调用) */
static udp_fw_pre_start_hook_t pre_start_hook;

void udp_fw_add_pre_start_hook(udp_fw_pre_start_hook_t hook)
{
	pre_start_hook = hook;
}

/* 固件升级会话失败钩子 (应用释放升级锁等资源) */
static udp_fw_fail_hook_t fw_fail_hook;

void udp_fw_add_fail_hook(udp_fw_fail_hook_t hook)
{
	fw_fail_hook = hook;
}

/* 配置端口 socket + 发送方地址 (回复路由用) */
static int config_sock = -1;
static struct sockaddr_in config_remote_addr;

/* 允许跨子网广播接收的命令 allowlist (默认空; 应用通过
 * udp_fw_allow_broadcast_cmd() 放行, 如网络发现命令 GET_NET/SET_NET).
 * 未放行的命令跨子网接收时被 RX 线程直接丢弃, 不执行也不回复. */
#ifdef CONFIG_UDP_FW_REPLY_BCAST_RESTRICT
static uint8_t bcast_allowed_cmds[CONFIG_UDP_FW_UPGRADE_MAX_BCAST_CMDS];
static size_t bcast_allowed_cnt;

static bool bcast_cmd_allowed(uint8_t cmd)
{
	for (size_t i = 0; i < bcast_allowed_cnt; i++) {
		if (bcast_allowed_cmds[i] == cmd) {
			return true;
		}
	}
	return false;
}
#endif

/* 固件升级状态 */
static struct flash_img_context flash_img_ctx;
static bool fw_started;
static uint32_t fw_size;        /* START 保存的固件大小 */
static uint32_t fw_received;    /* 已接收字节数 (用于 DATA offset 回复) */

#define FW_CRC_CHUNK 64         /* 读回 slot1 重算 CRC 的分块大小 */

/* 会话以失败告终: 清状态并通知应用 (释放升级锁等资源)。
 * FW_END 成功不调用 — 镜像已校验待重启, 升级锁保持占用直到重启 */
static void fw_session_fail(void)
{
	fw_started = false;
	if (fw_fail_hook != NULL) {
		fw_fail_hook();
	}
}

/* ================================================================
 * 子网判断 + 回复
 * ================================================================ */

/* 判断发送方 IP 是否与本机同子网.
 * 本机 IP 从网络接口直接获取 (net_if_ipv4_get_global_addr),
 * 不依赖应用参数结构体, 保证库的自治性. */
static bool is_same_subnet(struct in_addr sender_ip)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		return true;  /* 无法判断时按同子网处理 (单播) */
	}

	/* 从接口获取本机首选全局 IPv4 地址 */
	struct in_addr *local_p = (struct in_addr *)
		net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

	if (!local_p) {
		return true;  /* 还没拿到 IP, 按同子网 (单播) */
	}

	/* 用本机 IP 反查 netmask (与应用侧 is_same_subnet 逻辑一致) */
	struct net_in_addr nm = net_if_ipv4_get_netmask_by_addr(
		iface, (const struct net_in_addr *)local_p);
	struct in_addr mask = *(struct in_addr *)&nm;

	return (sender_ip.s_addr & mask.s_addr) ==
	       (local_p->s_addr & mask.s_addr);
}

/* 配置端口回复: 通过配置 socket 回复. 同子网单播给发送方, 跨子网广播.
 * 回复格式: [cmd 1B][data...] (无魔数头) */
void udp_fw_reply(uint8_t cmd, const uint8_t *data, uint8_t len)
{
	if (config_sock < 0) {
		return;
	}

	uint8_t buf[64] = { 0 };
	size_t send_len = 1;  /* cmd */

	buf[0] = cmd;
	if (len > 0 && len <= sizeof(buf) - 1) {
		memcpy(buf + 1, data, len);
		send_len += len;
	}

	struct sockaddr_in dst;

	if (is_same_subnet(config_remote_addr.sin_addr)) {
		/* 同子网: 单播回复到发送方源地址 (端口=发送方源端口) */
		dst = config_remote_addr;
	} else {
		/* 跨子网: 定向广播回复 (上位机监听 config+1).
		 * 能走到这里说明命令已在 RX 线程通过广播放行检查, 可安全广播回复. */
		dst.sin_family = AF_INET;
		dst.sin_port = htons(CONFIG_UDP_FW_CONFIG_PORT + 1);
		dst.sin_addr.s_addr = INADDR_BROADCAST;
	}

	sendto(config_sock, buf, send_len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* ================================================================
 * 固件升级命令处理
 * ================================================================ */

/* 读回 slot1 已写数据, 重算 CRC16-CCITT, 与 recv_crc 比对 */
static bool fw_verify_crc(uint16_t recv_crc)
{
	const struct flash_area *fa;

	if (flash_area_open(SLOT1_PARTITION_ID, &fa) != 0) {
		LOG_ERR("CRC verify: flash_area_open failed");
		return false;
	}

	size_t written = flash_img_bytes_written(&flash_img_ctx);
	uint16_t calc_crc = 0;
	uint8_t buf[FW_CRC_CHUNK];

	for (size_t off = 0; off < written; off += FW_CRC_CHUNK) {
		size_t len = (written - off < FW_CRC_CHUNK) ? (written - off) : FW_CRC_CHUNK;

		if (flash_area_read(fa, off, buf, len) != 0) {
			flash_area_close(fa);
			LOG_ERR("CRC verify: flash_area_read failed @%zu", off);
			return false;
		}
		calc_crc = crc16_ccitt(calc_crc, buf, len);
	}

	flash_area_close(fa);
	if (calc_crc != recv_crc) {
		LOG_ERR("CRC mismatch: calc=0x%04x recv=0x%04x", calc_crc, recv_crc);
		return false;
	}
	return true;
}

/* 构造版本字符串 "v<M>.<m>.<p>_<6hex>" 到 buf, 返回字符串长度 (不含 '\0').
 * 去掉 EXTRAVERSION (-dev 等), 加 v 前缀和 6 位 git commit 后缀.
 * 示例: v0.1.0_6e199a */
static int build_version_string(char *buf, size_t bufsz)
{
	return snprintf(buf, bufsz, "v%d.%d.%d_%s",
			APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_PATCHLEVEL,
			FW_GIT_VERSION);
}

/* 处理固件升级命令. 返回 true 表示是固件命令 (已处理). */
static bool handle_fw_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
	switch (cmd) {
	case FW_CMD_START: {
		uint8_t status = FW_START_ERR_FAIL;

		if (!fw_started && len >= 4) {
			fw_size = sys_get_le32(data);

			/* 升级前 keyhash 校验 (CONFIG_MCUBOOT_SIGNATURE_KEY_FILE): 仅当上位机在
			 * FW_START 携带 [keyhash 32B] (len==4+32) 时才校验; 老上位机发
			 * 旧 4B 帧 (无 keyhash) 时放行, 兼容旧协议. 不一致则拒绝且不触 flash. */
#ifdef CONFIG_MCUBOOT_SIGNATURE_KEY_FILE
			if (len == 4 + FW_KEYHASH_KEY_LEN &&
			    memcmp(data + 4, fw_keyhash, FW_KEYHASH_KEY_LEN) != 0) {
				LOG_WRN("FW_START rejected: keyhash mismatch");
				/* 回 [status][v2_chunk 2B]: 新上位机协商 V2, 老上位机只读 status */
				uint8_t rep[3] = {
					FW_START_ERR_KEYHASH,
					CONFIG_UDP_FW_DATA_V2_CHUNK & 0xff,
					(CONFIG_UDP_FW_DATA_V2_CHUNK >> 8) & 0xff,
				};

				udp_fw_reply(cmd, rep, 3);
				return true;
			}
#endif

			/* 调用应用注册的 pre_start 钩子 */
			if (pre_start_hook && !pre_start_hook()) {
				LOG_WRN("FW_START rejected: pre_start_hook declined");
				uint8_t rep[3] = {
					FW_START_ERR_FAIL,
					CONFIG_UDP_FW_DATA_V2_CHUNK & 0xff,
					(CONFIG_UDP_FW_DATA_V2_CHUNK >> 8) & 0xff,
				};

				udp_fw_reply(cmd, rep, 3);
				return true;
			}

			const struct flash_area *fa;

			if (flash_area_open(SLOT1_PARTITION_ID, &fa) != 0) {
				LOG_ERR("FW_START: flash_area_open failed");
				fw_session_fail();
			} else {
				flash_area_erase(fa, 0, ROUND_UP(fw_size, 4096));
				flash_area_close(fa);
				if (flash_img_init(&flash_img_ctx) != 0) {
					LOG_ERR("FW_START: flash_img_init failed");
					fw_session_fail();
				} else {
					fw_started = true;
					fw_received = 0;
					status = FW_START_OK;
					LOG_INF("FW upgrade started, size=%u", fw_size);
				}
			}
		}
		/* 回 [status][v2_chunk 2B]: 新上位机读 chunk 走 DATA_V2 窗口流水线,
		 * 老上位机只读 status 字节, 回复变长不影响兼容 */
		{
			uint8_t rep[3] = {
				status,
				CONFIG_UDP_FW_DATA_V2_CHUNK & 0xff,
				(CONFIG_UDP_FW_DATA_V2_CHUNK >> 8) & 0xff,
			};

			udp_fw_reply(cmd, rep, 3);
		}
		return true;
	}

	case FW_CMD_DATA: {
		uint8_t off[4] = { 0 };

			if (fw_started && len > 0) {
			if (flash_img_buffered_write(&flash_img_ctx, data, len, false) == 0) {
					fw_received += len;
					sys_put_le32(fw_received, off);
				} else {
					LOG_ERR("FW_DATA: flash write failed");
					fw_session_fail();
				}
			} else {
			LOG_WRN("FW data before start");
		}
		udp_fw_reply(cmd, off, 4);
		return true;
	}

	case FW_CMD_DATA_V2: {
		/* [offset 4B][data ≤ CONFIG_UDP_FW_DATA_V2_CHUNK]: offset 与已收
		 * 字节数一致才写入; 乱序/重复帧丢弃不写. 无论接受与否统一回当前
		 * 期望 offset, 上位机据此 go-back-N 重传. */
		uint8_t off[4] = { 0 };

		if (fw_started && len > 4) {
			uint32_t expect = sys_get_le32(data);

			if (expect == fw_received) {
				if (flash_img_buffered_write(&flash_img_ctx, data + 4,
							     len - 4, false) == 0) {
					fw_received += len - 4;
				} else {
					LOG_ERR("FW_DATA_V2: flash write failed");
					fw_session_fail();
				}
			}
			sys_put_le32(fw_received, off);
		} else {
			LOG_WRN("FW data before start");
		}
		udp_fw_reply(cmd, off, 4);
		return true;
	}

	case FW_CMD_END: {
		uint8_t result = 0;

		if (fw_started && len >= 3) {
			uint8_t test_mode = data[0];
			uint16_t recv_crc = sys_get_le16(data + 1);

			flash_img_buffered_write(&flash_img_ctx, NULL, 0, true);

			if (fw_verify_crc(recv_crc)) {
				int ret = boot_request_upgrade(test_mode ? 0 : 1);

				if (ret == 0) {
					result = 1;
					/* 成功: 会话结束但升级锁保持占用直到重启 */
					fw_started = false;
					LOG_INF("FW upgrade verified (test_mode=%d), waiting for reboot",
						test_mode);
				} else {
					LOG_ERR("FW_END: boot_request_upgrade failed: %d", ret);
					fw_session_fail();
				}
			} else {
				LOG_ERR("FW_END: CRC mismatch, upgrade rejected");
				fw_session_fail();
			}
		} else {
			LOG_WRN("FW end before start");
		}
		udp_fw_reply(cmd, &result, 1);
		return true;
	}

	case FW_CMD_GET_VERSION:
		/* 响应版本字符串 "v<M>.<m>.<p>_<6hex>" (无末尾 '\0').
		 * 示例: v0.1.0_6e199a */
		{
			char ver[24];
			int vlen = build_version_string(ver, sizeof(ver));

			udp_fw_reply(cmd, (const uint8_t *)ver, vlen);
		}
		return true;

	case FW_CMD_REBOOT:
		LOG_INF("reboot requested");
	{
		uint8_t ok = 1;

		udp_fw_reply(cmd, &ok, sizeof(ok));
	}
		/* 排空 deferred 日志缓冲, 给 shell 线程打印窗口, 避免随重启丢失 */
#ifdef CONFIG_LOG
		while (log_process()) {
		}
#endif
		for (int i = 0; i < pre_reboot_hook_cnt; i++) {
			pre_reboot_hooks[i]();
		}
		k_msleep(100);
		sys_reboot(SYS_REBOOT_COLD);
		return true;

	default:
		return false;
	}
}

/* ================================================================
 * RX 线程 (静态): 收配置命令 → 固件命令内部处理, 其余分发给应用回调
 * ================================================================ */
static void udp_fw_rx_thread(void *p1, void *p2, void *p3)
{
	/* 覆盖最大帧: [cmd 1B][offset 4B][data ≤ UDP_FW_DATA_V2_CHUNK] */
	static uint8_t buf[1 + 4 + CONFIG_UDP_FW_DATA_V2_CHUNK];

	while (1) {
		struct sockaddr_in src_addr;
		socklen_t addr_len = sizeof(src_addr);

		ssize_t received = recvfrom(config_sock, buf, sizeof(buf), 0,
					    (struct sockaddr *)&src_addr, &addr_len);
		if (received <= 0) {
			continue;
		}

		/* 记录配置端口发送方地址 (回复路由用) */
		config_remote_addr = src_addr;

		uint8_t cmd = buf[0];
		const uint8_t *cmd_data = buf + 1;
		size_t cmd_len = received - 1;

#ifdef CONFIG_UDP_FW_REPLY_BCAST_RESTRICT
		/* 跨子网/广播接收的命令: 仅处理放行的网络发现命令 (如 GET/SET IP),
		 * 其余直接丢弃, 不执行任何操作也不回复, 避免误触发配置/固件升级 */
		if (!is_same_subnet(src_addr.sin_addr) && !bcast_cmd_allowed(cmd)) {
			LOG_DBG("drop cross-subnet cmd 0x%02x", cmd);
			continue;
		}
#endif

		/* 固件升级命令: 内部处理 */
		if (handle_fw_cmd(cmd, cmd_data, cmd_len)) {
			continue;
		}

		/* 其他命令: 广播给所有已注册的应用回调; 若均未处理则告警 */
		bool handled = false;

		for (int i = 0; i < CONFIG_UDP_FW_UPGRADE_MAX_HANDLERS; i++) {
			if (handlers[i].cb &&
			    handlers[i].cb(cmd, cmd_data, cmd_len,
					   handlers[i].user_data)) {
				handled = true;
			}
		}
		if (!handled) {
			LOG_WRN("unhandled UDP cmd: 0x%02x", cmd);
		}
	}
}

K_THREAD_DEFINE(udp_fw_rx_thread_id, CONFIG_UDP_FW_RX_STACK_SIZE,
		udp_fw_rx_thread, NULL, NULL, NULL,
		CONFIG_UDP_FW_RX_PRIORITY, 0, SYS_FOREVER_MS);

/* ================================================================
 * 网络就绪后启动: 创建配置端口 socket + bind + 启动 RX 线程
 * 通过 net_mgmt NET_EVENT_IF_UP 等待接口 up, 适配各应用网络初始化时序
 * (如 gateway_udp_test 在 main 里手动 net_init, 接口注册晚).
 * ================================================================ */
static bool udp_fw_started;

static void udp_fw_start(void)
{
	if (udp_fw_started) {
		return;
	}

	config_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (config_sock < 0) {
		LOG_ERR("config socket create failed: %d", errno);
		return;
	}

	/* 允许广播收发 */
	int broadcast = 1;

	setsockopt(config_sock, SOL_SOCKET, SO_BROADCAST,
		   (const char *)&broadcast, sizeof(broadcast));

	/* 绑定 INADDR_ANY: 上位机可广播配置命令到此端口 */
	struct sockaddr_in local_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_UDP_FW_CONFIG_PORT),
	};

	if (bind(config_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		LOG_ERR("config socket bind failed: %d", errno);
		close(config_sock);
		config_sock = -1;
		return;
	}

	udp_fw_started = true;
	LOG_INF("config port %d listening (fw upgrade ready)", CONFIG_UDP_FW_CONFIG_PORT);

	/* 启动 RX 线程 */
	k_thread_start(udp_fw_rx_thread_id);
}

/* NET_EVENT_IF_UP 回调: 接口 up 后启动配置端口 */
static struct net_mgmt_event_callback if_up_cb;

static void if_up_handler(struct net_mgmt_event_callback *cb,
			  uint64_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IF_UP) {
		udp_fw_start();
	}
}

/* SYS_INIT: 检查接口是否已 up, 是则直接启动; 否则注册 net_mgmt 等 IF_UP */
static int udp_fw_init(void)
{
	struct net_if *iface = net_if_get_default();

	/* 接口已 up (网络在 SYS_INIT 前就绑定好): 直接启动 */
	if (iface && net_if_oper_state(iface) == NET_IF_OPER_UP) {
		udp_fw_start();
		return 0;
	}

	/* 接口未 up: 注册 net_mgmt 等待 IF_UP 事件 (适用于 main 里手动 net_init 的应用) */
	net_mgmt_init_event_callback(&if_up_cb, if_up_handler, NET_EVENT_IF_UP);
	net_mgmt_add_event_callback(&if_up_cb);
	LOG_INF("waiting for network interface up...");
	return 0;
}

SYS_INIT(udp_fw_init, APPLICATION, CONFIG_UDP_FW_INIT_PRIORITY);

/* ================================================================
 * 对外接口实现
 * ================================================================ */
void udp_fw_set_app_handler(udp_fw_app_cmd_cb_t cb, void *user_data)
{
	if (cb == NULL) {
		return;
	}
	for (int i = 0; i < CONFIG_UDP_FW_UPGRADE_MAX_HANDLERS; i++) {
		if (handlers[i].cb == NULL) {
			handlers[i].cb = cb;
			handlers[i].user_data = user_data;
			return;
		}
	}
	LOG_WRN("handler array full (%d)", CONFIG_UDP_FW_UPGRADE_MAX_HANDLERS);
}

int udp_fw_remove_handler(udp_fw_app_cmd_cb_t cb)
{
	for (int i = 0; i < CONFIG_UDP_FW_UPGRADE_MAX_HANDLERS; i++) {
		if (handlers[i].cb == cb) {
			handlers[i].cb = NULL;
			handlers[i].user_data = NULL;
			return 0;
		}
	}
	return -ENOENT;
}

void udp_fw_allow_broadcast_cmd(uint8_t cmd)
{
#ifdef CONFIG_UDP_FW_REPLY_BCAST_RESTRICT
	for (size_t i = 0; i < bcast_allowed_cnt; i++) {
		if (bcast_allowed_cmds[i] == cmd) {
			return;
		}
	}
	if (bcast_allowed_cnt < CONFIG_UDP_FW_UPGRADE_MAX_BCAST_CMDS) {
		bcast_allowed_cmds[bcast_allowed_cnt++] = cmd;
	}
#else
	ARG_UNUSED(cmd);
#endif
}
