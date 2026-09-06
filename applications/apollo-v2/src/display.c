/* SPDX-License-Identifier: Apache-2.0
 * LCD 双页 UI (TFTLCD 2.8" ILI9341 240x320, FMC 8080 16 位):
 *   LVGL 9 -> display_ili9341 -> mipi_dbi_stm32_fmc -> FMC NE1 (板 DTS)
 *
 * 主页: 状态栏(时间/日期/网络/CAN) + 输出卡片(红灯/绿灯/蜂鸣/背光)
 *       + 传感器卡片(电压/光照/接近) + 按键芯片(实时电平) + 文件浏览按钮
 * 文件页: 返回 + 当前路径 + 挂载切换 + 目录列表 (触摸进目录/..返回)
 *
 * 中文: apps/fonts/apollo_cjk_16.c (16px 4bpp, UI 用词 + ASCII, 编译内置;
 * 完整 GB2312 字库走 QSPI 字库分区属后续工作, 见计划 §10.4)
 * 注: 本 fork 的 LVGL port 无自动刷新线程, 应用自跑 lv_timer_handler。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "board_io.h"
#include "app_time.h"
#include "storage.h"
#include "net.h"
#include "font_qspi.h"
#ifdef CONFIG_APOLLO_CAN
#include "can.h"
#endif

LOG_MODULE_REGISTER(apollo_display, LOG_LEVEL_INF);

extern const lv_font_t apollo_cjk_16;

/* UI 字体: 优先 QSPI 全量中文字库 (memmap 直读), 未安装回退编译内置的
 * UI 用词字库 (apps/fonts/apollo_cjk_16.c, 文件名生僻字会缺字) */
static const lv_font_t *ui_font(void)
{
	return apollo_font_qspi_ready() ? &apollo_font_qspi : &apollo_cjk_16;
}

/* ---------------- 深色主题配色 (设计稿) ---------------- */
#define COL_BG		lv_color_hex(0x0d1420)	/* 页面背景 */
#define COL_CARD	lv_color_hex(0x1a2436)	/* 卡片背景 */
#define COL_CARD_ON	lv_color_hex(0x22304a)	/* 卡片高亮(开启) */
#define COL_BORDER	lv_color_hex(0x2a3850)	/* 边框 */
#define COL_ACCENT	lv_color_hex(0x3b82f6)	/* 主色 */
#define COL_ON		lv_color_hex(0x10b981)	/* 开启状态 */
#define COL_OFF		lv_color_hex(0x5b6b82)	/* 关闭状态 */
#define COL_TEXT	lv_color_hex(0xe8eef7)	/* 主文字 */
#define COL_SUB		lv_color_hex(0x8fa3bd)	/* 次要文字 */

static lv_obj_t *scr_home;
static lv_obj_t *scr_files;

static lv_obj_t *label_time;
static lv_obj_t *label_date;
static lv_obj_t *icon_net;
static lv_obj_t *icon_can;

/* 输出卡片 */
enum output_idx {
	OUT_RED = 0,
	OUT_GREEN,
	OUT_BEEP,
	OUT_BL,
	OUT_COUNT,
};

struct out_card {
	lv_obj_t *obj;
	lv_obj_t *label;	/* 名称 */
	lv_obj_t *state;	/* 开/关 */
	bool on;
};

static struct out_card out_cards[OUT_COUNT];

/* 传感器卡片 */
static lv_obj_t *sens_val[3];
static lv_obj_t *sens_bar[2];	/* 电压/光照带进度条 */

/* 按键芯片 */
static lv_obj_t *key_chips[4];

/* 文件页 */
static lv_obj_t *files_list;
static lv_obj_t *label_path;
static lv_obj_t *label_count;
static lv_obj_t *mnt_btns[2];
static char cur_path[128] = "/SD:";

/* ---------------- 通用小工具 ---------------- */
static void style_card(lv_obj_t *obj, int32_t w, int32_t h)
{
	lv_obj_set_size(obj, w, h);
	lv_obj_set_style_bg_color(obj, COL_CARD, 0);
	lv_obj_set_style_border_color(obj, COL_BORDER, 0);
	lv_obj_set_style_border_width(obj, 1, 0);
	lv_obj_set_style_radius(obj, 4, 0);
	lv_obj_set_style_pad_all(obj, 0, 0);
}

static lv_obj_t *label_make(lv_obj_t *parent, const char *text,
			    const lv_font_t *font, lv_color_t color)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, text);
	if (font != NULL) {
		lv_obj_set_style_text_font(l, font, 0);
	}
	lv_obj_set_style_text_color(l, color, 0);
	return l;
}

/* ---------------- 输出控制 ---------------- */
static void output_apply(unsigned idx, bool on)
{
	switch (idx) {
	case OUT_RED:
		board_io_led_set(0, on);
		break;
	case OUT_GREEN:
		board_io_led_set(1, on);
		break;
	case OUT_BEEP:
		board_io_beeper_set(on);
		break;
	case OUT_BL:
		apollo_display_backlight_set(on);
		break;
	}
	out_cards[idx].on = on;
	lv_obj_set_style_border_color(out_cards[idx].obj,
				      on ? COL_ON : COL_BORDER, 0);
	lv_obj_set_style_bg_color(out_cards[idx].obj,
				  on ? COL_CARD_ON : COL_CARD, 0);
	lv_label_set_text(out_cards[idx].state, on ? "开" : "关");
	lv_obj_set_style_text_color(out_cards[idx].state,
				    on ? COL_ON : COL_OFF, 0);
}

static void out_card_cb(lv_event_t *e)
{
	unsigned idx = (uintptr_t)lv_event_get_user_data(e);

	output_apply(idx, !out_cards[idx].on);
}

/* ---------------- 状态栏 / 传感器 刷新 ---------------- */
static void status_timer_cb(lv_timer_t *t)
{
	ARG_UNUSED(t);
	struct tm tm;
	char buf[24];

	if (app_time_get_local(&tm)) {
		snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
			 tm.tm_hour, tm.tm_min, tm.tm_sec);
		lv_label_set_text(label_time, buf);
		snprintf(buf, sizeof(buf), "%02d/%02d",
			 tm.tm_mon + 1, tm.tm_mday);
		lv_label_set_text(label_date, buf);
	} else {
		lv_label_set_text(label_time, "--:--:--");
		lv_label_set_text(label_date, "--/--");
	}

	/* 网络 / CAN 图标: 绿 = 就绪, 灰 = 离线 */
	lv_obj_set_style_text_color(icon_net,
				    net_ready() ? COL_ON : COL_OFF, 0);
#ifdef CONFIG_APOLLO_CAN
	lv_obj_set_style_text_color(icon_can, can_is_up() ? COL_ON : COL_OFF, 0);
#endif
}

static void sens_timer_cb(lv_timer_t *t)
{
	ARG_UNUSED(t);
	char buf[24];
	int mv = board_io_adc_mv();
	uint16_t als = 0, ps = 0;
	bool als_ok = board_io_als_read(&als) == 0;
	bool ps_ok = board_io_ps_read(&ps) == 0;

	if (mv >= 0) {
		snprintf(buf, sizeof(buf), "%d", mv);
		lv_label_set_text(sens_val[0], buf);
		lv_bar_set_value(sens_bar[0], CLAMP(mv, 0, 3300), LV_ANIM_OFF);
	}
	if (als_ok) {
		snprintf(buf, sizeof(buf), "%u", als);
		lv_label_set_text(sens_val[1], buf);
		lv_bar_set_value(sens_bar[1], CLAMP(als, 0, 3000), LV_ANIM_OFF);
	} else {
		lv_label_set_text(sens_val[1], "--");
	}
	if (ps_ok) {
		snprintf(buf, sizeof(buf), "%u", ps);
		lv_label_set_text(sens_val[2], buf);
	} else {
		lv_label_set_text(sens_val[2], "--");
	}
}

static void key_timer_cb(lv_timer_t *t)
{
	ARG_UNUSED(t);

	for (int i = 0; i < 4; i++) {
		lv_obj_set_style_bg_color(key_chips[i],
					  board_io_key_is_down(i) ? COL_ON
								  : COL_CARD,
					  0);
	}
}

/* ---------------- 文件浏览 ---------------- */
static void files_reload(void);

static void back_home_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	lv_scr_load(scr_home);
}

static void files_btn_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	files_reload();
	lv_scr_load(scr_files);
}

static void mount_btn_cb(lv_event_t *e)
{
	const char *mnt = lv_event_get_user_data(e);

	strncpy(cur_path, mnt, sizeof(cur_path) - 1);
	cur_path[sizeof(cur_path) - 1] = '\0';
	/* 高亮当前挂载点 */
	for (int i = 0; i < 2; i++) {
		lv_obj_set_style_bg_color(mnt_btns[i],
					  strcmp(cur_path, "/SD:") == 0 ?
					  (i == 0 ? COL_ACCENT : COL_CARD_ON) :
					  (i == 1 ? COL_ACCENT : COL_CARD_ON), 0);
	}
	files_reload();
}

/* 挂载点根路径 ("/SD:" / "/QSPI:") */
static bool is_root_path(const char *p)
{
	return strcmp(p, "/SD:") == 0 || strcmp(p, "/QSPI:") == 0;
}

/* 挂载点是否可用 */
static bool mount_ready(const char *p)
{
	const struct apollo_storage_info *si = storage_info();

	if (strncmp(p, "/SD", 3) == 0) {
		return si->sd_mounted;
	}
	if (strncmp(p, "/QSPI", 5) == 0) {
		return si->qspi_fs_mounted;
	}
	return false;
}

struct dir_entry_info {
	bool is_dir;
	char name[64];
};

static void dir_entry_cb(lv_event_t *e)
{
	const struct dir_entry_info *ent = lv_event_get_user_data(e);

	if (!ent->is_dir) {
		return;
	}
	if (strcmp(ent->name, "..") == 0) {
		/* 去掉最后一级; 保留挂载点 "/SD:" / "/QSPI:" */
		size_t len = strlen(cur_path);

		while (len > 0 && cur_path[len - 1] != '/') {
			len--;
		}
		if (len > 0) {
			len--;
		}
		cur_path[len] = '\0';
		if (strcmp(cur_path, "/SD") == 0) {
			strcpy(cur_path, "/SD:");
		} else if (strcmp(cur_path, "/QSPI") == 0) {
			strcpy(cur_path, "/QSPI:");
		}
	} else {
		size_t len = strlen(cur_path);

		if (len > 0 && cur_path[len - 1] != ':') {
			strncat(cur_path, "/", sizeof(cur_path) - len - 1);
		}
		strncat(cur_path, ent->name,
			sizeof(cur_path) - strlen(cur_path) - 1);
	}
	files_reload();
}

static void files_reload(void)
{
	static struct dir_entry_info entries[40];
	unsigned count = 0;
	struct fs_dir_t dir;
	struct fs_dirent ent;
	char buf[80];

	lv_obj_clean(files_list);
	fs_dir_t_init(&dir);
	lv_label_set_text(label_path, cur_path);

	/* 手动行布局 (本配置未启用 flex, lv_list 会全部堆叠在 (0,0)) */
	static char texts[44][72];
	struct {
		const char *icon;
		struct dir_entry_info *ent;	/* NULL = 提示行 */
	} rows[44];
	unsigned nrow = 0;

	if (!mount_ready(cur_path)) {
		rows[nrow].icon = LV_SYMBOL_CLOSE;
		strncpy(texts[nrow], strncmp(cur_path, "/QSPI", 5) == 0 ?
			"QSPI 未挂载" : "TF 未挂载", sizeof(texts[0]) - 1);
		rows[nrow].ent = NULL;
		nrow++;
		goto fill;
	}

	if (fs_opendir(&dir, cur_path) != 0) {
		rows[nrow].icon = LV_SYMBOL_WARNING;
		strncpy(texts[nrow], "打开目录失败", sizeof(texts[0]) - 1);
		rows[nrow].ent = NULL;
		nrow++;
		goto fill;
	}

	if (!is_root_path(cur_path)) {
		entries[count].is_dir = true;
		strcpy(entries[count].name, "..");
		count++;
	}
	/* 第一遍: 目录; 第二遍: 文件 (目录在前, 与设计稿一致) */
	while (count < 40 && fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
		if (ent.type == FS_DIR_ENTRY_DIR && strcmp(ent.name, ".") != 0 &&
		    strcmp(ent.name, "..") != 0) {
			entries[count].is_dir = true;
			strncpy(entries[count].name, ent.name,
				sizeof(entries[0].name) - 1);
			entries[count].name[sizeof(entries[0].name) - 1] = '\0';
			count++;
		}
	}
	fs_closedir(&dir);

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, cur_path) == 0) {
		while (count < 40 && fs_readdir(&dir, &ent) == 0 &&
		       ent.name[0] != '\0') {
			if (ent.type == FS_DIR_ENTRY_FILE) {
				entries[count].is_dir = false;
				strncpy(entries[count].name, ent.name,
					sizeof(entries[0].name) - 1);
				entries[count].name[sizeof(entries[0].name) - 1] = '\0';
				count++;
			}
		}
		fs_closedir(&dir);
	}

	for (unsigned i = 0; i < count; i++) {
		rows[nrow].icon = entries[i].is_dir ? LV_SYMBOL_DIRECTORY
						    : LV_SYMBOL_FILE;
		snprintf(texts[nrow], sizeof(texts[0]), "%s%s", entries[i].name,
			 entries[i].is_dir ? "  <dir>" : "");
		rows[nrow].ent = &entries[i];
		nrow++;
	}

	if (count == 0) {
		rows[nrow].icon = LV_SYMBOL_REFRESH;
		strncpy(texts[nrow], "目录为空", sizeof(texts[0]) - 1);
		rows[nrow].ent = NULL;
		nrow++;
	}

fill:
	for (unsigned i = 0; i < nrow; i++) {
		lv_obj_t *btn = lv_button_create(files_list);

		lv_obj_set_size(btn, 228, 26);
		lv_obj_set_pos(btn, 6, (int32_t)i * 28);
		lv_obj_set_style_bg_color(btn, COL_CARD, 0);
		lv_obj_set_style_radius(btn, 3, 0);
		lv_obj_set_style_pad_all(btn, 0, 0);

		lv_obj_t *ic = label_make(btn, rows[i].icon, NULL, COL_ACCENT);

		lv_obj_set_pos(ic, 6, 5);

		lv_obj_t *lbl = label_make(btn, texts[i], ui_font(), COL_TEXT);

		lv_obj_set_pos(lbl, 26, 4);
		lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
		lv_obj_set_width(lbl, 196);
		lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);

		if (rows[i].ent != NULL && rows[i].ent->is_dir) {
			lv_obj_add_event_cb(btn, dir_entry_cb, LV_EVENT_CLICKED,
					    rows[i].ent);
		}
	}

	snprintf(buf, sizeof(buf), "%u 项", count);
	lv_label_set_text(label_count, buf);
}

/* ---------------- 构建主页 ---------------- */
static void build_status_bar(lv_obj_t *scr)
{
	lv_obj_t *bar = lv_obj_create(scr);

	lv_obj_set_size(bar, 240, 30);
	lv_obj_set_pos(bar, 0, 0);
	lv_obj_set_style_bg_color(bar, COL_CARD, 0);
	lv_obj_set_style_border_width(bar, 0, 0);
	lv_obj_set_style_radius(bar, 0, 0);
	lv_obj_set_style_pad_all(bar, 0, 0);

	label_time = label_make(bar, "--:--:--", ui_font(), COL_TEXT);
	lv_obj_align(label_time, LV_ALIGN_LEFT_MID, 8, 0);

	label_date = label_make(bar, "--/--", ui_font(), COL_SUB);
	lv_obj_align(label_date, LV_ALIGN_LEFT_MID, 92, 0);

	icon_net = label_make(bar, LV_SYMBOL_WIFI, NULL, COL_OFF);
	lv_obj_align(icon_net, LV_ALIGN_RIGHT_MID, -30, 0);

	icon_can = label_make(bar, LV_SYMBOL_SETTINGS, NULL, COL_OFF);
	lv_obj_align(icon_can, LV_ALIGN_RIGHT_MID, -8, 0);
}

static void build_home(void)
{
	static const char *out_names[OUT_COUNT] = { "红灯", "绿灯", "蜂鸣", "背光" };
	static const lv_point_t out_pos[OUT_COUNT] = {
		{12, 56}, {122, 56}, {12, 92}, {122, 92},
	};

	scr_home = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_home, COL_BG, 0);
	lv_obj_set_scroll_dir(scr_home, LV_DIR_NONE);

	build_status_bar(scr_home);

	lv_obj_t *sec1 = label_make(scr_home, "输 出", ui_font(), COL_SUB);

	lv_obj_set_pos(sec1, 12, 36);

	for (int i = 0; i < OUT_COUNT; i++) {
		struct out_card *c = &out_cards[i];

		c->obj = lv_obj_create(scr_home);
		style_card(c->obj, 110, 30);
		lv_obj_set_pos(c->obj, out_pos[i].x, out_pos[i].y);
		lv_obj_add_flag(c->obj, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(c->obj, out_card_cb, LV_EVENT_CLICKED,
				    (void *)(uintptr_t)i);

		c->label = label_make(c->obj, out_names[i], ui_font(), COL_TEXT);
		lv_obj_align(c->label, LV_ALIGN_LEFT_MID, 8, 0);

		c->state = label_make(c->obj, "关", ui_font(), COL_OFF);
		lv_obj_align(c->state, LV_ALIGN_RIGHT_MID, -8, 0);
		c->on = false;
	}

	lv_obj_t *sec2 = label_make(scr_home, "传 感", ui_font(), COL_SUB);

	lv_obj_set_pos(sec2, 12, 128);

	static const char *sens_names[3] = { "电压mV", "光照", "接近" };

	for (int i = 0; i < 3; i++) {
		lv_obj_t *card = lv_obj_create(scr_home);

		style_card(card, 72, 54);
		lv_obj_set_pos(card, 8 + i * 75, 148);

		lv_obj_t *name = label_make(card, sens_names[i],
					    ui_font(), COL_SUB);

		lv_obj_set_pos(name, 4, 3);

		sens_val[i] = label_make(card, "--", ui_font(), COL_TEXT);
		lv_obj_set_pos(sens_val[i], 4, 22);

		if (i < 2) {
			sens_bar[i] = lv_bar_create(card);
			lv_obj_set_size(sens_bar[i], 62, 5);
			lv_obj_set_pos(sens_bar[i], 5, 44);
			lv_bar_set_range(sens_bar[i], 0, 100);
			lv_obj_set_style_bg_color(sens_bar[i], COL_BORDER,
						  LV_PART_MAIN);
			lv_obj_set_style_bg_color(sens_bar[i], COL_ACCENT,
						  LV_PART_INDICATOR);
		}
	}

	lv_obj_t *sec3 = label_make(scr_home, "按 键", ui_font(), COL_SUB);

	lv_obj_set_pos(sec3, 12, 208);

	static const char *key_names[4] = { "上键", "键0", "键1", "键2" };

	for (int i = 0; i < 4; i++) {
		key_chips[i] = lv_obj_create(scr_home);
		style_card(key_chips[i], 53, 30);
		lv_obj_set_pos(key_chips[i], 12 + i * 57, 226);

		lv_obj_t *l = label_make(key_chips[i], key_names[i],
					 ui_font(), COL_TEXT);

		lv_obj_center(l);
	}

	lv_obj_t *files_btn = lv_button_create(scr_home);

	lv_obj_set_size(files_btn, 216, 36);
	lv_obj_set_pos(files_btn, 12, 276);
	lv_obj_set_style_bg_color(files_btn, COL_ACCENT, 0);
	lv_obj_add_event_cb(files_btn, files_btn_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *fb_label = lv_label_create(files_btn);

	lv_label_set_text(fb_label, LV_SYMBOL_DIRECTORY "  文件浏览");
	lv_obj_set_style_text_font(fb_label, ui_font(), 0);
	lv_obj_center(fb_label);
}

/* ---------------- 构建文件页 ---------------- */
static void build_files(void)
{
	static const char *mnt_names[2] = { "/SD:", "/QSPI:" };

	scr_files = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_files, COL_BG, 0);
	lv_obj_set_scroll_dir(scr_files, LV_DIR_NONE);

	/* 顶栏: 返回 + 路径 + 计数 */
	lv_obj_t *bar = lv_obj_create(scr_files);

	lv_obj_set_size(bar, 240, 30);
	lv_obj_set_pos(bar, 0, 0);
	lv_obj_set_style_bg_color(bar, COL_CARD, 0);
	lv_obj_set_style_border_width(bar, 0, 0);
	lv_obj_set_style_radius(bar, 0, 0);
	lv_obj_set_style_pad_all(bar, 0, 0);

	lv_obj_t *back = lv_button_create(bar);

	lv_obj_set_size(back, 40, 26);
	lv_obj_set_pos(back, 2, 2);
	lv_obj_set_style_bg_color(back, COL_CARD_ON, 0);
	lv_obj_add_event_cb(back, back_home_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *bl = lv_label_create(back);

	lv_label_set_text(bl, LV_SYMBOL_LEFT);
	lv_obj_center(bl);

	label_path = label_make(bar, "/SD:", ui_font(), COL_TEXT);
	lv_obj_align(label_path, LV_ALIGN_LEFT_MID, 50, 0);

	label_count = label_make(bar, "", ui_font(), COL_SUB);
	lv_obj_align(label_count, LV_ALIGN_RIGHT_MID, -6, 0);

	/* 挂载点切换 */
	for (int i = 0; i < 2; i++) {
		mnt_btns[i] = lv_button_create(scr_files);

		lv_obj_t *b = mnt_btns[i];

		lv_obj_set_size(b, 108, 26);
		lv_obj_set_pos(b, 12 + i * 112, 34);
		lv_obj_set_style_bg_color(b, i == 0 ? COL_ACCENT : COL_CARD_ON, 0);
		lv_obj_add_event_cb(b, mount_btn_cb, LV_EVENT_CLICKED,
				    (void *)mnt_names[i]);
		lv_obj_t *l = lv_label_create(b);

		lv_label_set_text(l, mnt_names[i]);
		lv_obj_center(l);
	}

	files_list = lv_obj_create(scr_files);
	lv_obj_set_size(files_list, 240, 254);
	lv_obj_set_pos(files_list, 0, 66);
	lv_obj_set_style_bg_color(files_list, COL_BG, 0);
	lv_obj_set_style_border_width(files_list, 0, 0);
	lv_obj_set_style_pad_all(files_list, 0, 0);
	lv_obj_set_scroll_dir(files_list, LV_DIR_VER);
}

/* ---------------- 背光 ---------------- */
static const struct gpio_dt_spec lcd_bl =
	GPIO_DT_SPEC_GET(DT_NODELABEL(lcd_bl), gpios);

int apollo_display_backlight_set(bool on)
{
	if (!gpio_is_ready_dt(&lcd_bl)) {
		return -ENODEV;
	}
	gpio_pin_set_dt(&lcd_bl, on);
	return 0;
}

/* ---------------- 入口 ---------------- */
#define DISPLAY_STACK_SIZE 8192
#define DISPLAY_PRIORITY 5

static K_THREAD_STACK_DEFINE(display_stack, DISPLAY_STACK_SIZE);
static struct k_thread display_thread;

static void display_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (gpio_is_ready_dt(&lcd_bl)) {
		gpio_pin_configure_dt(&lcd_bl, GPIO_OUTPUT_ACTIVE);
	}

	if (!device_is_ready(disp)) {
		LOG_ERR("display device not ready");
		return;
	}
	display_blanking_off(disp);

	/* 预热 QSPI 字库 (决定 UI 用哪套字体) */
	(void)apollo_font_qspi_ready();

	build_home();
	build_files();
	lv_scr_load(scr_home);

	lv_timer_create(status_timer_cb, 1000, NULL);
	lv_timer_create(sens_timer_cb, 500, NULL);
	lv_timer_create(key_timer_cb, 100, NULL);
	status_timer_cb(NULL);
	sens_timer_cb(NULL);

	LOG_INF("LCD UI up (ILI9341 240x320, 主页+文件浏览双页)");

	/* 本 fork 的 LVGL port 无自动刷新线程 */
	for (;;) {
		uint32_t next = lv_timer_handler();

		k_msleep(CLAMP(next, 1U, 20U));
	}
}

int apollo_display_start(void)
{
	k_thread_create(&display_thread, display_stack,
			K_THREAD_STACK_SIZEOF(display_stack),
			display_thread_fn, NULL, NULL, NULL,
			DISPLAY_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&display_thread, "apollo_display");
	return 0;
}
