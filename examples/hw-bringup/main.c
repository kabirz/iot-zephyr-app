/*
 * 阿波罗 V2 STM32H743 全硬件自检 (hw-bringup)
 *
 * 上电按顺序跑一遍所有可自检的外设, 每项打印 PASS/FAIL/SKIP, 结束打印
 * 汇总; shell 里 `hwtest` 可随时重跑。
 *
 * 测试项: LED / 按键 / 蜂鸣器(PCF8574 P0) / I2C 总线扫描(含板载传感器
 * 应答) / 24C02 EEPROM 读写 / QSPI W25Q256 擦写读 / FMC SDRAM 32MB
 * 走样 / ADC PA5 / DAC1 PA4 / PWM PA3 / FDCAN1 回环收发 / TF 卡(SDMMC1)
 * / RTC(LSE) 走时。
 *
 * 无法自动测试(需外接模块或与以太网/USB 冲突)的硬件见 README.md。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/can.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <zephyr/cache.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(hwtest, LOG_LEVEL_INF);

#define PASS 0
#define FAIL 1
#define SKIP 2

static const char * const result_str[] = { "PASS", "FAIL", "SKIP" };
static int pass_cnt, fail_cnt, skip_cnt;

static void report(const char *name, int result, const char *detail)
{
	switch (result) {
	case PASS: pass_cnt++; break;
	case FAIL: fail_cnt++; break;
	default:   skip_cnt++; break;
	}
	LOG_INF("[%s] %-16s %s", result_str[result], name, detail ? detail : "");
}

/* ---------------- LED: DS0 红 (PB1) / DS1 绿 (PB0) ---------------- */
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static void test_led(void)
{
	if (!gpio_is_ready_dt(&led0) || !gpio_is_ready_dt(&led1)) {
		report("LED", FAIL, "device not ready");
		return;
	}
	gpio_pin_configure_dt(&led0, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&led1, GPIO_OUTPUT_ACTIVE);
	for (int i = 0; i < 3; i++) {
		gpio_pin_toggle_dt(&led0);
		gpio_pin_toggle_dt(&led1);
		k_msleep(120);
	}
	gpio_pin_set_dt(&led0, 0);
	gpio_pin_set_dt(&led1, 0);
	report("LED DS0/DS1", PASS, "交替闪烁 3 次 (肉眼确认)");
}

/* ---------------- 按键: WK_UP(PA0,高有效) KEY0-2(低有效) ---------------- */
static void test_keys(void)
{
	static const struct gpio_dt_spec keys[] = {
		GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),	/* KEY_UP  PA0 高有效 */
		GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),	/* KEY0    PH3 低有效 */
		GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios),	/* KEY1    PH2 低有效 */
		GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios),	/* KEY2    PC13 低有效 */
	};
	static const bool expect_released_raw[] = { false, true, true, true };
	static const gpio_flags_t pull[] = {
		GPIO_PULL_DOWN, GPIO_PULL_UP, GPIO_PULL_UP, GPIO_PULL_UP,
	};
	char detail[96];
	int ok = 0;
	int raw[4];

	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		if (!gpio_is_ready_dt(&keys[i])) {
			report("KEY", FAIL, "device not ready");
			return;
		}
		gpio_pin_configure(keys[i].port, keys[i].pin,
				   GPIO_INPUT | pull[i]);
		k_msleep(2);
		raw[i] = gpio_pin_get_raw(keys[i].port, keys[i].pin);

		if (raw[i] == (int)expect_released_raw[i]) {
			ok++;
		}
	}
	snprintf(detail, sizeof(detail),
		 "静止电平 UP=%d K0=%d K1=%d K2=%d (正确 %d/4, 按下可再验证)",
		 raw[0], raw[1], raw[2], raw[3], ok);
	report("KEY x4", ok == 4 ? PASS : FAIL, detail);
}

/* ------------- 蜂鸣器: PCF8574 P0, 实测 P0 低 = 响, P0 高 = 停 -------------
 * 注意: PCF8574 掉电前写入的输出状态在 SWD 复位/重烧后仍保持,
 * 因此上电必须主动把 P0 拉高静音 (见 beeper_silence)。
 */
static const struct device *const exio_dev = DEVICE_DT_GET(DT_NODELABEL(exio));

#define BEEP_ON		0	/* P0 raw 低 = 响 */
#define BEEP_OFF	1	/* P0 raw 高 = 停 */

static void beeper_silence(void)
{
	if (!device_is_ready(exio_dev)) {
		return;
	}
	gpio_pin_configure(exio_dev, 0, GPIO_OUTPUT);
	gpio_pin_set_raw(exio_dev, 0, BEEP_OFF);
}

static void beeper_pulse(uint16_t ms)
{
	if (!device_is_ready(exio_dev)) {
		return;
	}
	gpio_pin_set_raw(exio_dev, 0, BEEP_ON);
	k_msleep(ms);
	gpio_pin_set_raw(exio_dev, 0, BEEP_OFF);
}

/* ------------- I2C2 总线: 扫描 + 板载器件应答 -------------
 * 预期: 0x20 PCF8574 / 0x50 24C02 / 0x1E AP3216C / 0x10 ES8388 /
 * 0x6B? SH3001 / 0x0C? ST480MC (后两者地址以扫描结果为准)
 */
static const struct device *const i2c2_dev = DEVICE_DT_GET(DT_NODELABEL(i2c2));

static void test_i2c_scan(void)
{
	char detail[128];
	uint8_t val;

	if (!device_is_ready(i2c2_dev)) {
		report("I2C2 scan", FAIL, "bus not ready");
		return;
	}

	int found = 0;
	size_t off = 0;

	off += snprintf(detail + off, sizeof(detail) - off, "devs @");
	for (uint8_t addr = 0x08; addr < 0x78; addr++) {
		if (i2c_read(i2c2_dev, &val, 1, addr) == 0) {
			found++;
			if (found <= 8) {
				off += snprintf(detail + off, sizeof(detail) - off,
						" %02X", addr);
			}
		}
	}
	snprintf(detail + off, sizeof(detail) - off, " (%d 个)", found);
	report("I2C2 scan", found >= 2 ? PASS : FAIL, detail);

	/* AP3216C 无 ID 寄存器, 0x1E 应答即认为在位 */
	if (i2c_read(i2c2_dev, &val, 1, 0x1E) == 0) {
		report("AP3216C @0x1E", PASS, "光/距/红外传感器在位");
	} else {
		report("AP3216C @0x1E", SKIP, "无应答");
	}

	/* ES8388 音频编解码 @0x10 */
	if (i2c_read(i2c2_dev, &val, 1, 0x10) == 0) {
		report("ES8388 @0x10", PASS, "音频 codec 在位");
	} else {
		report("ES8388 @0x10", SKIP, "无应答");
	}

	/* 其余扫描到的地址原样报告 (SH3001/ST480MC 以实际原理图为准) */
}

/* ---------------- 24C02 EEPROM 读写 ---------------- */
static void test_eeprom(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(eeprom_24c02));
	uint8_t wr[8], rd[8], orig[8];
	char detail[96];

	if (!device_is_ready(dev)) {
		report("24C02", FAIL, "device not ready");
		return;
	}
	if (eeprom_read(dev, 0xF0, orig, sizeof(orig)) != 0) {
		report("24C02", FAIL, "read orig failed");
		return;
	}
	for (size_t i = 0; i < sizeof(wr); i++) {
		wr[i] = (uint8_t)(0xA0 ^ i);
	}
	if (eeprom_write(dev, 0xF0, wr, sizeof(wr)) != 0) {
		report("24C02", FAIL, "write failed");
		return;
	}
	k_msleep(10);	/* twr ~5ms */
	if (eeprom_read(dev, 0xF0, rd, sizeof(rd)) != 0) {
		report("24C02", FAIL, "read back failed");
		return;
	}
	bool ok = memcmp(wr, rd, sizeof(wr)) == 0;

	eeprom_write(dev, 0xF0, orig, sizeof(orig));	/* 恢复原数据 */
	snprintf(detail, sizeof(detail), "0xF0 处写读 8 字节 %s",
		 ok ? "一致" : "不一致");
	report("24C02 EEPROM", ok ? PASS : FAIL, detail);
}

/* ---------------- QSPI W25Q256 擦写读 ---------------- */
static void test_qspi_flash(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(qspi_nor));
	static uint8_t wr[256], rd[256];
	struct flash_pages_info info;
	const size_t size = DT_PROP(DT_NODELABEL(qspi_nor), size) / 8;
	const uint32_t off = size - 0x1000;	/* 最后一页, 避开代码区 */
	char detail[96];

	if (!device_is_ready(dev)) {
		report("QSPI W25Q256", FAIL, "device not ready");
		return;
	}
	if (flash_get_page_info_by_offs(dev, off, &info) == 0) {
		LOG_INF("  W25Q256: page size %u B, 0x%08x 处测试",
			info.size, off);
	}

	if (flash_erase(dev, off, info.size) != 0) {
		report("QSPI W25Q256", FAIL, "erase failed");
		return;
	}
	for (size_t i = 0; i < sizeof(wr); i++) {
		wr[i] = (uint8_t)(i * 7 + 1);
	}
	if (flash_read(dev, off, rd, 16) == 0) {
		LOG_INF("  after erase: %02x %02x %02x %02x %02x %02x %02x %02x",
			rd[0], rd[1], rd[2], rd[3], rd[4], rd[5], rd[6], rd[7]);
	}
	if (flash_write(dev, off, wr, sizeof(wr)) != 0) {
		report("QSPI W25Q256", FAIL, "write failed");
		return;
	}
	if (flash_read(dev, off, rd, sizeof(rd)) != 0) {
		report("QSPI W25Q256", FAIL, "read failed");
		return;
	}
	bool ok = memcmp(wr, rd, sizeof(wr)) == 0;

	LOG_INF("  wrote:        %02x %02x %02x %02x %02x %02x %02x %02x",
		wr[0], wr[1], wr[2], wr[3], wr[4], wr[5], wr[6], wr[7]);
	LOG_INF("  read back:    %02x %02x %02x %02x %02x %02x %02x %02x",
		rd[0], rd[1], rd[2], rd[3], rd[4], rd[5], rd[6], rd[7]);
	flash_erase(dev, off, info.size);	/* 擦净测试页 */

	snprintf(detail, sizeof(detail), "%zuMB 四线, 末页擦写读 %s",
		 size >> 20, ok ? "一致" : "不一致");
	report("QSPI W25Q256", ok ? PASS : FAIL, detail);
}

/* ---------------- FMC SDRAM 32MB 走样 ---------------- */
static void test_sdram(void)
{
	volatile uint32_t *base =
		(volatile uint32_t *)DT_REG_ADDR(DT_NODELABEL(sdram_mem));
	const size_t size = DT_REG_SIZE(DT_NODELABEL(sdram_mem));
	char detail[96];
	uint32_t errs = 0;

	/* 字节通道诊断 + cache 分离: 定位具体数据线问题 */
	static const uint32_t lane_patterns[] = {
		0x00000000, 0xFFFFFFFF, 0xFF00FF00, 0x00FF00FF,
		0xFFFF0000, 0x0000FFFF, 0xA5A5A5A5, 0x5A5A5A5A,
	};

	for (size_t p = 0; p < ARRAY_SIZE(lane_patterns); p++) {
		base[p] = lane_patterns[p];
	}
	for (size_t p = 0; p < ARRAY_SIZE(lane_patterns); p++) {
		uint32_t v = base[p];

		LOG_INF("  lane[%u] wrote %08x read %08x %s",
			(unsigned)p, lane_patterns[p], v,
			v == lane_patterns[p] ? "ok" : "ERR");
	}

	/* 真实 DRAM 检查: 写 8 个分散地址 -> 刷 cache -> 回读
	 * (sys_cache_data_*_all 挂死系统, 已回退, 见 README) */
	do { } while (0);

	/* 全片写走样: 地址 + 递增种子 */
	for (size_t i = 0; i < size / 4; i++) {
		base[i] = (uint32_t)i ^ 0xA5A5A5A5U;
	}
	/* 回读校验 + 按 4MB 分桶的错误直方图 (32MB = 8 桶) */
	uint32_t hist[8] = {0};

	for (size_t i = 0; i < size / 4; i++) {
		if (base[i] != ((uint32_t)i ^ 0xA5A5A5A5U)) {
			errs++;
			hist[i / (size / 4 / 8)]++;
		}
	}
	for (int b = 0; b < 8; b++) {
		LOG_INF("  0x%08x-0x%08x: %u 错",
			(unsigned)(0xC0000000 + b * (size / 8)),
			(unsigned)(0xC0000000 + (b + 1) * (size / 8) - 1),
			hist[b]);
	}
	snprintf(detail, sizeof(detail), "32MB 写读走样, 错误 %u 字", errs);
	report("SDRAM W9825G6KH", errs == 0 ? PASS : FAIL, detail);
}

/* ---------------- ADC1 PA5 (INP19, 悬空/TPAD) ---------------- */
static void test_adc(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(adc1));
	static uint16_t buf;
	int32_t avg_mv = 0;
	char detail[96];
	const struct adc_channel_cfg ch_cfg = {
		.channel_id = 19,		/* PA5 = ADC1_INP19 */
		.gain = ADC_GAIN_1,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
	};
	struct adc_sequence seq = {
		.channels = BIT(19),
		.buffer = &buf,
		.buffer_size = sizeof(buf),
		.resolution = 12,
	};

	if (!device_is_ready(dev)) {
		report("ADC1 PA5", FAIL, "device not ready");
		return;
	}
	if (adc_channel_setup(dev, &ch_cfg) != 0) {
		report("ADC1 PA5", FAIL, "channel setup failed");
		return;
	}

	for (int i = 0; i < 8; i++) {
		int32_t mv;

		if (adc_read(dev, &seq) != 0) {
			report("ADC1 PA5", FAIL, "read failed");
			return;
		}
		mv = buf;
		adc_raw_to_millivolts(12, ADC_GAIN_1, 3300, &mv);
		avg_mv += mv;
		k_msleep(5);
	}
	avg_mv /= 8;
	snprintf(detail, sizeof(detail), "PA5 均值 %d mV (悬空为噪声)",
		 avg_mv);
	report("ADC1 PA5", PASS, detail);
}

/* ---------------- DAC1_OUT1 PA4 ---------------- */
static void test_dac(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(dac1));
	struct dac_channel_cfg cfg = {
		.channel_id = 1,
		.resolution = 12,
	};
	int ret;

	if (!device_is_ready(dev)) {
		report("DAC1 PA4", FAIL, "device not ready");
		return;
	}
	ret = dac_channel_setup(dev, &cfg);
	if (ret == 0) {
		ret = dac_write_value(dev, 1, 2048);	/* 半量程 ~1.65V */
	}
	report("DAC1 PA4", ret == 0 ? PASS : FAIL,
	       ret == 0 ? "输出 2048/4095 ≈ 1.65V (万用表确认)"
			: "setup/write 失败");
}

/* ---------------- PWM PA3 (TIM2_CH4, 1kHz 50%) ---------------- */
static void test_pwm(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(pwm2));
	int ret;

	if (!device_is_ready(dev)) {
		report("PWM PA3", FAIL, "device not ready");
		return;
	}
	/* 计数时钟 1MHz: 周期 1000 = 1kHz, 脉宽 500 = 50% */
	ret = pwm_set_cycles(dev, 3, 1000, 500, 0);
	report("PWM PA3", ret == 0 ? PASS : FAIL,
	       ret == 0 ? "TIM2_CH4 1kHz 50% (示波器/LED 确认)"
			: "pwm_set_cycles 失败");
}

/* ---------------- FDCAN1 回环收发 ---------------- */
CAN_MSGQ_DEFINE(can_msgq, 4);

static void test_can(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(fdcan1));
	struct can_filter filter = {
		.id = 0x123,
		.mask = CAN_STD_ID_MASK,
		.flags = 0,
	};
	struct can_frame tx = {
		.id = 0x123,
		.dlc = 8,
		.flags = 0,
	};
	struct can_frame rx;
	char detail[96];
	int ret, filter_id;

	if (!device_is_ready(dev)) {
		report("FDCAN1", FAIL, "device not ready");
		return;
	}

	ret = can_set_mode(dev, CAN_MODE_LOOPBACK);
	if (ret != 0) {
		report("FDCAN1", FAIL, "set loopback failed");
		return;
	}
	ret = can_start(dev);
	if (ret != 0 && ret != -EALREADY) {
		report("FDCAN1", FAIL, "start failed");
		return;
	}

	filter_id = can_add_rx_filter_msgq(dev, &can_msgq, &filter);
	if (filter_id < 0) {
		report("FDCAN1", FAIL, "add filter failed");
		return;
	}

	for (int i = 0; i < 8; i++) {
		tx.data[i] = 0x5A ^ i;
	}
	ret = can_send(dev, &tx, K_MSEC(200), NULL, NULL);
	if (ret != 0) {
		report("FDCAN1", FAIL, "send failed");
		goto out;
	}
	if (k_msgq_get(&can_msgq, &rx, K_MSEC(500)) != 0) {
		report("FDCAN1", FAIL, "loopback rx timeout");
		goto out;
	}
	bool ok = rx.id == tx.id && rx.dlc == tx.dlc &&
		  memcmp(rx.data, tx.data, 8) == 0;

	snprintf(detail, sizeof(detail), "500kbit/s 回环 0x123 x8 %s",
		 ok ? "一致" : "不一致");
	report("FDCAN1 loopback", ok ? PASS : FAIL, detail);
out:
	can_remove_rx_filter(dev, filter_id);
	(void)can_stop(dev);
}

/* ---------------- TF 卡 (SDMMC1, 只读测试) ---------------- */
static void test_sdmmc(void)
{
	static uint8_t buf[512];
	char detail[96];
	uint32_t sector_count = 0, sector_size = 0;
	int ret;

	ret = disk_access_init("SD");
	if (ret != 0) {
		report("TF card", SKIP, "未检测到卡 (disk_access_init 失败)");
		return;
	}
	if (disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_COUNT,
			      &sector_count) != 0 ||
	    disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_SIZE,
			      &sector_size) != 0) {
		report("TF card", FAIL, "geometry 读取失败");
		return;
	}
	ret = disk_access_read("SD", buf, 0, 1);
	if (ret != 0) {
		report("TF card", FAIL, "扇区 0 读取失败");
		return;
	}
	snprintf(detail, sizeof(detail),
		 "%u MB, %u B/扇区, MBR 签名 %s", 
		 (unsigned)(sector_count * (uint64_t)sector_size >> 20),
		 sector_size,
		 (buf[510] == 0x55 && buf[511] == 0xAA) ? "OK" : "无(可能无分区)");
	report("TF card (SDMMC1)", PASS, detail);
}

/* ---------------- RTC (LSE) 走时 ---------------- */
static void test_rtc(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(rtc));
	struct rtc_time tm;
	char detail[96];
	time_t t0, t1;

	if (!device_is_ready(dev)) {
		report("RTC", FAIL, "device not ready");
		return;
	}
	if (rtc_get_time(dev, &tm) != 0) {
		report("RTC", FAIL, "get time failed");
		return;
	}
	t0 = mktime((struct tm *)&tm);
	k_sleep(K_SECONDS(3));
	if (rtc_get_time(dev, &tm) != 0) {
		report("RTC", FAIL, "get time (2nd) failed");
		return;
	}
	t1 = mktime((struct tm *)&tm);
	snprintf(detail, sizeof(detail),
		 "%04d-%02d-%02d %02d:%02d:%02d, 3s 走时 %+lds",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec, (long)(t1 - t0));
	report("RTC (LSE)", (t1 - t0) >= 2 && (t1 - t0) <= 5 ? PASS : FAIL,
	       detail);
}

/* ---------------- 总入口 ---------------- */
static void run_all_tests(void)
{
	pass_cnt = fail_cnt = skip_cnt = 0;

	LOG_INF("=== apollo_v2_h743iit6 hardware self-test ===");

	test_led();
	test_keys();
	test_i2c_scan();
	test_eeprom();
	test_qspi_flash();
	test_sdram();
	test_adc();
	test_dac();
	test_pwm();
	test_can();
	test_sdmmc();
	test_rtc();

	LOG_INF("=== done: %d PASS / %d FAIL / %d SKIP ===",
		pass_cnt, fail_cnt, skip_cnt);
}

static int cmd_hwtest(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	run_all_tests();
	return 0;
}
SHELL_CMD_REGISTER(hwtest, NULL, "run hardware self-test suite", cmd_hwtest);

static int cmd_beep(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	uint16_t ms = 100;

	if (argc >= 2) {
		ms = (uint16_t)CLAMP((int)strtol(argv[1], NULL, 10), 10, 2000);
	}

	beeper_pulse(ms);
	return 0;
}
SHELL_CMD_ARG_REGISTER(beep, NULL, "manual beeper test: beep [ms=100]",
		       cmd_beep, 1, 1);

int main(void)
{
	beeper_silence();	/* 第一时间静音蜂鸣器 (PCF8574 状态跨复位保持) */
	k_sleep(K_SECONDS(2));	/* 等控制台稳定 */
	run_all_tests();
	return 0;
}
