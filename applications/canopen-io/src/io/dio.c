/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 数字 IO: 16 路 DI 采集 + 8 路 DO 输出 + 8 路 LED 指示
 * (合并 io-edge-hub 与 canopen-io 两版: 数据源统一为 holding/input 寄存器,
 *  同时镜像到 CANopen OD 0x2001/0x2002 并触发 TPDO)
 *
 *   - DI 线程按 holding_reg[DI_SI] 间隔采样 (仅使能通道参与), 结果写
 *     input_reg[DI] 并镜像 OD 0x2001, 变更时触发 TPDO2 (事件驱动)
 *   - DO 由 mb_set_do() 驱动 (io_write_holding[HOLDING_DO] / RPDO1 / SDO
 *     写 0x2002 共路): GPIO 输出 + LED 联动 + OD 0x2002 回读
 *   - DI 使能且历史开启时, 采样数据异步送历史记录
 *
 * GPIO 引脚定义在应用 overlay 的 /zephyr,user 节点 (di-gpios/do-gpios/led-gpios).
 */

#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>

#include "CANopen.h"
#include "OD.h"
#include "io.h"
#include "init.h"

LOG_MODULE_REGISTER(io_dio, LOG_LEVEL_INF);

#define ZU_NODE DT_PATH(zephyr_user)

/* 从 /zephyr,user 的 gpio 列表生成引脚描述数组 */
#define DI_SPEC_FN(inst, prop, idx)  GPIO_DT_SPEC_GET_BY_IDX(inst, prop, idx),
#define DO_SPEC_FN(inst, prop, idx)  GPIO_DT_SPEC_GET_BY_IDX(inst, prop, idx),
#define LED_SPEC_FN(inst, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(inst, prop, idx),

static const struct gpio_dt_spec di_gpios[] = {DT_FOREACH_PROP_ELEM(ZU_NODE, di_gpios, DI_SPEC_FN)};
static const struct gpio_dt_spec do_gpios[] = {DT_FOREACH_PROP_ELEM(ZU_NODE, do_gpios, DO_SPEC_FN)};
static const struct gpio_dt_spec led_gpios[] = {
	DT_FOREACH_PROP_ELEM(ZU_NODE, led_gpios, LED_SPEC_FN)};

/* ================================================================
 * DO 执行层: GPIO 输出 + LED 联动 + OD 0x2002 回读 (+TPDO 触发)
 * 不写 holding_reg (调用方已更新寄存器), 避免递归。
 * ================================================================ */
int mb_set_do(uint16_t val)
{
	OD_RAM.x2002_digitalOutput = val;

	for (int i = 0; i < DO_NUM && i < ARRAY_SIZE(do_gpios); i++) {
		bool on = (val & BIT(i)) != 0;

		gpio_pin_set_dt(&do_gpios[i], on);
		if (i < ARRAY_SIZE(led_gpios)) {
			gpio_pin_set_dt(&led_gpios[i], on);
		}
	}

	OD_requestTPDO(OD_ENTRY_H2002, 0);
	return 0;
}

/* CANopen 侧入口 (SDO 写 0x2002 / RPDO1): 同步到 holding_reg 后走同一执行层 */
void co_io_set_do(uint16_t val)
{
	update_holding_reg(HOLDING_DO_IDX, val);
	mb_set_do(val);
}

uint16_t co_io_get_do(void)
{
	return OD_RAM.x2002_digitalOutput;
}

/* ================================================================
 * DI 采样线程
 * ================================================================ */
static void di_thread(void *p1, void *p2, void *p3)
{
	uint16_t last = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		uint32_t si = get_holding_reg(HOLDING_DI_SAMPLE_MS_IDX);
		uint16_t en = get_holding_reg(HOLDING_DI_ENABLE_IDX);
		uint16_t val = 0;

		if (si < CONFIG_CANOPEN_IO_SAMPLE_MIN_MS) {
			si = CONFIG_CANOPEN_IO_SAMPLE_MIN_MS;
		} else if (si > CONFIG_CANOPEN_IO_SAMPLE_MAX_MS) {
			si = CONFIG_CANOPEN_IO_SAMPLE_MAX_MS;
		}

		for (int i = 0; i < DI_NUM && i < ARRAY_SIZE(di_gpios); i++) {
			if ((en & BIT(i)) && gpio_pin_get_dt(&di_gpios[i])) {
				val |= BIT(i);
			}
		}
		update_input_reg(INPUT_DI_IDX, val);

		/* CANopen 镜像: 变更时触发 TPDO2 (事件型, inhibit 20ms) */
		if (val != last) {
			last = val;
			OD_RAM.x2001_digitalInput = val;
			OD_requestTPDO(OD_ENTRY_H2001, 0);
		}

		/* 历史记录: send_history_data 内部按使能状态决定是否入队 */
		if (en) {
			struct his_data d = {0};

			d.type = DI_TYPE;
			d.timestamps = (uint32_t)time(NULL);
			d.di.di_en_status = en;
			d.di.di_value = val;
			send_history_data(&d);
		}

		k_msleep(si);
	}
}

K_THREAD_DEFINE(di, CONFIG_CANOPEN_IO_DI_STACK_SIZE, di_thread, NULL, NULL, NULL,
		CONFIG_CANOPEN_IO_DI_PRIORITY, 0, 0);

/* ================================================================
 * GPIO 初始化
 * ================================================================ */
static int dio_init(void)
{
	for (int i = 0; i < ARRAY_SIZE(di_gpios); i++) {
		if (!gpio_is_ready_dt(&di_gpios[i])) {
			LOG_ERR("DI%zu GPIO not ready", (size_t)i);
			return -ENODEV;
		}
		gpio_pin_configure_dt(&di_gpios[i], GPIO_INPUT);
	}
	for (int i = 0; i < ARRAY_SIZE(do_gpios); i++) {
		gpio_pin_configure_dt(&do_gpios[i], GPIO_OUTPUT_INACTIVE);
	}
	for (int i = 0; i < ARRAY_SIZE(led_gpios); i++) {
		gpio_pin_configure_dt(&led_gpios[i], GPIO_OUTPUT_INACTIVE);
	}

	LOG_INF("DIO ready: %zu DI, %zu DO, %zu LED", ARRAY_SIZE(di_gpios), ARRAY_SIZE(do_gpios),
		ARRAY_SIZE(led_gpios));
	return 0;
}

SYS_INIT(dio_init, APPLICATION, CONFIG_CANOPEN_IO_INIT_PRIORITY_IO);
