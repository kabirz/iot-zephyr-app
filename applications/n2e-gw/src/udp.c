/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 透传模块 - nRF24 与上位机之间的双向 UDP 转发
 * + 网络配置命令处理 (固件升级由 udp_fw_upgrade 库自管)
 *
 * 双端口架构:
 *   - 数据端口 (默认 9600, 本机监听):
 *       nRF24 → 上位机数据转发 (gw_udp_send) + 上位机 → nRF24 扫描仪数据透传
 *       转发目标固定为上位机 host_ip:host_port (默认 192.168.11.150:9602,
 *       可通过 UDP_CMD_SET_HOST 配置, 持久化), 不再广播/学习.
 *   - 配置端口 (默认 8600, 由 udp_fw_upgrade 库自管, Kconfig CONFIG_UDP_FW_CONFIG_PORT):
 *       库内部处理固件升级命令 (FW_START/DATA/END/VERSION/REBOOT),
 *       其余配置命令 (IP/RF24/HOST/发现) 通过回调分发到此模块.
 *
 * 配置命令帧格式: [cmd 1B][data...] (无魔数头, 配置端口只收命令)
 * 数据帧格式: [帧 ID 2B BE][payload]  (帧 ID 见 enum can_ids, 复用历史编号)
 *
 * UDP 数据转发目标:
 *   nRF24 → 上位机: gw_udp_send 固定单播到 gw_params.host_ip:host_port
 *                   (默认 192.168.11.150:9602, 通过 UDP_CMD_SET_HOST 配置, 持久化)
 *   上位机 → nRF24: 数据端口绑定 gw_params.data_port (默认 9600)
 *                   收到的扫描仪数据帧透传到 nRF24
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/sys/byteorder.h>
#include <n2e_gw.h>
#include <udp_fw_upgrade.h>

LOG_MODULE_REGISTER(gw_udp, LOG_LEVEL_INF);

/* 数据端口 socket (绑定 gw_params.data_port, 默认 9600). */
static int data_sock = -1;

/* ================================================================
 * 本机 IP 查询 + 上位机目标序列化
 * ================================================================ */

/* 取本机 live IPv4 地址 (DHCP 分配或静态配置的当前地址), 失败返回 NULL.
 * DHCP 模式下 gw_params.ip_addr 是旧静态值 (stale), 必须从 live interface 读. */
struct in_addr *gw_get_live_ipv4(void)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		return NULL;
	}
	return (struct in_addr *)net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
}

/* 上位机目标序列化: [ip 4B][port 2B BE] = 6B (SET_HOST 回复用) */
static void pack_host(uint8_t *buf)
{
	struct in_addr addr;

	if (inet_pton(AF_INET, gw_params.host_ip, &addr) != 1) {
		memset(buf, 0, 4);
	} else {
		memcpy(buf, &addr.s_addr, 4);
	}
	sys_put_be16(gw_params.host_port, buf + 4);
}

/* IP 有效性校验: 排除 0.0.0.0 / 127.x 环回 / 224+ 组播 / 255.255.255.255 广播.
 * 返回 true=合法单播地址. 用于 UDP_CMD_SET_IP 拒绝非法 IP. */
static bool ip_is_valid(struct in_addr addr)
{
	uint32_t a = ntohl(addr.s_addr);
	uint8_t b0 = (uint8_t)(a >> 24);

	if (a == 0)          return false;  /* 0.0.0.0 */
	if (a == 0xFFFFFFFF) return false;  /* 255.255.255.255 */
	if (b0 == 127)       return false;  /* 环回 127.x.x.x */
	if (b0 >= 224)       return false;  /* 组播 224-239 + 保留 240-255 */
	return true;
}

/* 数据端口发送: nRF24 数据 → 上位机. 固定单播到 host_ip:host_port (可配). */
void gw_udp_send(const uint8_t *data, size_t len)
{
	if (data_sock < 0 || len == 0) {
		return;
	}
	/* 链路 down 时不转发 nRF24 数据到上位机 (网线断开/PHY 未就绪时避免无效 sendto) */
	if (!gw_net_link_up) {
		return;
	}

	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port = htons(gw_params.host_port),
	};

	if (inet_pton(AF_INET, gw_params.host_ip, &dst.sin_addr) != 1) {
		LOG_WRN("invalid host ip %s, drop", gw_params.host_ip);
		return;
	}

	sendto(data_sock, data, len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* ================================================================
 * 配置命令处理 (应用回调, 由 udp_fw_upgrade 库 RX 线程调用)
 * 处理业务命令: SET_IP (0x10) / GET_NET (0x11) / SET/GET_RF24 (0x12/0x13) /
 * SET_HOST (0x14) / DISCOVER (0x15);
 * 固件升级及版本/重启命令 (0x01-0x05) 由库内部处理, 不会到达此处.
 * 回复通过 udp_fw_reply 发送 (库自管 socket + 回复路由).
 * ================================================================ */
static bool app_cmd_handler(uint8_t cmd, const uint8_t *cmd_data, size_t cmd_len,
			    void *user_data)
{
	switch (cmd) {
	case UDP_CMD_SET_IP: {
		/* 设置静态 IP: [ip 4B] → 回 [1B: 1=成功/0=失败].
		 * 仅静态模式生效 (DHCP 模式 IP 由服务器分配, 拒绝并回 0).
		 * IP 有效性: 排除 0.0.0.0 / 127.x / 224+ / 255.255.255.255.
		 * 持久化, 重启生效 (掩码固定 /24, 网关派生, 见 main.c net_init). */
		uint8_t ok = 0;

		if (cmd_len >= 4 && !gw_params.use_dhcp) {
			struct in_addr addr;

			memcpy(&addr.s_addr, cmd_data, 4);
			if (ip_is_valid(addr)) {
				inet_ntop(AF_INET, &addr, gw_params.ip_addr, sizeof(gw_params.ip_addr));
				persist_save_network_config();
				LOG_INF("UDP set ip: %s (reboot to apply)", gw_params.ip_addr);
				ok = 1;
			} else {
				LOG_WRN("UDP set ip: rejected invalid");
			}
		} else if (cmd_len >= 4 && gw_params.use_dhcp) {
			LOG_WRN("UDP set ip: rejected (DHCP mode)");
		}
		udp_fw_reply(cmd, &ok, sizeof(ok));
		return true;
	}

	case UDP_CMD_GET_NET: {
		/* 查询网络参数: (空) → [data_port 2B][host_ip 4B][host_port 2B] = 8B.
		 * 配置端口不在响应中返回 (由 UDP_CMD_DISCOVER 发现时带出). */
		uint8_t buf[8];
		struct in_addr host;

		sys_put_be16(gw_params.data_port, buf);
		if (inet_pton(AF_INET, gw_params.host_ip, &host) == 1) {
			memcpy(buf + 2, &host.s_addr, 4);
		} else {
			memset(buf + 2, 0, 4);
		}
		sys_put_be16(gw_params.host_port, buf + 6);
		udp_fw_reply(cmd, buf, sizeof(buf));
		return true;
	}

	case UDP_CMD_SET_RF24: {
		/* 设置 RF24 地址: [addr 5B]. 信道固定 RF24_FIXED_CH=1, 不可配.
		 * 回复: 设置后的 5B (回显) */
		if (cmd_len >= RF24_ADDR_LEN) {
			memcpy(gw_params.rf24_addr, cmd_data, RF24_ADDR_LEN);

			LOG_INF("UDP set rf24 addr=%02x%02x%02x%02x%02x", gw_params.rf24_addr[0],
				gw_params.rf24_addr[1], gw_params.rf24_addr[2], gw_params.rf24_addr[3],
				gw_params.rf24_addr[4]);
			persist_save_rf24_config();
			gw_rf24_set_config(gw_params.rf24_addr);

			uint8_t resp[RF24_ADDR_LEN];

			memcpy(resp, gw_params.rf24_addr, RF24_ADDR_LEN);
			udp_fw_reply(cmd, resp, sizeof(resp));
		}
		return true;
	}

	case UDP_CMD_GET_RF24: {
		/* 查询 RF24 地址: (空) → [addr 5B]. 信道固定为 1, 不在帧中返回. */
		uint8_t buf[RF24_ADDR_LEN];

		memcpy(buf, gw_params.rf24_addr, RF24_ADDR_LEN);
		udp_fw_reply(cmd, buf, sizeof(buf));
		return true;
	}

	case UDP_CMD_SET_HOST: {
		/* 设置上位机目标: [host ip 4B][port 2B BE] = 6B. 持久化, 即时生效.
		 * gw_udp_send 每次都从 gw_params.host_ip/host_port 重新解析, 无需重建 socket.
		 * 回复: 设置后的 6B (回显) */
		if (cmd_len >= 6) {
			struct in_addr addr;

			memcpy(&addr.s_addr, cmd_data, 4);
			inet_ntop(AF_INET, &addr, gw_params.host_ip, sizeof(gw_params.host_ip));
			gw_params.host_port = sys_get_be16(cmd_data + 4);

			LOG_INF("UDP set host: ip=%s port=%d", gw_params.host_ip,
				gw_params.host_port);
			persist_save_network_config();

			uint8_t resp[6];
			pack_host(resp);
			udp_fw_reply(cmd, resp, sizeof(resp));
		}
		return true;
	}

	case UDP_CMD_DISCOVER: {
		/* 广播发现: (空) → [ip 4B][config_port 2B] = 6B.
		 * IP 取自 live interface (拿不到则回退 ip_addr);
		 * config_port 取自编译期 Kconfig 宏 CONFIG_UDP_FW_CONFIG_PORT.
		 * 上位机广播发现后即可获知本机 IP + 配置端口, 用于后续定向通信. */
		uint8_t buf[6];
		struct in_addr addr;
		struct in_addr *live = gw_get_live_ipv4();

		if (live) {
			memcpy(buf, &live->s_addr, 4);
		} else if (inet_pton(AF_INET, gw_params.ip_addr, &addr) == 1) {
			memcpy(buf, &addr.s_addr, 4);
		} else {
			memset(buf, 0, 4);
		}
		sys_put_be16(CONFIG_UDP_FW_CONFIG_PORT, buf + 4);
		udp_fw_reply(cmd, buf, sizeof(buf));
		return true;
	}

	default:
		return false;
	}
}

/* ================================================================
 * 数据端口接收线程 (绑定 gw_params.data_port, 默认 9600)
 * 收到上位机扫描仪数据帧 → 透传到 nRF24; 同时学习 data_remote_addr
 * (nRF24 数据转发目标, 见 gw_udp_send)
 * ================================================================ */
static void udp_data_rx_thread(void)
{
	data_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (data_sock < 0) {
		LOG_ERR("data socket create failed: %d", errno);
		return;
	}

	struct sockaddr_in local_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(gw_params.data_port),
	};

	if (bind(data_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		LOG_ERR("data socket bind failed: %d", errno);
		close(data_sock);
		data_sock = -1;
		return;
	}

	LOG_INF("data port %d listening", gw_params.data_port);

	static uint8_t buf[512];

	while (1) {
		struct sockaddr_in src_addr;
		socklen_t addr_len = sizeof(src_addr);

		ssize_t received = recvfrom(data_sock, buf, sizeof(buf), 0,
					    (struct sockaddr *)&src_addr, &addr_len);
		if (received <= 0) {
			continue;
		}

		if (received >= 2) {
			uint16_t can_id = sys_get_be16(buf);

			if (can_id == OVERBREAK_LASER || can_id == COORD_XY ||
			    can_id == COORD_Z) {
				gw_rf24_send(can_id, buf + 2, received - 2);
				LOG_DBG("UDP->nRF24: id=0x%03x len=%zd", can_id,
					received - 2);
			}
		}
	}
}

K_THREAD_DEFINE(thread_udp_data_rx, CONFIG_N2E_GW_DATA_RX_STACK, udp_data_rx_thread, NULL, NULL,
		NULL, CONFIG_N2E_GW_DATA_RX_PRIORITY, 0, 0);

/* ================================================================
 * 初始化: 注册配置命令回调 (固件升级库 SYS_INIT 自管配置端口 socket)
 * ================================================================ */
static int gw_udp_init(void)
{
	udp_fw_set_app_handler(app_cmd_handler, NULL);
	/* 仅 DISCOVER 允许跨子网广播接收+回复 (用于设备发现);
	 * 其余命令广播接收时静默丢弃 (避免误触发配置/固件升级).
	 * GET_NET/SET_IP 需定向发送 (上位机先 DISCOVER 拿到设备 IP 后再单播). */
	udp_fw_allow_broadcast_cmd(UDP_CMD_DISCOVER);
	return 0;
}

SYS_INIT(gw_udp_init, APPLICATION, CONFIG_N2E_GW_UDP_INIT_PRIORITY);
