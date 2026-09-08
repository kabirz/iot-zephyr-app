/*
 * Copyright (c) 2026 Titan Mini contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Titan Mini (Renesas RA8P1) 硬件 bringup:
 *  - TX 验证: 串口心跳 + LED 闪烁 (绿灯 P108)
 *  - RX 验证: 输入一行回车, 原样回显
 * 调试串口: H1 (1=GND, 2=RXD2=P802, 3=TXD2=P801), 115200-8N1
 */

#include <zephyr/kernel.h>
#include <zephyr/console/console.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <stdio.h>

static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static void heartbeat(void)
{
	int i = 0;

	while (1) {
		gpio_pin_toggle_dt(&led_g);
		printk("heartbeat %d: uptime %d s\n", i++, (int)(k_uptime_get() / 1000));
		k_sleep(K_SECONDS(2));
	}
}

K_THREAD_DEFINE(heartbeat_tid, 2048, heartbeat, NULL, NULL, NULL, 5, 0, 2000);

int main(void)
{
	int ret;

	printk("\n");
	printk("========================================\n");
	printk("Titan Mini (Renesas RA8P1) HW bringup\n");
	printk("UART: SCI2 @ 115200-8N1, H1: TXD2=P801 RXD2=P802\n");
	printk("build: " __DATE__ " " __TIME__ "\n");
	printk("========================================\n");

	ret = gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("ERROR: LED config failed (%d)\n", ret);
	} else {
		printk("user LED (green) blinking\n");
	}

	printk("type a line + Enter to test RX\n");

	console_getline_init();

	while (1) {
		char *line = console_getline();

		printk("RX (%zu chars): %s\n", strlen(line), line);
	}

	return 0;
}
