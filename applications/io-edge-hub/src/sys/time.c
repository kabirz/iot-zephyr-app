/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * RTC 时间管理 (STM32 内部 RTC, LSI 驱动)
 *   - 启动时从 RTC 读取时间设置系统时钟 (CLOCK_REALTIME)
 *   - 日志时间戳使用 RTC 时间
 *   - set_timestamp(): Modbus 0x0E/0x0F 或 UDP 命令设置 RTC + 系统时钟
 */

#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <init.h>

LOG_MODULE_REGISTER(io_time, LOG_LEVEL_INF);

static const struct device *rtc_dev;

#ifdef CONFIG_LOG
/* 日志时间戳: 返回 RTC 当前 Unix 秒 */
static uint32_t rtc_timestamp_get(void)
{
	struct rtc_time tm;

	if (rtc_dev && rtc_get_time(rtc_dev, &tm) == 0) {
		return (uint32_t)mktime((struct tm *)&tm);
	}
	return 0;
}
#endif

void set_timestamp(time_t t)
{
	if (!rtc_dev) {
		return;
	}

	struct tm *lt = gmtime(&t);
	struct rtc_time tm;

	memset(&tm, 0, sizeof(tm));
	tm.tm_sec = lt->tm_sec;
	tm.tm_min = lt->tm_min;
	tm.tm_hour = lt->tm_hour;
	tm.tm_mday = lt->tm_mday;
	tm.tm_mon = lt->tm_mon;
	tm.tm_year = lt->tm_year;
	tm.tm_wday = lt->tm_wday;
	tm.tm_yday = lt->tm_yday;
	tm.tm_isdst = lt->tm_isdst;

	rtc_set_time(rtc_dev, &tm);

	struct timespec ts = { .tv_sec = t, .tv_nsec = 0 };

	clock_settime(CLOCK_REALTIME, &ts);
	LOG_INF("time set: %lld", (long long)t);
}

static int clock_init(void)
{
	rtc_dev = DEVICE_DT_GET(DT_NODELABEL(rtc));
	if (!device_is_ready(rtc_dev)) {
		LOG_ERR("RTC device not ready");
		return -ENODEV;
	}

	/* 启动时从 RTC 恢复系统时钟 */
	struct rtc_time tm;

	if (rtc_get_time(rtc_dev, &tm) == 0) {
		time_t t = mktime((struct tm *)&tm);
		struct timespec ts = { .tv_sec = t, .tv_nsec = 0 };

		clock_settime(CLOCK_REALTIME, &ts);
		LOG_INF("RTC time restored: %lld", (long long)t);
	}

#ifdef CONFIG_LOG
	log_set_timestamp_func(rtc_timestamp_get, 1U);
#endif
	return 0;
}

SYS_INIT(clock_init, POST_KERNEL, 41);
