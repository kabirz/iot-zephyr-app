/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#ifndef CONFIG_FLASH_SIZE
#define CONFIG_FLASH_SIZE 0x1000
#endif

int main(void)
{
	printk("build time: %s-%s, board: %s, system clk: %dMHz, flash size: %dKB, version: %s\n",
	 __DATE__, __TIME__, CONFIG_BOARD_TARGET, CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000,
	 CONFIG_FLASH_SIZE, APP_VERSION_STRING);

	return 0;
}
