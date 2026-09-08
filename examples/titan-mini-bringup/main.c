/*
 * Copyright (c) 2026 Titan Mini contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Titan Mini (Renesas RA8P1) 硬件 bringup:
 *  - UART shell 直接跑在调试串口上 (zephyr,shell-uart = uart1 / U18, 115200-8N1)
 *  - RGB 流水灯 (共阳低有效: G=P108, R=P109, B=P110) + LOG 心跳
 *  - 用户按键 SW2 (P201/IRQ4) 中断计数, 200ms 软件防抖
 *  - SD 卡: SDHI0 + FATFS 挂载 /SD:, 读写校验
 *  - QSPI Flash: W25Q64 擦写读校验
 *  - kernel reboot 命令验证 CONFIG_REBOOT
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

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

#if defined(CONFIG_FILE_SYSTEM)
#if DT_NODE_HAS_STATUS(DT_NODELABEL(w25q64), okay)
	qspi_test();
#endif
	sd_test();
#endif

	return 0;
}
