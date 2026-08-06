/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SH1106 OLED 显示驱动 (128x64, I2C)
 * 基于 Zephyr display API, 8x16 ASCII 字体 + 独立位图图标
 * global_params 定义在此文件
 *
 * 布局 (SCANNER_USE_8x16 = 0, 5x8 字体):
 *   Row 0 (y=0-15):   [8x16] 连接类型图标(8px) + 信号图标(16px) + 电池图标(24px)
 *   (y=16-23):        (空行)
 *   Row 1 (y=24-31):  [5x8]  超欠挖 "OverBreak: {value}"
 *   Row 2 (y=32-39):  [5x8]  激光距离 "Distance:  {value}"
 *   Row 3 (y=40-47):  [5x8]  X 轴坐标 "X axis:    {value}"
 *   Row 4 (y=48-55):  [5x8]  Y 轴坐标 "Y axis:    {value}"
 *   Row 5 (y=56-63):  [5x8]  Z 轴坐标 "Z axis:    {value}"
 *
 * 布局 (SCANNER_USE_8x16 = 1, SHOW_XYZ = 1, 8x16 字体):
 *   Row 0 (y=0-15):   [8x16] 连接类型图标(8px) + 信号图标(16px) + 电池图标(24px)
 *   Row 1 (y=16-31):  [8x16] 超欠挖 "OB:" | 激光距离 "Dis:"    ← 左右各 64px
 *   Row 2 (y=32-47):  [8x16] X 轴 "X:" | Y 轴 "Y:"             ← 左右各 64px
 *   Row 3 (y=48-63):  [8x16] Z 轴 "Z:"
 *
 * 布局 (SCANNER_USE_8x16 = 1, SHOW_XYZ = 0, 8x16 字体):
 *   Row 0 (y=0-15):   [8x16] ASCII文字("NRF "/"CAN") + 信号图标(16px) + 电池图标(24px)
 *   Row 1 (y=24-31):  [8x16] 超欠挖 "OB:"
 *   Row 2 (y=40-47):  [8x16] 激光距离 "Dis:"
 *
 * 测试模式布局 (8x16 字体):
 *   Row 0 (y=0-15):   [8x16] 同上
 *   Row 1 (y=16-31):  [8x16] "R:{rssi} S:{snr}"
 *   Row 2 (y=32-47):  [8x16] "LOST:{count}"
 *   Row 3 (y=48-63):  [8x16] "R:{rtt} A:{avg}"
 */

#include <common.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <display.h>
#include <label_icons.h>
#include <battery_icons.h>
#include <signal_icons.h>
#include <mod-can.h>
#include <mod-gpio.h>

LOG_MODULE_REGISTER(mod_display, LOG_LEVEL_INF);

/* ================================================================
 * 字体定义 (font_8x16.c)
 * ================================================================ */
#define FONT_8X16_FIRST 0x20
#define FONT_8X16_LAST  0x7E
#define FONT_8X16_COUNT (FONT_8X16_LAST - FONT_8X16_FIRST + 1)

extern const uint8_t font_8x16[FONT_8X16_COUNT][16];

/* ================================================================
 * 字体定义 (font_5x8.c)
 * ================================================================ */
#define FONT_5X8_FIRST 0x20
#define FONT_5X8_LAST  0x7E
#define FONT_5X8_COUNT (FONT_5X8_LAST - FONT_5X8_FIRST + 1)

extern const uint8_t font_5x8[FONT_5X8_COUNT][6];

/* ================================================================
 * 显示设备 + 全局状态
 * ================================================================ */
static const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static struct display_capabilities capabilities;
static struct display_buffer_descriptor buf_desc;
static K_MUTEX_DEFINE(display_mutex);

/* ================================================================
 * 低级渲染原语
 * ================================================================ */

/* 写入任意位图块 */
static void display_write_buf(int x, int y, int w, int h, const void *data)
{
	/* 睡眠期间 OLED 已断电, 跳过显示写入: 避免 rf24/adc 线程在处理残留
	 * 帧时对 I2C 写失败 (OLED 不响应) 刷屏错误日志, 拖慢进入 STOP */
	if (atomic_get(&global_params.sleeping)) {
		return;
	}
	buf_desc.buf_size = w * (h / 8);
	buf_desc.height = h;
	buf_desc.width = w;
	buf_desc.pitch = w;
	display_write(display_dev, x, y, &buf_desc, data);
}

/* 渲染单个 8x16 ASCII 字符 */
static void display_8x16_char(char ch, int x, int y)
{
	if (ch < FONT_8X16_FIRST || ch > FONT_8X16_LAST) {
		ch = '?';
	}
	display_write_buf(x, y, 8, 16, font_8x16[ch - FONT_8X16_FIRST]);
}

/* 渲染字符串并用空格填充到指定宽度, 覆盖旧内容 */
static void display_8x16_str_pad(const char *s, int x, int y, int width)
{
	int end = x + width;

	while (*s && x + 8 <= end && x + 8 <= 128) {
		display_8x16_char(*s, x, y);
		x += 8;
		s++;
	}
	/* 用空格填充剩余宽度, 清除旧数据 */
	while (x + 8 <= end && x + 8 <= 128) {
		display_8x16_char(' ', x, y);
		x += 8;
	}
}

/* 渲染单个 5x8 ASCII 字符 */
static void display_5x8_char(char ch, int x, int y)
{
	if (ch < FONT_5X8_FIRST || ch > FONT_5X8_LAST) {
		ch = '?';
	}
	display_write_buf(x, y, 6, 8, font_5x8[ch - FONT_5X8_FIRST]);
}

/* 渲染字符串并用空格填充到指定宽度, 覆盖旧内容 */
static void display_5x8_str_pad(const char *s, int x, int y, int width)
{
	char empty[6] = {0};
	int end = x + width;

	while (*s && x + 6 <= end && x + 6 <= 128) {
		display_5x8_char(*s, x, y);
		x += 6;
		s++;
	}
	/* 用空格填充剩余宽度, 清除旧数据 */
	while (x + 6 <= end && x + 6 <= 128) {
		display_5x8_char(' ', x, y);
		x += 6;
	}
	if (end > 126 && x < 128) {
		display_write_buf(x, y, 128-x, 8, empty);
	}
}

void mod_display_reinit(void)
{
	k_mutex_lock(&display_mutex, K_FOREVER);
	if (display_dev->ops.init) {
		display_dev->ops.init(display_dev);
	}
	k_mutex_unlock(&display_mutex);
}

/* 用空格填充到行尾, 覆盖旧内容 */
void mod_display_clear(void)
{
	k_mutex_lock(&display_mutex, K_FOREVER);
	for (int j = 0; j < capabilities.y_resolution; j += 16) {
		display_8x16_str_pad(" ", 0, j, 128);
	}
	k_mutex_unlock(&display_mutex);
}

/* ================================================================
 * 业务显示函数
 * ================================================================ */
#if IS_ENABLED(CONFIG_ANGLE_HANDLER_ROW0_ASCII)
#define ROW0_ASCII 1
#else
#define ROW0_ASCII 0
#endif
/* ASCII(64): Row 0 左侧(40x16, 16x16): NRF 信号 0/1/2/3/4 */
/* ICON(64):  Row 0 左侧(8x16, 16x16): NRF 信号 0/1/2/3/4 */
void mod_display_rf24(uint8_t rssi)
{
	if (global_params.connect_type != RF24_TYPE) {
		return;
	}
	k_mutex_lock(&display_mutex, K_FOREVER);
	if (rssi > 4) {
		rssi = 4;
	}
#if ROW0_ASCII
	display_8x16_str_pad("24G", 0, 0, 40);
	display_write_buf(40, 0, SIGNAL_ICON_W, SIGNAL_ICON_H, signal_levels[rssi]);
	display_8x16_char(' ', 56, 0);
#else
	display_write_buf(0, 0, LABEL_ICON_W, LABEL_ICON_H, label_rf24);
	display_write_buf(8, 0, SIGNAL_ICON_W, SIGNAL_ICON_H, signal_levels[rssi]);
	display_8x16_str_pad(" ", 24, 0, 40);
#endif
	k_mutex_unlock(&display_mutex);
}

/* ASCII(64): Row 0 左侧(24x16): 连接 CAN */
/* ICON(64):  Row 0 左侧(8x16):  连接 CAN */
void mod_display_can(void)
{
	if (global_params.connect_type != CAN_TYPE) {
		return;
	}
	k_mutex_lock(&display_mutex, K_FOREVER);
#if ROW0_ASCII
	display_8x16_str_pad("CAN", 0, 0, 64);
#else
	display_write_buf(0, 0, LABEL_ICON_W, LABEL_ICON_H, label_can);
	display_8x16_str_pad(" ", 8, 0, 64);
#endif
	k_mutex_unlock(&display_mutex);
}

/* Row 0 右侧(24x16 16x16): 电池电量和图标 */
void mod_display_battery(uint32_t power_mv, battery_status_t status)
{
	k_mutex_lock(&display_mutex, K_FOREVER);

	int idx = power_mv >= 3850   ? 4
		  : power_mv >= 3750 ? 3
		  : power_mv >= 3550 ? 2
		  : power_mv >= 3400 ? 1
				     : 0;
	if (status == BATTERY_STATUS_CHARGING) {
		display_8x16_str_pad(" ", 64, 0, 24);
		display_write_buf(88, 0, BATTERY_ICON_W, BATTERY_ICON_H, battery_levels[idx]);
		display_write_buf(112, 0, CHARGING_ICON_W, CHARGING_ICON_H, icon_charging);
	} else {
		display_8x16_str_pad(" ", 64, 0, 40);
		display_write_buf(104, 0, BATTERY_ICON_W, BATTERY_ICON_H, battery_levels[idx]);
	}

	k_mutex_unlock(&display_mutex);
}

static void int_to_decimal_str(int32_t value, uint8_t valid, char *buffer, size_t len,
			       const char *prefix)
{
	if (!valid) {
		snprintf(buffer, len, "%s --------", prefix);
		return;
	}

	/* 直接用 %d 格式化，避免 INT32_MIN 取负溢出 */
	if (value < 0) {
		int32_t int_part = -(-value / 1000);
		int32_t dec_part = -value % 1000;
		snprintf(buffer, len, "%s%d.%03d m", prefix, int_part, dec_part);
	} else {
		snprintf(buffer, len, "%s%d.%03d m", prefix, value / 1000, value % 1000);
	}
}

#if IS_ENABLED(CONFIG_ANGLE_HANDLER_SCANNER_8X16)
#define SCANNER_USE_8x16 1
#else
#define SCANNER_USE_8x16 0
#endif
#if IS_ENABLED(CONFIG_ANGLE_HANDLER_SHOW_XYZ)
#define SHOW_XYZ 1
#else
#define SHOW_XYZ 0
#endif
/* Row 1, 2, 3: 激光距离 + 超欠挖 + 坐标 (来自扫描仪数据) */
void mod_display_scanner(const scanner_data_t *s)
{
	char line[32] = {0};

	k_mutex_lock(&display_mutex, K_FOREVER);

/* Row 1 */
#if SCANNER_USE_8x16
	int_to_decimal_str(s->overbreak_value, s->overbreak_valid, line, sizeof(line), "OB: ");
	display_5x8_str_pad(" ", 0, 16, 128);
	display_8x16_str_pad(line, 0, 24, 128);
#else
	int_to_decimal_str(s->overbreak_value, s->overbreak_valid, line, sizeof(line), "OverBreak: ");
	display_5x8_str_pad(" ", 0, 16, 128);
	display_5x8_str_pad(line, 0, 24, 128);
#endif

#if SCANNER_USE_8x16
	int_to_decimal_str(s->laser_distance, s->laser_valid, line, sizeof(line), "Dis:");
	display_8x16_str_pad(line, 0, 40, 128);
#else
	int_to_decimal_str(s->laser_distance, s->laser_valid, line, sizeof(line), "Distance:  ");
	display_5x8_str_pad(line, 0, 32, 128);
#endif

/* Row 2 */
#if SHOW_XYZ
#if SCANNER_USE_8x16
	int_to_decimal_str(s->coord_x, s->coord_xy_valid, line, sizeof(line), "X:");
	display_8x16_str_pad(line, 0, 32, 64);
#else
	int_to_decimal_str(s->coord_x, s->coord_xy_valid, line, sizeof(line), "X axis:    ");
	display_5x8_str_pad(line, 0, 40, 128);
#endif

#if SCANNER_USE_8x16
	int_to_decimal_str(s->coord_y, s->coord_xy_valid, line, sizeof(line), "Y:");
	display_8x16_str_pad(line, 64, 32, 64);
#else
	int_to_decimal_str(s->coord_y, s->coord_xy_valid, line, sizeof(line), "Y axis:    ");
	display_5x8_str_pad(line, 0, 48, 128);
#endif

/* Row 3 */
#if SCANNER_USE_8x16
	int_to_decimal_str(s->coord_z, s->coord_z_valid, line, sizeof(line), "Z:");
	display_8x16_str_pad(line, 0, 48, 128);
#else
	int_to_decimal_str(s->coord_z, s->coord_z_valid, line, sizeof(line), "Z axis:    ");
	display_5x8_str_pad(line, 0, 56, 128);
#endif
#else
#if SCANNER_USE_8x16
	display_5x8_str_pad(" ", 0, 56, 128);
#else
	display_5x8_str_pad(line, 0, 40, 128);
	display_8x16_str_pad(line, 0, 48, 128);
#endif
#endif

	k_mutex_unlock(&display_mutex);
}

/* 全屏刷新 */
void mod_display_all(const global_params_t *params)
{
	if (params->connect_type == CAN_TYPE) {
		mod_display_can();
	} else {
		mod_display_rf24(params->rssi);
	}
	mod_display_battery(params->power_mv, params->battery_status);
	mod_display_scanner(&params->scanner);
}
/* ================================================================
 * 初始化
 * ================================================================ */
global_params_t global_params;

int mod_display_init(void)
{
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Device %s not found.", display_dev->name);
		return -1;
	}

	display_get_capabilities(display_dev, &capabilities);
	LOG_INF("Display %dx%d", capabilities.x_resolution, capabilities.y_resolution);

	display_blanking_off(display_dev);
	global_params.h_button = handler_get_btn();
	mod_display_all(&global_params);

	return 0;
}

SYS_INIT(mod_display_init, APPLICATION, 1);
