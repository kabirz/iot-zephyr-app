/*
 * Copyright (c) 2026 Titan Mini contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Titan Mini (Renesas RA8P1) 硬件 bringup:
 *  - UART shell 直接跑在调试串口上 (zephyr,shell-uart = uart1 / U18, 115200-8N1)
 *  - RGB 流水灯 (共阳低有效: G=P108, R=P109, B=P110) + LOG 心跳
 *  - kernel reboot 命令验证 CONFIG_REBOOT
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(titan_bringup, LOG_LEVEL_INF);

#define LED_FLOW_STEP_MS   300
#define LOG_HEARTBEAT_LOOPS 6

static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios), /* G P108 */
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios), /* R P109 */
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios), /* B P110 */
};

static void led_flow(void)
{
	int step = 0;
	int loop = 0;

	while (1) {
		for (int i = 0; i < ARRAY_SIZE(leds); i++) {
			gpio_pin_set_dt(&leds[i], i == step);
		}
		step = (step + 1) % ARRAY_SIZE(leds);

		if (step == 0 && ++loop >= LOG_HEARTBEAT_LOOPS) {
			loop = 0;
			LOG_INF("heartbeat: uptime %d s", (int)(k_uptime_get() / 1000));
		}

		k_sleep(K_MSEC(LED_FLOW_STEP_MS));
	}
}

K_THREAD_DEFINE(led_flow_tid, 2048, led_flow, NULL, NULL, NULL, 5, 0, 2000);

int main(void)
{
	int ret = 0;

	for (int i = 0; i < ARRAY_SIZE(leds); i++) {
		ret = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("LED %d config failed (%d)", i, ret);
		}
	}

	LOG_INF("Titan Mini (Renesas RA8P1) HW bringup");
	LOG_INF("debug UART: U18, TXD1=P707 RXD1=P706, 115200-8N1");
	LOG_INF("RGB flow: G(P108) -> R(P109) -> B(P110), %d ms/step", LED_FLOW_STEP_MS);
	LOG_INF("shell ready, try: kernel version | kernel reboot");

	return 0;
}
