/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * canopen-io 主入口 (CANopen 内核 + io-edge-hub 网络功能合并)
 *   - CANopenNode v4 栈: canopennode_init + 1ms canopennode_process
 *   - storage: OD 持久化 (0x1010/0x1011, 仅通信参数条目; 应用参数经
 *     modbus/ settings 命名空间持久化, 见 app_od.c)
 *   - LEDs: CiA 303-3 状态指示
 *   - 网络: MAC 从 STM32 UID 派生, 静态 IP (holding_reg), IF_UP/DOWN
 *     事件处理 (断连安全清零 DO)
 *   - housekeeping 线程: IWDG 喂狗 + 延迟重启处理
 *   - 栈溢出保护 (k_sys_fatal_error_handler -> warm reboot)
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/app_version.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/settings/settings.h>

#include "canopennode.h"
#include "OD.h"
#include "fw_gitver.h"
#include "app_od.h"
#include "fw_download.h"
#include "init.h"
#include "watchdog.h"

#if defined(CONFIG_CANOPENNODE_STORAGE)
#include "storage/CO_storage.h"
#include "canopen_storage.h"
#endif

#if defined(CONFIG_CANOPENNODE_LEDS)
#include "canopen_leds.h"
#endif

#ifndef CONFIG_FLASH_SIZE
#define CONFIG_FLASH_SIZE 0x1000
#endif
#ifndef CONFIG_SRAM_SIZE
#define CONFIG_SRAM_SIZE 0x1000
#endif

LOG_MODULE_REGISTER(io_main, LOG_LEVEL_INF);

#define MAIN_LOOP_US          1000
#define CANOPEN_BITRATE_KBPS  250
/* housekeeping 周期 100ms (喂狗 + 延迟重启检查) */
#define HOUSEKEEP_PERIOD_MS   100

#if defined(CONFIG_CANOPENNODE_STORAGE)
static CO_storage_t storage;
static CO_storage_entry_t storage_entries[] = {
	/* 仅通信参数条目 (0x1010:1); 应用参数改由 settings (modbus/)
	 * 持久化, 见 app_od.c 的 0x2004 桥接说明 */
	{ .addr = &OD_PERSIST_COMM, .len = sizeof(OD_PERSIST_COMM),
	  .subIndexOD = 1, .attr = CO_storage_cmd | CO_storage_restore,
	  .addrNV = NULL },
};
#endif

/* ================================================================
 * 网络链路状态 / 延迟重启标志
 * ================================================================ */
static K_SEM_DEFINE(net_link_sem, 0, 1);
static volatile bool net_link_up;
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
static void net_if_event_handler(uint64_t mgmt_event, struct net_if *iface, void *info,
				 size_t info_length, void *user_data)
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

NET_MGMT_REGISTER_EVENT_HANDLER(net_if_handler_cb, NET_EVENT_IF_UP | NET_EVENT_IF_DOWN,
				net_if_event_handler, NULL);

/* ================================================================
 * MAC 派生: STM32 96-bit UID 折叠为唯一 MAC (前 3B = Wiznet OUI)
 * ================================================================ */
static void derive_mac_from_uid(uint8_t *mac)
{
	static const uint8_t oui[3] = {0x00, 0x08, 0xDC};
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
	struct ethernet_req_params params = {0};

	derive_mac_from_uid(mac);
	memcpy(params.mac_address.addr, mac, NET_ETH_ADDR_LEN);
	if (net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface, &params, sizeof(params)) != 0) {
		LOG_WRN("set MAC failed, using DT default");
	} else {
		LOG_INF("MAC (UID): %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
			mac[3], mac[4], mac[5]);
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

/* ================================================================
 * housekeeping 线程: IWDG 喂狗 + 延迟重启
 *
 * main 运行 1ms 周期的 CANopen 主循环, 喂狗/重启移到此低优先级线程,
 * 保证 CANopen 时序不被打搅。状态指示交给 CiA 303-3 绿色 LED
 * (canopennode LEDs 模块按 NMT 状态驱动, 同为 PE7, 不重复控制)。
 * ================================================================ */
static void housekeeping_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		watchdog_feed();

		if (reboot_pending) {
			LOG_INF("delayed reboot");
			history_sync();
#ifdef CONFIG_LOG
			while (log_process()) {
			}
#endif
			k_msleep(500);
			sys_reboot(SYS_REBOOT_COLD);
		}

		k_msleep(HOUSEKEEP_PERIOD_MS);
	}
}

K_THREAD_DEFINE(housekeeping, 2048, housekeeping_thread, NULL, NULL, NULL, 10, 0, 0);

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

/* ================================================================
 * main: 网络 + Web 启动后进入 CANopen 主循环
 * ================================================================ */
int main(void)
{
	CO_NMT_reset_cmd_t reset;
	bool first_boot = true;

	LOG_INF("build time: %s %s", __DATE__, __TIME__);
	LOG_INF("version: v%d.%d.%d_%s (node_id=%d)", APP_VERSION_MAJOR, APP_VERSION_MINOR,
		APP_PATCHLEVEL, FW_GIT_VERSION, CONFIG_CANOPEN_NODE_ID);
	LOG_INF("board: %s, clk: %dMHz", CONFIG_BOARD_TARGET,
		CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / MHZ(1));
	LOG_INF("flash: %dKB, ram: %dKB", CONFIG_FLASH_SIZE, CONFIG_SRAM_SIZE);

#if defined(CONFIG_CANOPENNODE_STORAGE)
	/* settings/FCB 后端在 SYS_INIT(APPLICATION) 阶段已完成 subsys 初始化
	 * (见 src/settings/init.c), 这里兜底幂等调用一次 */
	if (settings_subsys_init() != 0) {
		LOG_ERR("settings subsystem init failed, OD persistence disabled");
	}
#endif

	net_init();

	/* 等待 PHY 链路 up (带超时兜底, 网线未插时不永久阻塞) */
	struct net_if *iface = net_if_get_default();

	if (iface != NULL && net_if_oper_state(iface) != NET_IF_OPER_UP) {
		if (k_sem_take(&net_link_sem, K_SECONDS(5)) != 0) {
			LOG_WRN("net link up timeout, continue anyway");
		}
	}

#ifdef CONFIG_CANOPEN_IO_WEB
	/* Web 服务在链路就绪后启动 (SYS_INIT 阶段 iface 未 up) */
	io_web_start();
#endif

	LOG_INF("canopen-io ready");

	while (1) {
		if (canopennode_init(CONFIG_CANOPEN_NODE_ID, CANOPEN_BITRATE_KBPS) != 0) {
			LOG_ERR("canopennode_init failed, retry in 1s");
			k_sleep(K_SECONDS(1));
			continue;
		}

#if defined(CONFIG_CANOPENNODE_STORAGE)
		/* 通信参数持久化条目 (0x1010:1) */
		uint32_t storage_init_error = 0;

		if (canopen_storage_init(&storage, CO->CANmodule, OD, storage_entries,
					 ARRAY_SIZE(storage_entries),
					 &storage_init_error) != 0) {
			LOG_WRN("storage init failed (entry %u)", storage_init_error);
		}
#endif

		app_od_init();
		fw_download_init();

#if defined(CONFIG_CANOPENNODE_LEDS)
		CO_LEDs_t leds;

		if (canopen_leds_init(&leds) != 0) {
			LOG_WRN("CANopen LED init failed");
		}
#endif

		/* 首轮: storage 已把持久化值覆盖进 OD, 再做一次完整的
		 * shutdown+init (等价通信复位), 让 PDO/心跳等用加载值重建 */
		if (first_boot) {
			first_boot = false;
			canopennode_shutdown();
			continue;
		}

		reset = CO_RESET_NOT;
		while (reset == CO_RESET_NOT) {
			reset = canopennode_process(MAIN_LOOP_US);
#if defined(CONFIG_CANOPENNODE_LEDS)
			canopen_leds_process(&leds, MAIN_LOOP_US,
					     CO_NMT_getInternalState(CO->NMT),
					     CO->em);
#endif
			k_sleep(K_USEC(MAIN_LOOP_US));
		}

		LOG_INF("CANopen reset: %s",
			reset == CO_RESET_COMM ? "COMM" : "APP");
		canopennode_shutdown();
	}

	return 0;
}
