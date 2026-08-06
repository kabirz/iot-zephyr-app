/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * n2e-gw 主入口 - 数据中转网关
 * 接收 angle-handler 的 nRF24 数据，通过 W5500 UDP 转发给上位机
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/settings/settings.h>
#include <n2e_gw.h>
#include <zephyr/app_version.h>
#include <fw_gitver.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/drivers/hwinfo.h>
#ifndef CONFIG_FLASH_SIZE
#define CONFIG_FLASH_SIZE 0x1000
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(n2e_gw, LOG_LEVEL_INF);

n2e_gw_params_t gw_params;

/* ================================================================
 * 网络链路就绪事件
 * ================================================================
 * W5500 驱动在 PHY 检测到载波 (PHYCFGR.LNK) 后调 net_eth_carrier_on →
 * net_if_carrier_on (置 NET_IF_LOWER_UP) → update_operational_state: 接口
 * admin up 且 carrier ok 时 oper state → UP → notify_iface_up 发出
 * NET_EVENT_IF_UP. 此处用信号量捕获该事件, 替代原先固定 k_msleep(500) 的盲
 * 等待 (PHY 自动协商耗时不定, 网线未插时延时更毫无意义).
 * ================================================================ */
static K_SEM_DEFINE(net_link_sem, 0, 1);

/* 网络链路状态: IF_UP 置 true, IF_DOWN 置 false. gw_udp_send 据此决定是否转发. */
volatile bool gw_net_link_up;

/* 网络事件回调 (静态注册, 编译时进 section, 不受运行时注册时序影响).
 * 注意: Zephyr net_mgmt 的 mask 匹配按 layer 精确相等, 不同 layer 的事件
 * (IF_UP=L2, IPV4_ADDR_ADD=L3) 不能 OR 在同一个 mask 里, 否则全部匹配失败.
 * 故分成两个独立 handler 注册. IF_UP/IF_DOWN 同 layer 可共用一个. */

/* IF_UP/IF_DOWN: carrier 状态变化 → 唤醒 main + 启动 DHCP + 维护 gw_net_link_up. */
static void net_if_event_handler(uint64_t mgmt_event, struct net_if *iface,
				 void *info, size_t info_length, void *user_data)
{
	ARG_UNUSED(info);
	ARG_UNUSED(info_length);
	ARG_UNUSED(user_data);

	if (mgmt_event == NET_EVENT_IF_UP) {
		LOG_INF("net link up");
		gw_net_link_up = true;
		k_sem_give(&net_link_sem);
		/* DHCP 模式: carrier up 后才启动 DHCP 客户端 (启动早了 Discover 发不出) */
		if (gw_params.use_dhcp) {
			LOG_INF("Starting DHCPv4 client...");
			net_dhcpv4_start(iface);
		}
	} else if (mgmt_event == NET_EVENT_IF_DOWN) {
		LOG_WRN("net link down");
		gw_net_link_up = false;
	}
}

/* ADDR_ADD: IPv4 地址分配完成 → 打印 IP/掩码/网关 (DHCP 模式额外打印租期). */
static void net_ipv4_event_handler(uint64_t mgmt_event, struct net_if *iface,
				   void *info, size_t info_length, void *user_data)
{
	ARG_UNUSED(info);
	ARG_UNUSED(info_length);
	ARG_UNUSED(user_data);

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}
	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		if (iface->config.ip.ipv4->unicast[i].ipv4.addr_type !=
		    NET_ADDR_DHCP && iface->config.ip.ipv4->unicast[i].ipv4.addr_type !=
		    NET_ADDR_MANUAL) {
			continue;
		}
		char buf[NET_IPV4_ADDR_LEN];

		net_addr_ntop(AF_INET,
			      &iface->config.ip.ipv4->unicast[i].ipv4.address.in_addr,
			      buf, sizeof(buf));
		LOG_INF("IPv4 address: %s", buf);
		net_addr_ntop(AF_INET, &iface->config.ip.ipv4->unicast[i].netmask,
			      buf, sizeof(buf));
		LOG_INF("IPv4 netmask: %s", buf);
		net_addr_ntop(AF_INET, &iface->config.ip.ipv4->gw, buf, sizeof(buf));
		LOG_INF("IPv4 gateway: %s", buf);
		if (iface->config.ip.ipv4->unicast[i].ipv4.addr_type == NET_ADDR_DHCP) {
			LOG_INF("DHCP lease time: %u seconds", iface->config.dhcpv4.lease_time);
		}
		break;
	}
}

NET_MGMT_REGISTER_EVENT_HANDLER(net_if_handler_cb,
				NET_EVENT_IF_UP | NET_EVENT_IF_DOWN,
				net_if_event_handler, NULL);
NET_MGMT_REGISTER_EVENT_HANDLER(net_ipv4_handler_cb, NET_EVENT_IPV4_ADDR_ADD,
				net_ipv4_event_handler, NULL);

/* ================================================================
 * MAC 派生: 用 STM32 96-bit UID 生成唯一 MAC, 覆盖设备树默认值
 * ================================================================
 * 设备树 local-mac-address 是固定值 (00:08:DC:01:02:03), 同型号多块板会冲突.
 * 这里前 3B 沿用 Wiznet OUI (00:08:DC), 末 3B 由 hwinfo 读出的 12B UID 折叠
 * 而来, 保证每块板有不同 MAC. SET_MAC_ADDRESS 要求接口 admin down (在 net_if_up
 * 前调用), 同时会更新 W5500 SHAR 寄存器 + net_if link_addr.
 */
static void derive_mac_from_uid(uint8_t *mac)
{
	static const uint8_t oui[3] = { 0x00, 0x08, 0xDC };
	uint8_t uid[12];
	ssize_t n = hwinfo_get_device_id(uid, sizeof(uid));

	mac[0] = oui[0];
	mac[1] = oui[1];
	mac[2] = oui[2];

	if (n >= (ssize_t)sizeof(uid)) {
		/* 12B UID 折叠成 3B: 每 4B 异或到 1B, 充分利用全部熵 */
		mac[3] = uid[0] ^ uid[3] ^ uid[6] ^ uid[9];
		mac[4] = uid[1] ^ uid[4] ^ uid[7] ^ uid[10];
		mac[5] = uid[2] ^ uid[5] ^ uid[8] ^ uid[11];
	} else {
		/* 取不到 UID: 回退设备树默认后缀, 至少能与 OUI 拼成合法 MAC */
		mac[3] = 0x01;
		mac[4] = 0x02;
		mac[5] = 0x03;
	}
}

/* ================================================================
 * 网络初始化 (静态 IP / DHCP)
 * ================================================================ */
static int net_init(void)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		LOG_ERR("No network interface found");
		return -ENODEV;
	}

	/* 用 UID 派生唯一 MAC 覆盖设备树默认值. 接口自动 up 已通过
	 * CONFIG_ETH_NET_IF_NO_AUTO_START 关闭 (net_if_post_init 不会自动 up ethernet
	 * 接口), 故此时接口处于 admin down, SET_MAC_ADDRESS 可直接成功. 之后 net_if_up
	 * 手动拉起. 失败则沿用设备树 MAC. */
	uint8_t mac[NET_ETH_ADDR_LEN];
	struct ethernet_req_params params;

	derive_mac_from_uid(mac);
	memcpy(params.mac_address.addr, mac, NET_ETH_ADDR_LEN);
	if (net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface,
		     &params, sizeof(params)) != 0) {
		LOG_WRN("set MAC from UID failed, using DT default");
	} else {
		LOG_INF("MAC (from UID): %02x:%02x:%02x:%02x:%02x:%02x",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	}

	/* 事件回调已静态注册 (NET_MGMT_REGISTER_EVENT_HANDLER). */
	if (gw_params.use_dhcp) {
		/* DHCP 模式: 只 up 接口, DHCP 客户端在 IF_UP 回调里启动
		 * (carrier 就绪后再启动, 否则 Discover 发不出) */
		LOG_INF("DHCP mode, waiting for link up...");
		net_if_up(iface);
	} else {
		/* 静态模式: 掩码固定 /24; 网关 = IP 末段改 1 (a.b.c.1), 不存储 */
		struct in_addr addr, mask, gw;

		if (net_addr_pton(AF_INET, gw_params.ip_addr, &addr) < 0) {
			LOG_ERR("Invalid IP address: %s", gw_params.ip_addr);
			return -EINVAL;
		}
		mask.s_addr = htonl(0xFFFFFF00);
		memcpy(&gw, &addr, sizeof(gw));
		((uint8_t *)&gw.s_addr)[3] = 1;

		net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
		net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
		net_if_ipv4_set_gw(iface, &gw);

		/* 触发接口 administrative up: 幂等. oper state 由 (admin up + carrier) 决定,
		 * 是 NET_EVENT_IF_UP 的可靠触发点. */
		net_if_up(iface);

		char gw_str[NET_IPV4_ADDR_LEN];
		net_addr_ntop(AF_INET, &gw, gw_str, sizeof(gw_str));
		LOG_INF("Network: %s/24 gw %s", gw_params.ip_addr, gw_str);
	}
	return 0;
}

int main(void)
{
	LOG_INF("build time: %s-%s", __DATE__, __TIME__);
	LOG_INF("board: %s, system clk: %dMHz", CONFIG_BOARD_TARGET,
		CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / MHZ(1));
	LOG_INF("flash size: %dKB, ram size: %dKB", CONFIG_FLASH_SIZE, CONFIG_SRAM_SIZE);
	LOG_INF("version: v%d.%d.%d_%s", APP_VERSION_MAJOR, APP_VERSION_MINOR,
		APP_PATCHLEVEL, FW_GIT_VERSION);

	/* 初始化默认配置 */
	memset(gw_params.rf24_addr, 0, RF24_ADDR_LEN);
	strncpy(gw_params.ip_addr, N2E_GW_DEFAULT_IP, sizeof(gw_params.ip_addr) - 1);
	gw_params.data_port = N2E_GW_DATA_PORT_DEFAULT;
	gw_params.use_dhcp = GW_USE_DHCP_DEFAULT;
	strncpy(gw_params.host_ip, N2E_GW_HOST_DEFAULT_IP, sizeof(gw_params.host_ip) - 1);
	gw_params.host_port = N2E_GW_HOST_PORT_DEFAULT;
	gw_params.log = false;   /* nRF24 详细日志默认关 (shell: rf24 log 1 开启) */

	/* 加载持久化配置 (覆盖默认值) */
	gw_config_load();

	/* 初始化状态灯 (PA1 rf24 常亮收发闪, PA2 sys 灭, PA3 err 灭) */
	gw_led_init();

	/* 初始化各模块 */
	gw_rf24_init();

	net_init();

	/* 等待 PHY 链路 up: 若 net_init() 注册回调时链路已 up (oper state 已 UP)
	 * 则直接跳过; 否则等 NET_EVENT_IF_UP 信号量. 带超时兜底, 避免网线未插时
	 * 永久阻塞 (UDP 收发线程独立运行, 链路恢复后自然恢复转发). */
	struct net_if *iface = net_if_get_default();

	if (iface != NULL && net_if_oper_state(iface) != NET_IF_OPER_UP) {
		if (k_sem_take(&net_link_sem, K_SECONDS(5)) != 0) {
			LOG_WRN("net link up timeout, continue anyway");
		}
	}

	gw_params.running = true;
	LOG_INF("n2e-gw ready");

	/* 点亮系统灯 PA2 - 标志进入主循环 (系统就绪) */
	gw_led_sys_on();

	while (1) {
		k_sleep(K_MSEC(1000));
	}

	return 0;
}
