/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 模拟输入: 4 路 ADC (ADC1 IN10-IN13, PC0-PC3)
 * (合并 io-edge-hub 与 canopen-io 两版: 数据源统一为 input 寄存器,
 *  同时镜像到 CANopen OD 0x2000 并按通道触发 TPDO1)
 *
 *   - 通道号与工程量转换系数 (ai-coeffs) 均来自设备树:
 *     /zephyr,user 的 io-channels 引用 &adc1 下 channel@a-d 子节点
 *   - AI0/AI1 (IN10/11): 电流 4-20mA, value = 7.414 * voltage / 10  (单位 0.01mA)
 *   - AI2/AI3 (IN12/13): 电压 0-10V,  value = 3.7037 * voltage / 10 (单位 0.01V)
 *
 * 仅使能通道 (holding_reg[AI_EN] 低 4 位) 参与采样, 结果写 input_reg[AI0..3]。
 * 使能且历史开启时, 采样数据异步送历史记录。
 */

#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>

#include "CANopen.h"
#include "OD.h"
#include "io.h"
#include "init.h"

LOG_MODULE_REGISTER(io_adc, LOG_LEVEL_INF);

#define ADC_NODE DT_PATH(zephyr_user)

/* 通道配置 (device + channel_id + channel_cfg) 取自 /zephyr,user 的 io-channels */
#define ADC_SPEC_FN(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),
static const struct adc_dt_spec adc_specs[] = {
	DT_FOREACH_PROP_ELEM(ADC_NODE, io_channels, ADC_SPEC_FN)};

/* 工程量转换系数 (放大 1e4 倍做整数运算): 从 /zephyr,user 的 ai-coeffs 读取,
 * 与 io-channels 顺序一一对应 */
#define AI_COEFF_FN(node_id, prop, idx) DT_PROP_BY_IDX(node_id, prop, idx),
static const uint32_t ai_coeff[AI_NUM] = {DT_FOREACH_PROP_ELEM(ADC_NODE, ai_coeffs, AI_COEFF_FN)};

static int16_t ai_buffer[AI_NUM];

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
		uint32_t si = get_holding_reg(HOLDING_AI_SAMPLE_MS_IDX);
		uint16_t en = get_holding_reg(HOLDING_AI_ENABLE_IDX);

		if (si < CONFIG_CANOPEN_IO_SAMPLE_MIN_MS) {
			si = CONFIG_CANOPEN_IO_SAMPLE_MIN_MS;
		} else if (si > CONFIG_CANOPEN_IO_SAMPLE_MAX_MS) {
			si = CONFIG_CANOPEN_IO_SAMPLE_MAX_MS;
		}

		/* 采样循环上限取 min(adc_specs, AI_NUM), 防止 io-channels 配置过多时
		 * 越界访问 ai_coeff[] (ai_coeff 固定 AI_NUM 大小) */
		int ch_num = MIN(ARRAY_SIZE(adc_specs), AI_NUM);

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
				uint16_t val = ai_convert(i, ai_buffer[i]);

				update_input_reg(INPUT_AI0_IDX + i, val);
				/* CANopen 镜像 + 按通道触发 TPDO1 */
				OD_RAM.x2000_analogInput[i] = (int16_t)val;
				OD_requestTPDO(OD_ENTRY_H2000, i + 1);
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

		k_msleep(si);
	}
}

K_THREAD_DEFINE(adc_io, CONFIG_CANOPEN_IO_ADC_STACK_SIZE, adc_thread, NULL, NULL, NULL,
		CONFIG_CANOPEN_IO_ADC_PRIORITY, 0, 0);

static int adc_init(void)
{
	if (ARRAY_SIZE(adc_specs) != AI_NUM) {
		LOG_ERR("io-channels count mismatch (expect %d, got %d)", AI_NUM,
			(int)ARRAY_SIZE(adc_specs));
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
