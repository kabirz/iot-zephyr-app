/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 数字 IO: 16 路 DI 采集 + 8 路 DO 输出 + 8 路 LED 指示 (移植自 io-edge-hub
 * src/io/dio.c, 去历史/寄存器表依赖, 数据落点改为 OD)
 *
 *   - DI 线程按 OD 0x2004:3 间隔采样 (仅使能位参与), 写 OD 0x2001,
 *     变更时 OD_requestTPDO 触发 TPDO2 (事件驱动, inhibit 20ms)
 *   - DO 由 co_io_set_do() 驱动: SDO 写 0x2002 与 RPDO1 收包共路,
 *     LED 自动跟随 DO 状态
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>

#include "CANopen.h"
#include "OD.h"
#include "io.h"

LOG_MODULE_REGISTER(canopen_io_dio, LOG_LEVEL_INF);

#define ZU_NODE DT_PATH(zephyr_user)

#define DI_SPEC_FN(inst, prop, idx)  GPIO_DT_SPEC_GET_BY_IDX(inst, prop, idx),
#define DO_SPEC_FN(inst, prop, idx)  GPIO_DT_SPEC_GET_BY_IDX(inst, prop, idx),
#define LED_SPEC_FN(inst, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(inst, prop, idx),

static const struct gpio_dt_spec di_gpios[] = {
	DT_FOREACH_PROP_ELEM(ZU_NODE, di_gpios, DI_SPEC_FN)};
static const struct gpio_dt_spec do_gpios[] = {
	DT_FOREACH_PROP_ELEM(ZU_NODE, do_gpios, DO_SPEC_FN)};
static const struct gpio_dt_spec led_gpios[] = {
	DT_FOREACH_PROP_ELEM(ZU_NODE, led_gpios, LED_SPEC_FN)};

/* DO 输出 + LED 联动 (0x2002 写回调路径: SDO / RPDO1 共用) */
void co_io_set_do(uint16_t val)
{
	OD_RAM.x2002_digitalOutput = val;

	for (int i = 0; i < CO_IO_DO_NUM && i < ARRAY_SIZE(do_gpios); i++) {
		bool on = (val & BIT(i)) != 0;

		gpio_pin_set_dt(&do_gpios[i], on);
		if (i < ARRAY_SIZE(led_gpios)) {
			gpio_pin_set_dt(&led_gpios[i], on);
		}
	}

	OD_requestTPDO(OD_ENTRY_H2002, 0);
}

uint16_t co_io_get_do(void)
{
	return OD_RAM.x2002_digitalOutput;
}

static void di_thread(void *p1, void *p2, void *p3)
{
	uint16_t last = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		uint32_t si = OD_PERSIST_APP.x2004_configParams[2]; /* 0x2004:3 */
		uint16_t en = OD_PERSIST_APP.x2004_configParams[0]; /* 0x2004:1 */
		uint16_t val = 0;

		if (si < CONFIG_CANOPEN_IO_SAMPLE_MIN_MS) {
			si = CONFIG_CANOPEN_IO_SAMPLE_MIN_MS;
		} else if (si > CONFIG_CANOPEN_IO_SAMPLE_MAX_MS) {
			si = CONFIG_CANOPEN_IO_SAMPLE_MAX_MS;
		}

		for (int i = 0; i < CO_IO_DI_NUM && i < ARRAY_SIZE(di_gpios); i++) {
			if ((en & BIT(i)) && gpio_pin_get_dt(&di_gpios[i])) {
				val |= BIT(i);
			}
		}

		OD_RAM.x2001_digitalInput = val;
		if (val != last) {
			last = val;
			OD_requestTPDO(OD_ENTRY_H2001, 0);
		}

		k_msleep(si);
	}
}

K_THREAD_DEFINE(di, CONFIG_CANOPEN_IO_DI_STACK_SIZE, di_thread, NULL, NULL, NULL,
		CONFIG_CANOPEN_IO_DI_PRIORITY, 0, 0);

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

	LOG_INF("DIO ready: %zu DI, %zu DO, %zu LED", ARRAY_SIZE(di_gpios),
		ARRAY_SIZE(do_gpios), ARRAY_SIZE(led_gpios));
	return 0;
}

SYS_INIT(dio_init, APPLICATION, CONFIG_CANOPEN_IO_INIT_PRIORITY_IO);
