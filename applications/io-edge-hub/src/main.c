/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * io_edge-hub 主入口
 *   - MAC 从 STM32 UID 派生 (OUI 00:08:DC)
 *   - 静态 IP (从 holding_reg 读取, settings 已加载)
 *   - NET_EVENT_IF_UP/DOWN 处理 (断连时清零 DO, 拒绝新连接)
 *   - 状态 LED 心跳 + 延迟重启
 *   - 栈溢出保护 (k_sys_fatal_error_handler -> warm reboot)
 *
 * 应用层各模块 (Modbus/FTP/UDP/CAN/DI/AI/历史) 通过 SYS_INIT 或
 * K_THREAD_DEFINE 自启动, main 仅负责网络与状态指示。
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/app_version.h>
#include <fw_gitver.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <init.h>
#include "watchdog.h"

#ifndef CONFIG_FLASH_SIZE
#define CONFIG_FLASH_SIZE 0x1000
#endif
#ifndef CONFIG_SRAM_SIZE
#define CONFIG_SRAM_SIZE 0x1000
#endif

LOG_MODULE_REGISTER(io_main, LOG_LEVEL_INF);

/* 网络链路就绪信号量: IF_UP 唤醒 main */
static K_SEM_DEFINE(net_link_sem, 0, 1);
static volatile bool net_link_up;

/* 延迟重启标志 (改参数后刷新日志再重启) */
static volatile bool reboot_pending;

bool net_link_is_up(void)
{
	return net_link_up;
}

void set_reboot_status(bool en)
{
	reboot_pending = en;
}

bool get_reboot_status(void)
{
	return reboot_pending;
}

/* ================================================================
 * 网络事件: IF_UP 唤醒 main; IF_DOWN 清零 DO + 标记链路断开
 * ================================================================
 * Zephyr net_mgmt mask 按 layer 精确匹配, IF_UP/IF_DOWN 同属 L2 可共用。
 */
static void net_if_event_handler(uint64_t mgmt_event, struct net_if *iface,
				 void *info, size_t info_length, void *user_data)
{
	ARG_UNUSED(info);
	ARG_UNUSED(info_length);
	ARG_UNUSED(user_data);
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_IF_UP) {
		LOG_INF("net link up");
		net_link_up = true;
		k_sem_give(&net_link_sem);
	} else if (mgmt_event == NET_EVENT_IF_DOWN) {
		LOG_WRN("net link down");
		net_link_up = false;
		/* 工业安全: 链路断开立即清零所有 DO 输出 */
		update_holding_reg(HOLDING_DO_IDX, 0);
		mb_set_do(0);
	}
}

NET_MGMT_REGISTER_EVENT_HANDLER(net_if_handler_cb,
				NET_EVENT_IF_UP | NET_EVENT_IF_DOWN,
				net_if_event_handler, NULL);

/* ================================================================
 * MAC 派生: STM32 96-bit UID 折叠为唯一 MAC (前 3B = Wiznet OUI)
 * ================================================================ */
static void derive_mac_from_uid(uint8_t *mac)
{
	static const uint8_t oui[3] = { 0x00, 0x08, 0xDC };
	uint8_t uid[12];
	ssize_t n = hwinfo_get_device_id(uid, sizeof(uid));

	mac[0] = oui[0];
	mac[1] = oui[1];
	mac[2] = oui[2];

	if (n >= (ssize_t)sizeof(uid)) {
		mac[3] = uid[0] ^ uid[3] ^ uid[6] ^ uid[9];
		mac[4] = uid[1] ^ uid[4] ^ uid[7] ^ uid[10];
		mac[5] = uid[2] ^ uid[5] ^ uid[8] ^ uid[11];
	} else {
		mac[3] = 0x01;
		mac[4] = 0x02;
		mac[5] = 0x03;
	}
}

/* ================================================================
 * 网络初始化: 唯一 MAC + 静态 IP (从 holding_reg)
 * ================================================================ */
static int net_init(void)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		LOG_ERR("no network interface");
		return -ENODEV;
	}

	/* MAC: 接口自动 up 已由 CONFIG_ETH_NET_IF_NO_AUTO_START 关闭,
	 * 接口 admin down, SET_MAC_ADDRESS 可直接成功, 之后 net_if_up。 */
	uint8_t mac[NET_ETH_ADDR_LEN];
	struct ethernet_req_params params = { 0 };

	derive_mac_from_uid(mac);
	memcpy(params.mac_address.addr, mac, NET_ETH_ADDR_LEN);
	if (net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface,
		     &params, sizeof(params)) != 0) {
		LOG_WRN("set MAC failed, using DT default");
	} else {
		LOG_INF("MAC (UID): %02x:%02x:%02x:%02x:%02x:%02x",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	}

	/* 静态 IP: 从 holding_reg 组装; 掩码固定 /24; 网关 = IP 末段改 1 */
	struct in_addr addr, mask, gw;
	uint8_t *a = (uint8_t *)&addr.s_addr;

	a[0] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET1_IDX);
	a[1] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET2_IDX);
	a[2] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET3_IDX);
	a[3] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET4_IDX);

	mask.s_addr = htonl(0xFFFFFF00);
	memcpy(&gw, &addr, sizeof(addr));
	((uint8_t *)&gw.s_addr)[3] = 1;

	net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
	net_if_ipv4_set_gw(iface, &gw);

	char ip_str[INET_ADDRSTRLEN];

	inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
	LOG_INF("IP: %s/24", ip_str);

	net_if_up(iface);
	return 0;
}

/* 栈溢出保护: K_ERR_STACK_CHK_FAIL -> warm reboot */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *pEsf)
{
	ARG_UNUSED(pEsf);

	if (reason == K_ERR_STACK_CHK_FAIL) {
		LOG_ERR("Stack overflow detected, rebooting");
		sys_reboot(SYS_REBOOT_WARM);
	}

	while (true) {
		/* 其他 fatal: 停机 */
	}
}

int main(void)
{
	LOG_INF("build time: %s %s", __DATE__, __TIME__);
	LOG_INF("board: %s, clk: %dMHz", CONFIG_BOARD_TARGET,
		CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / MHZ(1));
	LOG_INF("flash: %dKB, ram: %dKB", CONFIG_FLASH_SIZE, CONFIG_SRAM_SIZE);
	LOG_INF("version: v%d.%d.%d_%s", APP_VERSION_MAJOR, APP_VERSION_MINOR,
		APP_PATCHLEVEL, FW_GIT_VERSION);

	net_init();

	/* 等待 PHY 链路 up (带超时兜底, 网线未插时不永久阻塞) */
	struct net_if *iface = net_if_get_default();

	if (iface != NULL && net_if_oper_state(iface) != NET_IF_OPER_UP) {
		if (k_sem_take(&net_link_sem, K_SECONDS(5)) != 0) {
			LOG_WRN("net link up timeout, continue anyway");
		}
	}

	LOG_INF("io-edge-hub ready");

#ifdef CONFIG_IO_WEB
	/* Web 服务在链路就绪后启动 (SYS_INIT 阶段 iface 未 up) */
	io_web_start();
#endif

	/* 状态 LED 心跳: 300ms on / 2700ms off */
	static const struct gpio_dt_spec status_led =
		GPIO_DT_SPEC_GET(DT_ALIAS(mcuboot_led0), gpios);

	if (gpio_is_ready_dt(&status_led)) {
		gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
	}

	while (1) {
		watchdog_feed();

		if (gpio_is_ready_dt(&status_led)) {
			gpio_pin_set_dt(&status_led, 1);
			k_msleep(300);
			gpio_pin_set_dt(&status_led, 0);
			k_msleep(2700);
		} else {
			k_msleep(1000);
		}

		if (get_reboot_status()) {
			LOG_INF("delayed reboot");
			history_sync();
#ifdef CONFIG_LOG
			while (log_process()) {
			}
#endif
			k_msleep(1000);
			sys_reboot(SYS_REBOOT_COLD);
		}
	}

	return 0;
}
