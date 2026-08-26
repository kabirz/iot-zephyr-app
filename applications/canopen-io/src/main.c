/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * canopen-io: 纯 CANopen IO 计测节点 (io-edge-hub 的 CANopen 版)
 *
 *   - CANopenNode v4 栈: canopennode_init + 1ms canopennode_process
 *   - storage: OD 持久化 (0x1010/0x1011, 两组条目)
 *   - LEDs: CiA 303-3 状态指示
 *   - 首轮 init 后强制一次通信复位, 让 storage 加载的参数生效
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/app_version.h>

#include "canopennode.h"
#include "OD.h"
#include "fw_gitver.h"
#include "app_od.h"
#include "fw_download.h"

#if defined(CONFIG_CANOPENNODE_STORAGE)
#include <zephyr/settings/settings.h>
#include "storage/CO_storage.h"
#include "canopen_storage.h"
#endif

#if defined(CONFIG_CANOPENNODE_LEDS)
#include "canopen_leds.h"
#endif

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define MAIN_LOOP_US        1000
#define CANOPEN_BITRATE_KBPS 250

int main(void)
{
	CO_NMT_reset_cmd_t reset;

	LOG_INF("canopen-io v%d.%d.%d_%s start (node_id=%d)",
		APP_VERSION_MAJOR, APP_VERSION_MINOR,
		APP_PATCHLEVEL, FW_GIT_VERSION,
		CONFIG_CANOPEN_NODE_ID);

	bool first_boot = true;

#if defined(CONFIG_CANOPENNODE_STORAGE)
	/* settings/FCB 后端必须先于 canopen_storage_init 初始化, 否则
	 * 0x1010 save 与启动加载都会静默失败 (写不进 FCB, 读不到数据) */
	if (settings_subsys_init() != 0) {
		LOG_ERR("settings subsystem init failed, OD persistence disabled");
	}
#endif

	while (1) {
		if (canopennode_init(CONFIG_CANOPEN_NODE_ID,
				     CANOPEN_BITRATE_KBPS) != 0) {
			LOG_ERR("canopennode_init failed, retry in 1s");
			k_sleep(K_SECONDS(1));
			continue;
		}

#if defined(CONFIG_CANOPENNODE_STORAGE)
		/* 两组持久化条目: 0x1010:1 通信参数, 0x1010:2 应用参数 (0x2004) */
		static CO_storage_t storage;
		static CO_storage_entry_t storage_entries[] = {
			{ .addr = &OD_PERSIST_COMM, .len = sizeof(OD_PERSIST_COMM),
			  .subIndexOD = 1, .attr = CO_storage_cmd | CO_storage_restore,
			  .addrNV = NULL },
			{ .addr = &OD_PERSIST_APP, .len = sizeof(OD_PERSIST_APP),
			  .subIndexOD = 2, .attr = CO_storage_cmd | CO_storage_restore,
			  .addrNV = NULL },
		};
		uint32_t storage_init_error = 0;

		if (canopen_storage_init(&storage, CO->CANmodule, OD,
					 storage_entries,
					 ARRAY_SIZE(storage_entries),
					 &storage_init_error) != 0) {
			LOG_WRN("storage init failed (entry %u)",
				storage_init_error);
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
