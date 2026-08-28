/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 模拟输入: 4 路 ADC (ADC1 IN10-IN13, PC0-PC3)
 *   - 通道号列表 (ai-channels) 与工程量转换系数 (ai-coeffs) 来自设备树
 *     /zephyr,user; 各通道参数一致 (增益/基准/分辨率), 锚定 io-channels
 *     首条 (&adc1 的 channel@a 模板), 仅通道号不同
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
#include <init.h>

LOG_MODULE_REGISTER(io_adc, LOG_LEVEL_INF);

/* 采样间隔上限 5s: 业务合理性约束 (远程调大时钳制, 防止采样响应过慢) */
#define SAMPLE_INTERVAL_MAX 5000U

#define ADC_NODE DT_PATH(zephyr_user)

/* 共用通道参数 (device + channel_cfg + 分辨率), 锚定 io-channels 首条
 * (即 &adc1 的 channel@a 模板): 各 AI 通道参数一致, 仅通道号不同 */
static const struct adc_dt_spec adc_spec = ADC_DT_SPEC_GET(ADC_NODE);

/* AI 通道号列表: 与 ai-coeffs 顺序一一对应 */
#define AI_CH_FN(node_id, prop, idx) DT_PROP_BY_IDX(node_id, prop, idx),
static const uint8_t ai_channel[AI_NUM] = {DT_FOREACH_PROP_ELEM(ADC_NODE, ai_channels, AI_CH_FN)};

/* 工程量转换系数 (放大 1e4 倍做整数运算): 从 /zephyr,user 的 ai-coeffs 读取,
 * 与 ai-channels 顺序一一对应 (数量错误时按少于 AI_NUM 的配置静默补 0,
 * 由 adc_init 的 ai-channels 数量校验兜底) */
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

		if (si < 10) {
			si = 10;
		} else if (si > SAMPLE_INTERVAL_MAX) {
			si = SAMPLE_INTERVAL_MAX;
		}

		/* 单次扫描采集全部使能通道: 一次 adc_read 走多通道序列,
		 * 结果按通道号升序连续写入 ai_buffer (依赖 adc_init 的升序校验) */
		uint32_t chan_mask = 0;

		for (int i = 0; i < AI_NUM; i++) {
			if (en & BIT(i)) {
				chan_mask |= BIT(ai_channel[i]);
			}
		}

		if (chan_mask != 0) {
			struct adc_sequence seq = {
				.channels = chan_mask,
				.buffer = ai_buffer,
				.buffer_size = sizeof(ai_buffer),
				.resolution = adc_spec.resolution,
				.oversampling = adc_spec.oversampling,
				.calibrate = 0,
			};

			if (adc_read(adc_spec.dev, &seq) == 0) {
				int slot = 0;

				for (int i = 0; i < AI_NUM; i++) {
					if (!(en & BIT(i))) {
						continue;
					}
					update_input_reg(INPUT_AI0_IDX + i,
							 ai_convert(i, ai_buffer[slot++]));
				}
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

K_THREAD_DEFINE(adc_io, CONFIG_IO_ADC_STACK_SIZE, adc_thread, NULL, NULL, NULL,
		CONFIG_IO_ADC_PRIORITY, 0, 0);

static int adc_init(void)
{
	if (DT_PROP_LEN(ADC_NODE, ai_channels) != AI_NUM) {
		LOG_ERR("ai-channels count mismatch (expect %d, got %d)", AI_NUM,
			DT_PROP_LEN(ADC_NODE, ai_channels));
		return -EINVAL;
	}

	/* 扫描结果按通道号升序写入缓冲: 通道号必须严格升序 */
	for (int i = 1; i < AI_NUM; i++) {
		if (ai_channel[i] <= ai_channel[i - 1]) {
			LOG_ERR("ai-channels must be in ascending order");
			return -EINVAL;
		}
	}

	if (!device_is_ready(adc_spec.dev)) {
		LOG_ERR("ADC device not ready");
		return -ENODEV;
	}

	/* 各通道共用参数模板, 仅通道号不同 */
	for (int i = 0; i < AI_NUM; i++) {
		struct adc_dt_spec spec = adc_spec;

		spec.channel_id = ai_channel[i];
		if (adc_channel_setup_dt(&spec)) {
			LOG_ERR("ADC channel %u setup failed", ai_channel[i]);
			return -EIO;
		}
	}

	LOG_INF("ADC ready: %d channels", AI_NUM);
	return 0;
}

SYS_INIT(adc_init, APPLICATION, CONFIG_IO_INIT_PRIORITY_ADC);
