/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADC 操纵杆角度采集 + 电源电压采样
 * 基于 Zephyr ADC API, 500ms 周期采集 X/Y 角度, 2s 周期采集电压
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <common.h>
#include <display.h>
#include <mod-gpio.h>

LOG_MODULE_REGISTER(adc_reader, LOG_LEVEL_INF);

#define CAN_SLEEP_MS  20
#define RF24_SLEEP_MS 50
#define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, DT_SPEC_AND_COMMA)};


int adc_init_channels(void)
{
	int ret;

	for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
		if (!device_is_ready(adc_channels[i].dev)) {
			LOG_ERR("ADC device not ready");
			return -ENODEV;
		}

		ret = adc_channel_setup_dt(&adc_channels[i]);
		if (ret < 0) {
			LOG_ERR("Could not setup channel #%d (%d)", adc_channels[i].channel_id,
				ret);
			return ret;
		}
	}

	LOG_INF("ADC channels configured successfully");

	return ret;
}

/* 去除两个极值后求平均值 */
static int32_t trim_avg(int32_t *samples, int count)
{
	int32_t min_v = samples[0];
	int32_t max_v = samples[0];
	int32_t sum = 0;

	for (int i = 0; i < count; i++) {
		if (samples[i] < min_v) {
			min_v = samples[i];
		}
		if (samples[i] > max_v) {
			max_v = samples[i];
		}
		sum += samples[i];
	}
	return (sum - min_v - max_v) / (count - 2);
}

void adc_read_thread(void)
{
	int ret;
	uint16_t buf;
	struct adc_sequence sequence = {.buffer = &buf, .buffer_size = sizeof(buf)};
	int32_t val_mv;

	#define XY_SAMPLE_COUNT 10
	static int32_t x_samples[XY_SAMPLE_COUNT];
	static int32_t y_samples[XY_SAMPLE_COUNT];
	static int xy_sample_idx = 0;

	LOG_INF("ADC read thread started");

	while (true) {
		if (atomic_get(&global_params.sleeping)) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}
		uint32_t t1 = k_uptime_get_32();

		/* 连续快速采集 10 次 X/Y */
		for (xy_sample_idx = 0; xy_sample_idx < XY_SAMPLE_COUNT; xy_sample_idx++) {
			for (size_t i = 0U; i < ARRAY_SIZE(adc_channels) - 1; i++) {
				(void)adc_sequence_init_dt(&adc_channels[i], &sequence);
				ret = adc_read_dt(&adc_channels[i], &sequence);
				if (ret < 0) {
					LOG_ERR("Could not read (%d)", ret);
					continue;
				}
				val_mv = (int32_t)buf;

				ret = adc_raw_to_millivolts_dt(&adc_channels[i], &val_mv);
				if (ret < 0) {
					LOG_ERR(" (value in mV not available)");
				}
				/* ADC 直读电压 (无分压网络) 300~2700mV → -200~+200 (0.1° 单位) */
				if (i == 0) {
					x_samples[xy_sample_idx] =
						(CLAMP(val_mv, 300, 2700) - 300) * 400 / 2400 - 200;
					if (global_params.adc_debug) {
						LOG_INF("x: %d mv, degree: %d (0.1deg)", val_mv,
							x_samples[xy_sample_idx]);
					}
				} else {
					y_samples[xy_sample_idx] =
						(CLAMP(val_mv, 300, 2700) - 300) * 400 / 2400 - 200;
					if (global_params.adc_debug) {
						LOG_INF("y: %d mv, degree: %d (0.1deg)", val_mv,
							y_samples[xy_sample_idx]);
					}
				}
			}
			k_usleep(500);
		}

		/* 平均后转 1° 精度 (trim_avg 返回 0.1° 单位, /10 取整).
		 * 死区: 平均值落在 (-1°, +1°) 即 (-10, +10) 的 0.1° 值统一归 0°,
		 * 消除摇杆居中时的微小抖动. */
		int x_avg = trim_avg(x_samples, XY_SAMPLE_COUNT);
		int y_avg = trim_avg(y_samples, XY_SAMPLE_COUNT);
		int x_degree = (x_avg > -10 && x_avg < 10) ? 0 : x_avg / 10;
		int y_degree = (y_avg > -10 && y_avg < 10) ? 0 : y_avg / 10;

		if (x_degree != global_params.x_degree ||
		    y_degree != global_params.y_degree) {
			last_activity_time = k_uptime_get_32();
			global_params.x_degree = x_degree;
			global_params.y_degree = y_degree;
		}

		/* 上报 */
		uint8_t buf[8];
		sys_put_be16((uint16_t)global_params.x_degree, &buf[0]);
		sys_put_be16((uint16_t)global_params.y_degree, &buf[2]);
		buf[4] = global_params.h_button;
		buf[5] = buf[6] = buf[7] = 0xff;
		if (global_params.log) {
			LOG_INF("x: %d, y: %d, button: %d",
				global_params.x_degree, global_params.y_degree, global_params.h_button);
		}
		send_handler_state(buf, 8);
		uint32_t diff = k_uptime_get_32() - t1;
		uint32_t sleep_ms = global_params.connect_type == CAN_TYPE ? CAN_SLEEP_MS : RF24_SLEEP_MS;
		if (diff < sleep_ms) {
			k_sleep(K_MSEC(sleep_ms - diff));
		}
	}
}
K_THREAD_DEFINE(thread_adc, 1024, adc_read_thread, NULL, NULL, NULL, 7, 0, 0);

void adc_power_thread(void)
{
	int ret;
	uint16_t buf;
	struct adc_sequence sequence = {.buffer = &buf, .buffer_size = sizeof(buf)};
	int32_t val_mv, power_mv = 0;
	static const struct adc_dt_spec *adc_voltage_channel = &adc_channels[ARRAY_SIZE(adc_channels) - 1];
	#define POWER_SAMPLE_COUNT 10
	static int32_t power_samples[POWER_SAMPLE_COUNT];
	static int power_sample_idx = 0;


	LOG_INF("ADC power thread started");
	(void)adc_sequence_init_dt(adc_voltage_channel, &sequence);

	while (true) {
		if (atomic_get(&global_params.sleeping)) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}

		ret = adc_read_dt(adc_voltage_channel, &sequence);
		if (ret < 0) {
			LOG_ERR("Could not read power channel (%d)", ret);
		} else {
			val_mv = (int32_t)buf;
			ret = adc_raw_to_millivolts_dt(adc_voltage_channel, &val_mv);
			if (ret == 0) {
				power_samples[power_sample_idx] = val_mv * 2;
				power_sample_idx++;
			}
		}

		if (power_sample_idx >= POWER_SAMPLE_COUNT) {
			power_mv = trim_avg(power_samples, POWER_SAMPLE_COUNT);
			power_sample_idx = 0;

			battery_status_t battery_status = read_battery_status();
			if (power_mv != global_params.power_mv ||
			    global_params.battery_status != battery_status) {
				mod_display_battery(power_mv, battery_status);
				global_params.power_mv = power_mv;
				global_params.battery_status = battery_status;
			}
		}

		k_sleep(K_MSEC(200));
	}
}

K_THREAD_DEFINE(thread_adc_power, 1024, adc_power_thread, NULL, NULL, NULL, 8, 0, 0);

#ifdef CONFIG_SHELL

/* adc log [0/1] — 手动开关 ADC 原始电压采样日志 (val[i]: <mv>).
 * 默认编译期 LOG_LEVEL_INF 不打印每通道原始值, 此开关运行时打开用于调试. */
static int cmd_adc_log(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(ctx, "adc log: %s", global_params.adc_debug ? "on" : "off");
		return 0;
	}
	int val = (int)strtol(argv[1], NULL, 10);

	global_params.adc_debug = (val != 0);
	shell_print(ctx, "adc log: %s", global_params.adc_debug ? "on" : "off");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_adc_cmds,
	SHELL_CMD_ARG(log, NULL, "Enable/disable ADC raw sample log [0/1]", cmd_adc_log, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(adc, &sub_adc_cmds, "ADC sample debug commands", NULL);

#endif /* CONFIG_SHELL */
