/*
 * Copyright (c) 2026 Titan Mini contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Titan Mini (Renesas RA8P1) LCD + 触摸 + LVGL 验证:
 *  - GLCDC 1024x600 RGB565, 帧缓冲在 SDRAM NOCACHE 区 (板级 dts)
 *  - GT911 触摸 (IIC0) -> LVGL 指针
 *  - 界面: 计数按钮 + 滑条, 触摸可交互
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

LOG_MODULE_REGISTER(titan_lvgl, LOG_LEVEL_INF);

static lv_obj_t *count_label;
static int counter;

static void btn_cb(lv_event_t *e)
{
	counter++;
	lv_label_set_text_fmt(count_label, "PRESSED: %d", counter);
}

static void slider_cb(lv_event_t *e)
{
	lv_obj_t *slider = lv_event_get_target(e);
	char buf[32];

	lv_snprintf(buf, sizeof(buf), "SLIDER: %d%%", (int)lv_slider_get_value(slider));
	lv_label_set_text(count_label, buf);
}

static void ui_init(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "Titan Mini RA8P1 LVGL");
	lv_obj_set_style_text_color(title, lv_color_white(), 0);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

	count_label = lv_label_create(scr);
	lv_label_set_text(count_label, "PRESSED: 0");
	lv_obj_set_style_text_color(count_label, lv_color_hex(0x00E0A0), 0);
	lv_obj_set_style_text_font(count_label, &lv_font_montserrat_28, 0);
	lv_obj_align(count_label, LV_ALIGN_TOP_MID, 0, 100);

	lv_obj_t *btn = lv_button_create(scr);
	lv_obj_set_size(btn, 300, 90);
	lv_obj_align(btn, LV_ALIGN_CENTER, 0, 40);
	lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *btn_label = lv_label_create(btn);
	lv_label_set_text(btn_label, "Press me");
	lv_obj_center(btn_label);

	lv_obj_t *slider = lv_slider_create(scr);
	lv_obj_set_width(slider, 700);
	lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -60);
	lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/*
 * GT911 恢复探测: ACK 但读全 0 的疑点是固件启动时间不足
 * (PHY 也需要 ~150ms). 策略: 长等待 + 软复位 + 分离事务.
 */
static void touch_recover(void)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(iic0));
	static const struct gpio_dt_spec ctp_rst = {
		.port = DEVICE_DT_GET(DT_NODELABEL(ioport4)),
		.pin = 12, /* CTP_RST P412, ATK FPC-40 pin35, 低有效 */
		.dt_flags = GPIO_ACTIVE_LOW,
	};
	static const struct gpio_dt_spec intp = {
		.port = DEVICE_DT_GET(DT_NODELABEL(ioport0)),
		.pin = 7, /* CTP_IRQ_N P007, ATK FPC-40 pin39 */
		.dt_flags = GPIO_ACTIVE_HIGH,
	};
	uint8_t id[4] = {0};
	uint8_t reg2[2] = {0x40, 0x81}; /* 0x8140 PID */
	int r, i;

	if (!device_is_ready(bus)) {
		return;
	}

	/* 复位: RST 拉低 -> INT 拉低(选 0x5D) -> RST 释放 -> 等 1s 固件启动 */
	gpio_pin_configure_dt(&intp, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&ctp_rst, GPIO_OUTPUT_ACTIVE);
	k_sleep(K_MSEC(20));
	gpio_pin_set_dt(&ctp_rst, 0); /* 释放复位, INT 保持低 */
	k_sleep(K_MSEC(5));
	gpio_pin_configure_dt(&intp, GPIO_INPUT);
	k_sleep(K_SECONDS(1));

	for (i = 0; i < 2; i++) {
		uint8_t addr = (i == 0) ? 0x5d : 0x14;

		r = i2c_write_read(bus, addr, reg2, 2, id, 4);
		LOG_INF("GT911@0x%02x settle-1s: r=%d id=%02x %02x %02x %02x", addr, r,
			id[0], id[1], id[2], id[3]);
		if (r == 0 && id[0] == 0x39 && id[1] == 0x31) {
			LOG_INF("PID OK ('911') @0x%02x", addr);
			return;
		}
	}

	/* 软复位尝试: 0x8040 <- 0x02, 等 60ms 再读 */
	{
		uint8_t sr[3] = {0x40, 0x80, 0x02};

		i2c_write(bus, sr, 3, 0x5d);
		k_sleep(K_MSEC(60));
		r = i2c_write_read(bus, 0x5d, reg2, 2, id, 4);
		LOG_INF("after soft-reset: r=%d id=%02x %02x %02x %02x", r,
			id[0], id[1], id[2], id[3]);
	}

	/* 分离事务 (write stop + read, 不用重复起始) */
	{
		r = i2c_write(bus, reg2, 2, 0x5d);
		r = i2c_read(bus, id, 4, 0x5d);
		LOG_INF("split txn: r=%d id=%02x %02x %02x %02x", r,
			id[0], id[1], id[2], id[3]);
	}
}

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	/* ATK 模块 FPC-40 LCD 复位: P011, 低有效 */
	static const struct gpio_dt_spec lcd_rst = {
		.port = DEVICE_DT_GET(DT_NODELABEL(ioport0)),
		.pin = 11,
		.dt_flags = GPIO_ACTIVE_LOW,
	};

	if (!device_is_ready(display)) {
		LOG_ERR("display not ready");
		return -ENODEV;
	}
	{
		struct display_capabilities caps;

		display_get_capabilities(display, &caps);
		LOG_INF("display ready, %ux%u", caps.x_resolution, caps.y_resolution);
	}

	gpio_pin_configure_dt(&lcd_rst, GPIO_OUTPUT_ACTIVE);
	k_sleep(K_MSEC(20));
	gpio_pin_set_dt(&lcd_rst, 0);
	k_sleep(K_MSEC(200));

	touch_recover();

	ui_init();
	LOG_INF("LVGL running, touch the screen");

	while (1) {
		lv_task_handler();
		k_sleep(K_MSEC(20));
	}

	return 0;
}
