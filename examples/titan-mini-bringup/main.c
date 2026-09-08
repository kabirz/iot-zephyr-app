/*
 * Copyright (c) 2026 Titan Mini contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Titan Mini (Renesas RA8P1) 硬件 bringup:
 *  - UART shell 直接跑在调试串口上 (zephyr,shell-uart = uart1 / U18, 115200-8N1)
 *  - LOG 心跳 + 绿灯闪烁 (P108)
 *  - kernel reboot 命令验证 CONFIG_REBOOT
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(titan_bringup, LOG_LEVEL_INF);

static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static void heartbeat(void)
{
	while (1) {
		gpio_pin_toggle_dt(&led_g);
		LOG_INF("heartbeat: uptime %d s", (int)(k_uptime_get() / 1000));
		k_sleep(K_SECONDS(2));
	}
}

K_THREAD_DEFINE(heartbeat_tid, 2048, heartbeat, NULL, NULL, NULL, 5, 0, 2000);

int main(void)
{
	int ret = gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);

	if (ret < 0) {
		LOG_ERR("LED config failed (%d)", ret);
	}

	LOG_INF("Titan Mini (Renesas RA8P1) HW bringup");
	LOG_INF("debug UART: U18, TXD1=P707 RXD1=P706, 115200-8N1");
	LOG_INF("shell ready, try: kernel version | kernel reboot");

	return 0;
}
