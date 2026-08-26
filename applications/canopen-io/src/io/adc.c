/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 模拟输入: 4 路 ADC (ADC1 IN10-IN13, PC0-PC3), 移植自 io-edge-hub src/io/adc.c
 *   - 通道号与工程量系数 (ai-coeffs) 来自设备树 /zephyr,user
 *   - 按 OD 0x2004:4 间隔采样 (仅使能通道), 写 OD 0x2000:1-4 并触发 TPDO1
 *     (事件驱动, 无 event timer -> 上报节奏 = 采样间隔)
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>

#include "CANopen.h"
#include "OD.h"
#include "io.h"

LOG_MODULE_REGISTER(canopen_io_adc, LOG_LEVEL_INF);

#define ADC_NODE DT_PATH(zephyr_user)

#define ADC_SPEC_FN(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),
static const struct adc_dt_spec adc_specs[] = {
	DT_FOREACH_PROP_ELEM(ADC_NODE, io_channels, ADC_SPEC_FN)};

#define AI_COEFF_FN(node_id, prop, idx) DT_PROP_BY_IDX(node_id, prop, idx),
static const uint32_t ai_coeff[CO_IO_AI_NUM] = {
	DT_FOREACH_PROP_ELEM(ADC_NODE, ai_coeffs, AI_COEFF_FN)};

static int16_t ai_buffer[CO_IO_AI_NUM];

/* 12-bit raw -> 工程量 (0.01mA / 0.01V) */
static uint16_t ai_convert(int ch, int32_t raw)
{
	int32_t voltage_mv = raw * 3300 / 4096; /* VREF = VDDA = 3.3V */
	uint32_t val = (uint64_t)ai_coeff[ch] * (uint32_t)voltage_mv / 10000U;

	return (uint16_t)val;
}

static void adc_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		uint32_t si = OD_PERSIST_APP.x2004_configParams[3]; /* 0x2004:4 */
		uint16_t en = OD_PERSIST_APP.x2004_configParams[1]; /* 0x2004:2 */

		if (si < CONFIG_CANOPEN_IO_SAMPLE_MIN_MS) {
			si = CONFIG_CANOPEN_IO_SAMPLE_MIN_MS;
		} else if (si > CONFIG_CANOPEN_IO_SAMPLE_MAX_MS) {
			si = CONFIG_CANOPEN_IO_SAMPLE_MAX_MS;
		}

		int ch_num = MIN(ARRAY_SIZE(adc_specs), CO_IO_AI_NUM);

		for (int i = 0; i < ch_num; i++) {
			if (!(en & BIT(i))) {
				continue;
			}
			struct adc_sequence seq = {
				.channels = BIT(adc_specs[i].channel_id),
				.buffer = &ai_buffer[i],
				.buffer_size = sizeof(ai_buffer[i]),
				.resolution = adc_specs[i].resolution,
				.oversampling = adc_specs[i].oversampling,
				.calibrate = 0,
			};

			if (adc_read_dt(&adc_specs[i], &seq) == 0) {
				OD_RAM.x2000_analogInput[i] =
					(int16_t)ai_convert(i, ai_buffer[i]);
				OD_requestTPDO(OD_ENTRY_H2000, i + 1);
			}
		}

		k_msleep(si);
	}
}

K_THREAD_DEFINE(adc_io, CONFIG_CANOPEN_IO_ADC_STACK_SIZE, adc_thread, NULL, NULL,
		NULL, CONFIG_CANOPEN_IO_ADC_PRIORITY, 0, 0);

static int adc_init(void)
{
	if (ARRAY_SIZE(adc_specs) != CO_IO_AI_NUM) {
		LOG_ERR("io-channels count mismatch (expect %d, got %d)",
			CO_IO_AI_NUM, (int)ARRAY_SIZE(adc_specs));
		return -EINVAL;
	}

	for (int i = 0; i < ARRAY_SIZE(adc_specs); i++) {
		if (!device_is_ready(adc_specs[i].dev)) {
			LOG_ERR("ADC device not ready");
			return -ENODEV;
		}
		if (adc_channel_setup_dt(&adc_specs[i])) {
			LOG_ERR("ADC channel %u setup failed", adc_specs[i].channel_id);
			return -EIO;
		}
	}

	LOG_INF("ADC ready: %d channels", (int)ARRAY_SIZE(adc_specs));
	return 0;
}

SYS_INIT(adc_init, APPLICATION, CONFIG_CANOPEN_IO_INIT_PRIORITY_IO);
