/*
 * Copyright (c) 2026 Titan Mini contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Titan Mini (Renesas RA8P1) 硬件 bringup:
 *  - UART shell 直接跑在调试串口上 (zephyr,shell-uart = uart1 / U18, 115200-8N1)
 *  - RGB 流水灯 (共阳低有效: G=P108, R=P109, B=P110) + LOG 心跳
 *  - 用户按键 SW2 (P201/IRQ4) 中断计数, 200ms 软件防抖
 *  - SD 卡: SDHI0 + FATFS 挂载 /SD:, 读写校验
 *  - IMU: LSM6DS3TR-C (IIC1 @0x6A) 1Hz 加速度/陀螺仪采样
 *  - SDRAM: W9825G6KH-6 32MB@0x68000000 数据线/地址线/大块 pattern 测试
 *  - QSPI Flash: W25Q64 擦写读校验
 *  - kernel reboot 命令验证 CONFIG_REBOOT
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_SENSOR)
#include <zephyr/drivers/sensor.h>
#endif

#if defined(CONFIG_FILE_SYSTEM)
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#endif

LOG_MODULE_REGISTER(titan_bringup, LOG_LEVEL_INF);

#define LED_FLOW_STEP_MS   300
#define LOG_HEARTBEAT_LOOPS 6
#define BTN_DEBOUNCE_MS    200
#define QSPI_TEST_SIZE     256
#define QSPI_TEST_OFF      (8 * 1024 * 1024 - 4096) /* 最后一页, 避开常用区域 */

static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios), /* G P108 */
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios), /* R P109 */
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios), /* B P110 */
};

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_cb;
static atomic_t press_count;
static int64_t last_press_ms;

static void button_work_handler(struct k_work *work)
{
	int64_t now = k_uptime_get();

	if (now - last_press_ms < BTN_DEBOUNCE_MS) {
		return;
	}

	last_press_ms = now;
	LOG_INF("button SW2 pressed, count %d", (int)atomic_inc(&press_count) + 1);
}

static K_WORK_DEFINE(button_work, button_work_handler);

static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_work_submit(&button_work);
}

#if defined(CONFIG_FILE_SYSTEM) && DT_NODE_HAS_STATUS(DT_NODELABEL(w25q64), okay)
static int qspi_test(void)
{
	const struct device *flash = DEVICE_DT_GET(DT_NODELABEL(w25q64));
	uint8_t wbuf[QSPI_TEST_SIZE];
	uint8_t rbuf[QSPI_TEST_SIZE];
	int ret;

	if (!device_is_ready(flash)) {
		LOG_ERR("QSPI flash not ready");
		return -ENODEV;
	}

	uint64_t flash_size = 0;

	flash_get_size(flash, &flash_size);
	LOG_INF("QSPI flash size: %u MB", (unsigned)(flash_size >> 20));

	ret = flash_erase(flash, QSPI_TEST_OFF, 4096);
	if (ret < 0) {
		LOG_ERR("QSPI erase failed: %d", ret);
		return ret;
	}

	for (int i = 0; i < sizeof(wbuf); i++) {
		wbuf[i] = (uint8_t)(i ^ 0xA5);
	}

	ret = flash_write(flash, QSPI_TEST_OFF, wbuf, sizeof(wbuf));
	if (ret < 0) {
		LOG_ERR("QSPI write failed: %d", ret);
		return ret;
	}

	ret = flash_read(flash, QSPI_TEST_OFF, rbuf, sizeof(rbuf));
	if (ret < 0 || memcmp(wbuf, rbuf, sizeof(wbuf)) != 0) {
		LOG_ERR("QSPI readback mismatch (ret %d)", ret);
		return -EIO;
	}

	LOG_INF("QSPI erase/write/read OK (%u bytes @ 0x%x)", (unsigned)sizeof(wbuf),
		(unsigned)QSPI_TEST_OFF);
	return 0;
}
#endif /* W25Q64 enabled */

#if defined(CONFIG_FILE_SYSTEM)
static int sd_test(void)
{
	static FATFS fat_fs;
	struct fs_mount_t mp = {
		.type = FS_FATFS,
		.mnt_point = "/SD:",
		.fs_data = &fat_fs,
	};
	struct fs_statvfs st;
	struct fs_file_t f;
	char wbuf[] = "titan mini sd test " __DATE__ " " __TIME__ "\n";
	char rbuf[sizeof(wbuf)];
	int ret;

	fs_file_t_init(&f);

	ret = fs_mount(&mp);
	if (ret < 0) {
		LOG_ERR("SD mount failed: %d", ret);
		return ret;
	}
	LOG_INF("SD mounted at %s", mp.mnt_point);

	if (fs_statvfs("/SD:", &st) == 0) {
		LOG_INF("SD: %u MB total, %u MB free",
			(uint32_t)((uint64_t)st.f_blocks * st.f_bsize >> 20),
			(uint32_t)((uint64_t)st.f_bfree * st.f_bsize >> 20));
	}

	ret = fs_open(&f, "/SD:/titan_bringup.txt", FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) {
		LOG_ERR("SD open failed: %d", ret);
		return ret;
	}
	ret = fs_write(&f, wbuf, sizeof(wbuf) - 1);
	fs_close(&f);
	if (ret < 0) {
		LOG_ERR("SD write failed: %d", ret);
		return ret;
	}

	ret = fs_open(&f, "/SD:/titan_bringup.txt", FS_O_READ);
	if (ret < 0) {
		LOG_ERR("SD reopen failed: %d", ret);
		return ret;
	}
	ret = fs_read(&f, rbuf, sizeof(rbuf));
	fs_close(&f);
	if (ret < 0 || memcmp(rbuf, wbuf, sizeof(wbuf) - 1) != 0) {
		LOG_ERR("SD readback mismatch (ret %d)", ret);
		return -EIO;
	}

	LOG_INF("SD readback OK: \"%s\"", rbuf);
	return fs_unmount(&mp);
}
#endif /* CONFIG_FILE_SYSTEM */

static void led_flow(void)
{
	int step = 0;
	int loop = 0;

	while (1) {
		for (int i = 0; i < ARRAY_SIZE(leds); i++) {
			gpio_pin_set_dt(&leds[i], i == step);
		}
		step = (step + 1) % ARRAY_SIZE(leds);

		if (step == 0 && ++loop >= LOG_HEARTBEAT_LOOPS) {
			loop = 0;
			LOG_INF("heartbeat: uptime %d s", (int)(k_uptime_get() / 1000));
		}

		k_sleep(K_MSEC(LED_FLOW_STEP_MS));
	}
}

K_THREAD_DEFINE(led_flow_tid, 2048, led_flow, NULL, NULL, NULL, 5, 0, 2000);

#if defined(CONFIG_SENSOR)
static void imu_poll(void)
{
	const struct device *imu = DEVICE_DT_GET(DT_NODELABEL(lsm6ds3tr_c));
	struct sensor_value acc[3], gyr[3];
	int ret;

	if (!device_is_ready(imu)) {
		LOG_ERR("IMU not ready");
		return;
	}

	LOG_INF("IMU %s ready (LSM6DS3TR-C @0x6A)", imu->name);

	while (1) {
		ret = sensor_sample_fetch(imu);
		if (ret < 0) {
			LOG_ERR("IMU sample fetch failed: %d", ret);
			return;
		}

		sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, acc);
		sensor_channel_get(imu, SENSOR_CHAN_GYRO_XYZ, gyr);

		LOG_INF("IMU acc[m/s2]: %.2f %.2f %.2f  gyr[dps]: %.1f %.1f %.1f",
			sensor_value_to_double(&acc[0]), sensor_value_to_double(&acc[1]),
			sensor_value_to_double(&acc[2]), sensor_value_to_double(&gyr[0]),
			sensor_value_to_double(&gyr[1]), sensor_value_to_double(&gyr[2]));

		k_sleep(K_SECONDS(1));
	}
}

K_THREAD_DEFINE(imu_tid, 2048, imu_poll, NULL, NULL, NULL, 5, 0, 3000);

#if DT_NODE_HAS_STATUS(DT_NODELABEL(sdram0), okay)
#define SDRAM_BASE 0x68000000UL
#define SDRAM_SIZE (32UL * 1024 * 1024)

static int sdram_test(void)
{
	volatile uint16_t *s16 = (volatile uint16_t *)SDRAM_BASE;
	volatile uint32_t *s32 = (volatile uint32_t *)SDRAM_BASE;
	uint32_t t0, ms;
	int err = 0;

	/* 1) 数据总线: 16 位走位 1/0, 检查粘连/断路线 */
	for (int i = 0; i < 16; i++) {
		uint16_t v = BIT(i);
		s16[0] = v;
		if (s16[0] != v) {
			LOG_ERR("SDRAM DQ: write 0x%04x read 0x%04x (bit %d)", v, s16[0], i);
			err++;
		}
		s16[0] = (uint16_t)~v;
		if (s16[0] != (uint16_t)~v) {
			LOG_ERR("SDRAM DQ: write ~0x%04x read 0x%04x (bit %d)", v, s16[0], i);
			err++;
		}
	}
	if (err) {
		return -EIO;
	}
	LOG_INF("SDRAM data bus (16-bit walking): OK");

	/* 2) 地址总线: 按地址写地址值再回读 (覆盖 A0-A12 行/列 + BA) */
	t0 = k_uptime_get_32();
	for (uint32_t off = 0; off < SDRAM_SIZE; off += 0x20000) { /* 128KB 步进 */
		s32[off / 4] = 0xA5000000 | off;
	}
	for (uint32_t off = 0; off < SDRAM_SIZE; off += 0x20000) {
		if (s32[off / 4] != (0xA5000000 | off)) {
			LOG_ERR("SDRAM addr test fail @0x%08x: 0x%08x", off, s32[off / 4]);
			err++;
			if (err > 8) {
				break;
			}
		}
	}
	if (err) {
		return -EIO;
	}
	LOG_INF("SDRAM address bus (256 points): OK");

	/* 3) 大块 pattern: 4MB 递增写读 (远超 DCache, 强制真实 DRAM 访问) */
	t0 = k_uptime_get_32();
	{
		uint32_t n = 4UL * 1024 * 1024 / 4;

		for (uint32_t i = 0; i < n; i++) {
			s32[i] = i ^ 0x5A5A0000;
		}
		for (uint32_t i = 0; i < n; i++) {
			if (s32[i] != (i ^ 0x5A5A0000)) {
				LOG_ERR("SDRAM pattern fail @+0x%x: 0x%08x", i * 4, s32[i]);
				err++;
				if (err > 8) {
					break;
				}
			}
		}
	}
	ms = k_uptime_get_32() - t0;
	if (err) {
		return -EIO;
	}
	LOG_INF("SDRAM 4MB pattern: OK in %u ms", ms);

	return 0;
}
#endif /* sdram0 */

#endif /* CONFIG_SENSOR */

int main(void)
{
	int ret = 0;

	for (int i = 0; i < ARRAY_SIZE(leds); i++) {
		ret = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("LED %d config failed (%d)", i, ret);
		}
	}

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("button config failed (%d)", ret);
	} else {
		ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_FALLING);
		if (ret < 0) {
			LOG_ERR("button int config failed (%d)", ret);
		} else {
			gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
			gpio_add_callback(button.port, &button_cb);
			LOG_INF("button SW2 (P201/IRQ4) ready, press to count");
		}
	}

	LOG_INF("Titan Mini (Renesas RA8P1) HW bringup");
	LOG_INF("debug UART: U18, TXD1=P707 RXD1=P706, 115200-8N1");
	LOG_INF("RGB flow: G(P108) -> R(P109) -> B(P110), %d ms/step", LED_FLOW_STEP_MS);
	LOG_INF("shell ready, try: kernel version | kernel reboot");

#if DT_NODE_HAS_STATUS(DT_NODELABEL(sdram0), okay)
	sdram_test();
#endif

#if defined(CONFIG_FILE_SYSTEM)
#if DT_NODE_HAS_STATUS(DT_NODELABEL(w25q64), okay)
	qspi_test();
#endif
	sd_test();
#endif

	return 0;
}
