/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 模拟输入: 4 路 ADC (ADC1 IN10-IN13, PC0-PC3)
 *   AI0/AI1 (IN10/11): 电流 4-20mA, value = 7.414 * voltage / 10  (单位 0.01mA)
 *   AI2/AI3 (IN12/13): 电压 0-10V,  value = 3.7037 * voltage / 10 (单位 0.01V)
 *
 * 仅使能通道 (holding_reg[AI_EN] 低 4 位) 参与采样, 结果写 input_reg[AI0..3]。
 * 使能且历史开启时, 采样数据异步送历史记录。采样线程周期喂看门狗。
 */

#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <init.h>
#include "watchdog.h"

LOG_MODULE_REGISTER(io_adc, LOG_LEVEL_INF);

/* ADC1 通道: IN10, IN11, IN12, IN13 */
static const uint8_t ai_channel_id[AI_NUM] = { 10, 11, 12, 13 };
/* 工程量转换系数 (放大 1e4 倍做整数运算): 电流 7.414, 电压 3.7037 */
static const uint32_t ai_coeff[AI_NUM] = { 7414, 7414, 3704, 3704 };

static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc1));

static int16_t ai_buffer[AI_NUM];

/* 12-bit raw -> 工程量 (0.01mA / 0.01V) */
static uint16_t ai_convert(int ch, int32_t raw)
{
	int32_t voltage_mv = raw * 3300 / 4096;	/* VREF = VDDA = 3.3V */
	uint32_t val = (uint64_t)ai_coeff[ch] * (uint32_t)voltage_mv / 10000U;

	return (uint16_t)val;
}

static void adc_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		uint32_t si = get_holding_reg(HOLDING_AI_SI_IDX);
		uint16_t en = get_holding_reg(HOLDING_AI_EN_IDX);

		if (si < 10) {
			si = 10;
		}

		for (int i = 0; i < AI_NUM; i++) {
			struct adc_sequence seq = {
				.channels = BIT(ai_channel_id[i]),
				.buffer = &ai_buffer[i],
				.buffer_size = sizeof(ai_buffer[i]),
				.resolution = 12,
				.oversampling = 0,
				.calibrate = 0,
			};

			if (adc_read(adc_dev, &seq) == 0) {
				update_input_reg(INPUT_AI0_IDX + i, ai_convert(i, ai_buffer[i]));
			}
		}

		/* 历史记录 (仅当有通道使能) */
		if (en & 0x000F) {
			struct his_data d = {0};

			d.type = AI_TYPE;
			d.timestamps = (uint32_t)time(NULL);
			d.ai.ai_en_status = en & 0x000F;
			for (int i = 0; i < AI_NUM; i++) {
				d.ai.ai_value[i] = get_input_reg(INPUT_AI0_IDX + i);
			}
			send_history_data(&d);
		}

		watchdog_feed();
		k_msleep(si);
	}
}

K_THREAD_DEFINE(adc_io, CONFIG_IO_ADC_STACK_SIZE, adc_thread, NULL, NULL, NULL, 1, 0, 0);

static int adc_init(void)
{
	if (!device_is_ready(adc_dev)) {
		LOG_ERR("ADC1 device not ready");
		return -ENODEV;
	}

	for (int i = 0; i < AI_NUM; i++) {
		struct adc_channel_cfg ch_cfg = {
			.gain = ADC_GAIN_1,
			.reference = ADC_REF_INTERNAL,
			.acquisition_time = ADC_ACQ_TIME_DEFAULT,
			.channel_id = ai_channel_id[i],
			.differential = 0,
		};

		if (adc_channel_setup(adc_dev, &ch_cfg)) {
			LOG_ERR("ADC channel %u setup failed", ai_channel_id[i]);
			return -EIO;
		}
	}

	LOG_INF("ADC ready: 4 channels (IN10-IN13)");
	return 0;
}

SYS_INIT(adc_init, APPLICATION, 12);
