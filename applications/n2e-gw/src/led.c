/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 三路状态灯管理
 *   PA1 (led_rf24): 2.4G 活动灯 - 平时常亮, 收发时以固定频率闪烁
 *   PA3 (led_err):  错误灯     - 开机自检闪一下 (150ms) 后熄灭; 栈溢出/hardfault/
 *                                关键硬件初始化失败点亮, 锁定不灭
 *   PA2 (led_sys):  系统灯     - 进入 main 循环前点亮
 *
 * 全部低电平亮 (GPIO_ACTIVE_LOW), 通过 DT 描述; 代码用 gpio_pin_set_dt 的逻辑电平
 * (1=亮, 0=灭), 不关心物理极性.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <n2e_gw.h>

LOG_MODULE_REGISTER(gw_led, LOG_LEVEL_INF);

/* DT alias 引用, 编译期校验引脚存在 */
static const struct gpio_dt_spec led_rf24 = GPIO_DT_SPEC_GET(DT_ALIAS(led_rf24), gpios);
static const struct gpio_dt_spec led_err  = GPIO_DT_SPEC_GET(DT_ALIAS(led_err), gpios);
static const struct gpio_dt_spec led_sys  = GPIO_DT_SPEC_GET(DT_ALIAS(led_sys), gpios);

static void led_blink_work_handler(struct k_work *work);

/* 2.4G 活动指示: 平时常亮, 收发时以固定频率闪烁 (亮/灭各半周期).
 * 用固定频率翻转而非跟随每次收发 —— 高速通信时收发间隔远小于人眼
 * 响应, 跟随式亮脉冲/灭窗都会被吞掉 (退化为常亮或常灭, 完全无感);
 * 固定 ~5Hz 翻转无论通信快慢都稳定可见. 收发停止超过超时后恢复常亮. */
#define RF24_BLINK_HALF_PERIOD_MS  100   /* 半周期: 亮100ms灭100ms → 5Hz */
#define RF24_ACTIVITY_TIMEOUT_MS   400   /* 无收发超过此值视为通信停止, 恢复常亮 */
static struct k_work_delayable led_blink_work;
static volatile uint32_t rf24_last_activity;
/* 闪烁周期是否在跑 (避免收发路径重复提交 work) */
static volatile bool blinking;

/* 错误灯锁定标志 - 一旦置位, 不再熄灭 */
static volatile bool error_latched;

/* 开机自检: 故障灯点亮 ERR_SELFTEST_MS 后熄灭 (仅指示 LED/GPIO 通路正常,
 * 不置 latch). 用 delayed work 非阻塞, 不拖慢后续初始化. */
#define ERR_SELFTEST_MS 150
static struct k_work_delayable err_selftest_work;

static void err_selftest_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	gpio_pin_set_dt(&led_err, 0);
}

/* ================================================================
 * 初始化: PA1/PA2/PA3 配置为输出, 默认全灭; 随后点亮 PA1 (常亮=就绪),
 * 故障灯做一次开机自检闪烁 (亮 150ms 后自动熄灭).
 * ================================================================ */
void gw_led_init(void)
{
	gpio_pin_configure_dt(&led_rf24, GPIO_OUTPUT_INACTIVE);  /* 初始灭 */
	gpio_pin_configure_dt(&led_err,  GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_sys,  GPIO_OUTPUT_INACTIVE);

	gpio_pin_set_dt(&led_rf24, 1);   /* 平时常亮 (2.4G 就绪) */
	gpio_pin_set_dt(&led_err,  1);   /* 开机自检: 点亮故障灯 */
	gpio_pin_set_dt(&led_sys,  0);

	k_work_init_delayable(&led_blink_work, led_blink_work_handler);
	blinking = false;
	k_work_init_delayable(&err_selftest_work, err_selftest_work_handler);
	k_work_schedule(&err_selftest_work, K_MSEC(ERR_SELFTEST_MS));
}

void gw_led_sys_on(void)
{
	gpio_pin_set_dt(&led_sys, 1);
}

void gw_led_error_on(void)
{
	error_latched = true;
	gpio_pin_set_dt(&led_err, 1);
}

/* 闪烁翻转: 每半个周期翻转一次 LED.
 * 仍在活跃窗口内 → 翻转 + 排下一次; 超时 (通信停止) → 停止翻转, 置常亮. */
static void led_blink_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	uint32_t elapsed = k_uptime_get_32() - rf24_last_activity;

	if (elapsed < RF24_ACTIVITY_TIMEOUT_MS) {
		/* 用读当前电平来翻转, 避免维护额外状态变量 */
		int cur = gpio_pin_get_dt(&led_rf24);

		gpio_pin_set_dt(&led_rf24, cur ? 0 : 1);
		k_work_reschedule(&led_blink_work, K_MSEC(RF24_BLINK_HALF_PERIOD_MS));
	} else {
		/* 通信停止: 恢复常亮, 标记闪烁周期结束 */
		gpio_pin_set_dt(&led_rf24, 1);
		blinking = false;
	}
}

void gw_led_rf24_activity(void)
{
	rf24_last_activity = k_uptime_get_32();
	/* 仅在闪烁未启动时提交, 避免收发路径重复排 work (零阻塞) */
	if (!blinking) {
		blinking = true;
		gpio_pin_set_dt(&led_rf24, 0);   /* 进入闪烁: 先灭, 开始第一个半周期 */
		k_work_reschedule(&led_blink_work, K_MSEC(RF24_BLINK_HALF_PERIOD_MS));
	}
}

/* ================================================================
 * Zephyr fatal error handler - 覆盖默认弱符号
 * CPU 异常 (hardfault 等) 时点亮错误灯锁定, 之后交还默认处理流程
 * (不做 reboot, 让系统进入 fault dump / LOG_PANIC 以便调试).
 * 注: STM32F1 (Cortex-M3) 无 MPU, 硬件栈保护不可用, K_ERR_STACK_CHK_FAIL
 *     不会触发; 但 K_ERR_CPU_EXCEPTION (hardfault) 仍会被此 handler 捕获.
 * ================================================================ */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	if (reason == K_ERR_STACK_CHK_FAIL || reason == K_ERR_CPU_EXCEPTION) {
		/* fatal 上下文 (栈已损/ISR 中) 不调 LOG, 仅点亮错误灯这一可靠副作用.
		 * 之后交还默认 fatal 处理流程做 dump/panic. */
		gw_led_error_on();
	}
}
