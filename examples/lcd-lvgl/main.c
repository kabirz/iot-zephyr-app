/*
 * LVGL 9 测试程序 (TFTLCD 2.8" ILI9341, FMC 8080 16 位)
 *
 * 驱动栈: LVGL (zephyr port) -> display_ili9341 (ili9xxx)
 *         -> mipi_dbi_stm32_fmc (Intel 8080) -> FMC NE1 (板 DTS)
 *
 * 界面: 标题 + 计数标签 (500ms 自增) + 进度弧 + 三个按钮 + 色块;
 * shell 命令 `lvgl ping` 输出 LVGL 版本与显示分辨率。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <lvgl.h>
#include <stdio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lcd_lvgl, LOG_LEVEL_INF);

static lv_obj_t *count_label;
static lv_obj_t *arc;
static uint32_t count;

static void count_timer_cb(lv_timer_t *timer)
{
	ARG_UNUSED(timer);

	char buf[16];

	count++;
	snprintf(buf, sizeof(buf), "%u", count);
	lv_label_set_text(count_label, buf);
	lv_arc_set_value(arc, (int)(count % 100));
}

static int cmd_lv_ping(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(disp)) {
		shell_error(sh, "display not ready");
		return -ENODEV;
	}

	struct display_capabilities caps;

	display_get_capabilities(disp, &caps);
	shell_print(sh, "LVGL %d.%d.%d, %ux%u, %u bpp",
		    (int)lv_version_major(), (int)lv_version_minor(),
		    (int)lv_version_patch(),
		    caps.x_resolution, caps.y_resolution,
		    DISPLAY_BITS_PER_PIXEL(caps.current_pixel_format));
	return 0;
}
SHELL_CMD_REGISTER(lvgl, NULL, "lvgl/display info: lvgl ping", cmd_lv_ping);

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	struct display_capabilities caps;
	int ret;

	/* 背光 PB5 (显示栈无标准背光属性, 应用侧使能) */
	const struct device *gpiob = DEVICE_DT_GET(DT_NODELABEL(gpiob));

	if (device_is_ready(gpiob)) {
		gpio_pin_configure(gpiob, 5, GPIO_OUTPUT_ACTIVE);
	}

	if (!device_is_ready(display)) {
		LOG_ERR("display device not ready");
		return -ENODEV;
	}
	display_blanking_off(display);

	display_get_capabilities(display, &caps);
	LOG_INF("ILI9341 %ux%u ready (LVGL %d.%d.%d)",
		caps.x_resolution, caps.y_resolution,
		(int)lv_version_major(), (int)lv_version_minor(),
		(int)lv_version_patch());

	/* 标题 */
	lv_obj_t *title = lv_label_create(lv_screen_active());

	lv_label_set_text(title, "Apollo V2 / ILI9341");
	lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

	/* 计数器 */
	lv_obj_t *cnt_title = lv_label_create(lv_screen_active());

	lv_label_set_text(cnt_title, "uptime counter");
	lv_obj_align(cnt_title, LV_ALIGN_TOP_LEFT, 12, 56);

	count_label = lv_label_create(lv_screen_active());
	lv_obj_set_style_text_font(count_label, &lv_font_montserrat_24, 0);
	lv_label_set_text(count_label, "0");
	lv_obj_align(count_label, LV_ALIGN_TOP_LEFT, 12, 76);

	/* 进度弧 */
	arc = lv_arc_create(lv_screen_active());
	lv_obj_set_size(arc, 110, 110);
	lv_arc_set_range(arc, 0, 99);
	lv_arc_set_value(arc, 0);
	lv_obj_align(arc, LV_ALIGN_TOP_RIGHT, -8, 48);

	/* 三个按钮 (无触摸, 仅展示控件渲染) */
	static const char *btn_txt[] = { "OK", "ESC", "MENU" };

	for (int i = 0; i < 3; i++) {
		lv_obj_t *btn = lv_button_create(lv_screen_active());
		lv_obj_t *lbl = lv_label_create(btn);

		lv_label_set_text(lbl, btn_txt[i]);
		lv_obj_set_size(btn, 62, 36);
		lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, (i - 1) * 72, -16);
	}

	/* 底部色带 (红绿蓝黄青品红黑) */
	static const lv_color_t colors[] = {
		0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F, 0x0000,
	};

	for (size_t i = 0; i < ARRAY_SIZE(colors); i++) {
		lv_obj_t *c = lv_obj_create(lv_screen_active());

		lv_obj_set_size(c, 240 / 7 + 1, 10);
		lv_obj_set_style_bg_color(c, colors[i], 0);
		lv_obj_set_style_border_width(c, 0, 0);
		lv_obj_set_pos(c, (int)(i * 240 / 7), 310);
	}

	lv_timer_create(count_timer_cb, 500, NULL);

	LOG_INF("UI created, rendering ...");

	/* 本 fork 的 LVGL port 无自动刷新线程, 应用自跑事件循环 */
	while (true) {
		uint32_t next = lv_timer_handler();

		k_msleep(CLAMP(next, 1U, 20U));
	}

	return 0;
}
