/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 硬件看门狗 (STM32 独立看门狗 IWDG)
 * 30 秒超时, 由 main 主循环周期喂狗; 主循环冻结则系统复位.
 * fs_littlefs.c mkfs 擦整个 15MB NOR 期间事件型喂狗 (此时 main 尚未运行).
 * 30s 窗口需容纳外部 SPI NOR 擦除耗时 (LittleFS mkfs 擦 15MB 可达数十秒).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include "watchdog.h"

LOG_MODULE_REGISTER(io_wdt, LOG_LEVEL_INF);

#define WDG_TIMEOUT_MS 30000

static const struct device *const wdt_dev = DEVICE_DT_GET(DT_NODELABEL(iwdg));
/* -1: watchdog_init 完成前 feed 为 no-op (housekeeping 线程早于 SYS_INIT 运行) */
static int wdt_channel = -1;

int watchdog_init(void)
{
	if (!device_is_ready(wdt_dev)) {
		LOG_ERR("IWDG device not ready");
		return -ENODEV;
	}

	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0, .max = WDG_TIMEOUT_MS},
		.callback = NULL,
	};

	wdt_channel = wdt_install_timeout(wdt_dev, &cfg);
	if (wdt_channel < 0) {
		LOG_ERR("wdt_install_timeout failed: %d", wdt_channel);
		return wdt_channel;
	}

	int rc = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);

	if (rc) {
		LOG_ERR("wdt_setup failed: %d", rc);
		return rc;
	}

	LOG_INF("IWDG started (%dms)", WDG_TIMEOUT_MS);
	return 0;
}

void watchdog_feed(void)
{
	if (wdt_channel >= 0) {
		wdt_feed(wdt_dev, wdt_channel);
	}
}

SYS_INIT(watchdog_init, APPLICATION, CONFIG_CANOPEN_IO_INIT_PRIORITY_WATCHDOG);
