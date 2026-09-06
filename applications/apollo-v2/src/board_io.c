/* SPDX-License-Identifier: Apache-2.0
 * 板载基础 IO: LED / 按键 (input 子系统) / 蜂鸣器 (PCF8574) / ADC1 PA5
 *
 * LED/按键极性与偏置已在板 DTS 修正 (低有效 + 上拉, 见应用计划 §10.2)。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include "board_io.h"
#include "version.h"

LOG_MODULE_REGISTER(board_io, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)

static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(LED0_NODE, gpios),
	GPIO_DT_SPEC_GET(LED1_NODE, gpios),
};

/* 蜂鸣器: PCF8574 P0, 实测低 = 响。
 * PCF8574 输出状态跨 SWD 复位保持 —— 上电必须第一时间静音。 */
static const struct device *const exio_dev = DEVICE_DT_GET(DT_NODELABEL(exio));

#define BEEP_ON		0	/* P0 raw 低 = 响 */
#define BEEP_OFF	1	/* P0 raw 高 = 停 */

/* ---------------- 按键 (gpio-keys -> input 事件) ---------------- */
static const uint16_t key_codes[] = {
	INPUT_KEY_0,	/* WK_UP PA0 高有效 */
	INPUT_KEY_1,	/* KEY0  PH3 */
	INPUT_KEY_2,	/* KEY1  PH2 */
	INPUT_KEY_3,	/* KEY2  PC13 */
};
BUILD_ASSERT(ARRAY_SIZE(key_codes) == 4);

static uint32_t key_counts[ARRAY_SIZE(key_codes)];
static bool key_down[ARRAY_SIZE(key_codes)];

bool board_io_key_count(unsigned idx, uint32_t *count)
{
	if (idx >= ARRAY_SIZE(key_counts) || count == NULL) {
		return false;
	}
	*count = key_counts[idx];
	return true;
}

/* ---------------- ADC1 PA5 (INP19) ---------------- */
static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc1));

int board_io_adc_mv(void)
{
	static bool setup_done;
	static struct adc_channel_cfg ch_cfg = {
		.channel_id = 19,		/* PA5 = ADC1_INP19 */
		.gain = ADC_GAIN_1,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
	};
	uint16_t raw = 0;
	struct adc_sequence seq = {
		.channels = BIT(19),
		.buffer = &raw,
		.buffer_size = sizeof(raw),
		.resolution = 12,
	};
	int32_t mv;
	int ret;

	if (!device_is_ready(adc_dev)) {
		return -ENODEV;
	}
	if (!setup_done) {
		ret = adc_channel_setup(adc_dev, &ch_cfg);
		if (ret != 0) {
			return ret;
		}
		setup_done = true;
	}
	ret = adc_read(adc_dev, &seq);
	if (ret != 0) {
		return ret;
	}
	mv = raw;
	adc_raw_to_millivolts(3300, ADC_GAIN_1, 12, &mv);
	return mv;
}

/* ---------------- LED ---------------- */
int board_io_led_set(unsigned idx, bool on)
{
	if (idx >= ARRAY_SIZE(leds)) {
		return -EINVAL;
	}
	return gpio_pin_set_dt(&leds[idx], on);
}

int board_io_led_toggle(unsigned idx)
{
	if (idx >= ARRAY_SIZE(leds)) {
		return -EINVAL;
	}
	return gpio_pin_toggle_dt(&leds[idx]);
}

/* ---------------- 蜂鸣器 ---------------- */
int board_io_beep(uint16_t ms)
{
	int ret;

	if (!device_is_ready(exio_dev)) {
		return -ENODEV;
	}
	ret = gpio_pin_configure(exio_dev, 0, GPIO_OUTPUT);
	if (ret != 0) {
		return ret;
	}
	gpio_pin_set_raw(exio_dev, 0, BEEP_ON);
	k_msleep(ms);
	gpio_pin_set_raw(exio_dev, 0, BEEP_OFF);
	return 0;
}

int board_io_beeper_set(bool on)
{
	if (!device_is_ready(exio_dev)) {
		return -ENODEV;
	}
	gpio_pin_configure(exio_dev, 0, GPIO_OUTPUT);
	gpio_pin_set_raw(exio_dev, 0, on ? BEEP_ON : BEEP_OFF);
	return 0;
}

/* ---------------- 按键事件回调 ---------------- */
static void key_input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(key_codes); i++) {
		if (evt->code == key_codes[i]) {
			key_down[i] = evt->value != 0;
			if (evt->value) {
				key_counts[i]++;
				LOG_INF("KEY%zu pressed (%u)", i, key_counts[i]);
				if (i != 0) {
					/* KEY0/1/2 翻转绿色 LED (输入->输出演示) */
					board_io_led_toggle(1);
				}
			}
			break;
		}
	}
}
INPUT_CALLBACK_DEFINE(NULL, key_input_cb, NULL);

bool board_io_key_is_down(unsigned idx)
{
	return idx < ARRAY_SIZE(key_down) ? key_down[idx] : false;
}

/* ---------------- AP3216C 光照/接近 (I2C2 @0x1E) ---------------- */
#define AP3216C_ADDR		0x1E
#define AP3216C_REG_SYS_CONF	0x00
#define AP3216C_REG_ALS_DL	0x0C
#define AP3216C_REG_ALS_DH	0x0D
#define AP3216C_REG_PS_DL	0x0E
#define AP3216C_REG_PS_DH	0x0F

static const struct device *const als_i2c = DEVICE_DT_GET(DT_NODELABEL(i2c2));
static bool als_enabled;

static int ap3216_write_reg(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };

	return i2c_write(als_i2c, buf, sizeof(buf), AP3216C_ADDR);
}

static int ap3216_read_reg16(uint8_t reg_lo, uint16_t *val)
{
	uint8_t buf[2];
	int ret = i2c_write(als_i2c, &reg_lo, 1, AP3216C_ADDR);

	if (ret != 0) {
		return ret;
	}
	ret = i2c_read(als_i2c, buf, sizeof(buf), AP3216C_ADDR);
	if (ret != 0) {
		return ret;
	}
	*val = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
	return 0;
}

static int ap3216_ensure_enabled(void)
{
	if (als_enabled) {
		return 0;
	}
	if (!device_is_ready(als_i2c)) {
		return -ENODEV;
	}
	/* SYS_CONF = 0x03: ALS + PS 使能 */
	if (ap3216_write_reg(AP3216C_REG_SYS_CONF, 0x03) != 0) {
		return -ENODEV;
	}
	als_enabled = true;
	return 0;
}

int board_io_als_read(uint16_t *als)
{
	uint16_t raw;
	int ret = ap3216_ensure_enabled();

	if (ret != 0) {
		return ret;
	}
	ret = ap3216_read_reg16(AP3216C_REG_ALS_DL, &raw);
	if (ret == 0) {
		/* ALS DH 低 6 位有效, DL 高 4 位有效 -> range 0..0x3FFF */
		*als = (raw >> 4) & 0x3FFF;
	}
	return ret;
}

int board_io_ps_read(uint16_t *ps)
{
	uint16_t raw;
	int ret = ap3216_ensure_enabled();

	if (ret != 0) {
		return ret;
	}
	ret = ap3216_read_reg16(AP3216C_REG_PS_DL, &raw);
	if (ret == 0) {
		/* PS DH 低 2 位 + PS DL -> 10 位 */
		*ps = (raw >> 6) & 0x03FF;
	}
	return ret;
}

int board_io_init(void)
{

	/* PCF8574 状态跨复位保持: 静音必须最先做 (计划 §10.3 教训 #1)。
	 * 注: pcf857x 驱动对首次 GPIO_OUTPUT 配置会误报一条 ERR (驱动
	 * 内部 check 用 value 而非 mask), 但副作用已把引脚标记为输出,
	 * 随后的 set_raw 正常写入 —— hw-bringup 同样用法, 无功能影响 */
	if (device_is_ready(exio_dev)) {
		gpio_pin_configure(exio_dev, 0, GPIO_OUTPUT);
		gpio_pin_set_raw(exio_dev, 0, BEEP_OFF);
	} else {
		LOG_WRN("PCF8574 not ready, beeper unusable");
	}

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		if (!gpio_is_ready_dt(&leds[i])) {
			LOG_ERR("LED%zu device not ready", i);
			return -ENODEV;
		}
		gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
	}

	/* 依次点亮/熄灭一遍确认极性修复生效 (低有效: active = 亮) */
	board_io_led_set(0, true);
	board_io_led_set(1, true);
	k_msleep(100);
	board_io_led_set(0, false);
	board_io_led_set(1, false);

	LOG_INF("board_io ready (leds/keys/beeper/adc)");
	return 0;
}
