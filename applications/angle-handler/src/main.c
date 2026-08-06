/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 主入口 + 主循环 (休眠/唤醒事件驱动, 10 分钟无操作自动休眠)
 */

#include <zephyr/app_version.h>
#include <fw_gitver.h>
#ifndef CONFIG_FLASH_SIZE
#define CONFIG_FLASH_SIZE 0x1000
#endif
#include <zephyr/logging/log.h>
#include <common.h>
#include <mod-can.h>
#include <display.h>
#include <mod-gpio.h>
#include <rf24.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(main_app, LOG_LEVEL_INF);

#define ACTIVITY_TIMEOUT_MS (10 * 60 * 1000) /* 10 minutes */

volatile uint32_t last_activity_time;
extern int adc_init_channels(void);

int main(void)
{
	LOG_INF("build time: %s-%s", __DATE__, __TIME__);
	LOG_INF("board: %s, system clk: %dMHz", CONFIG_BOARD_TARGET,
		CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / MHZ(1));
	LOG_INF("flash size: %dKB, ram size: %dKB", CONFIG_FLASH_SIZE, CONFIG_SRAM_SIZE);
	LOG_INF("version: v%d.%d.%d_%s", APP_VERSION_MAJOR, APP_VERSION_MINOR,
		APP_PATCHLEVEL, FW_GIT_VERSION);

	last_activity_time = k_uptime_get_32();
	adc_init_channels();
	gpio_init();

	settings_load();

	connect_switch(global_params.connect_type);

	while (1) {
		if (atomic_get(&global_params.sleeping)) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}

		if ((k_uptime_get_32() - last_activity_time) > ACTIVITY_TIMEOUT_MS) {
			system_sleep();
			LOG_INF("system entering sleep (inactivity timeout)");
			continue;
		}

		k_sleep(K_MSEC(500));
	}

	return 0;
}
