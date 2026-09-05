/* SPDX-License-Identifier: Apache-2.0
 * RTC 时间: 内部存 UTC, 应用层统一 UTC+8 转换 (V1 修复 #10:
 * RTC 直读小时数曾导致显示差 8 小时)。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "app_time.h"

LOG_MODULE_REGISTER(app_time, LOG_LEVEL_INF);

#define RTC_NODE DT_NODELABEL(rtc)
static const struct device *const rtc_dev = DEVICE_DT_GET(RTC_NODE);

#define LOCAL_UTC_OFFSET_SEC (8 * 3600)	/* UTC+8 */

static void unix_to_rtc_tm(int64_t unix_sec, struct rtc_time *rt)
{
	time_t t = (time_t)unix_sec;
	struct tm tm;

	gmtime_r(&t, &tm);
	memset(rt, 0, sizeof(*rt));
	rt->tm_sec = tm.tm_sec;
	rt->tm_min = tm.tm_min;
	rt->tm_hour = tm.tm_hour;
	rt->tm_mday = tm.tm_mday;
	rt->tm_mon = tm.tm_mon;
	rt->tm_year = tm.tm_year;
	rt->tm_wday = tm.tm_wday;
	rt->tm_yday = tm.tm_yday;
	rt->tm_isdst = 0;
	rt->tm_nsec = 0;
}

static int64_t rtc_tm_to_unix(const struct rtc_time *rt)
{
	struct tm tm;

	memset(&tm, 0, sizeof(tm));
	tm.tm_sec = rt->tm_sec;
	tm.tm_min = rt->tm_min;
	tm.tm_hour = rt->tm_hour;
	tm.tm_mday = rt->tm_mday;
	tm.tm_mon = rt->tm_mon;
	tm.tm_year = rt->tm_year;
	tm.tm_wday = rt->tm_wday;
	tm.tm_yday = rt->tm_yday;
	tm.tm_isdst = 0;
	return (int64_t)timeutil_timegm(&tm);
}

bool app_time_get_utc(int64_t *unix_sec)
{
	struct rtc_time rt;

	if (!device_is_ready(rtc_dev)) {
		return false;
	}
	if (rtc_get_time(rtc_dev, &rt) != 0) {
		return false;
	}
	if (unix_sec != NULL) {
		*unix_sec = rtc_tm_to_unix(&rt);
	}
	/* 掉电后 RTC 从 2000-01-01 起数, 视为未设置 */
	return (rt.tm_year + 1900) >= 2025;
}

int app_time_set_utc(int64_t unix_sec)
{
	struct rtc_time rt;

	if (!device_is_ready(rtc_dev)) {
		return -ENODEV;
	}
	unix_to_rtc_tm(unix_sec, &rt);
	int ret = rtc_set_time(rtc_dev, &rt);

	if (ret == 0) {
		LOG_INF("RTC set: utc=%lld", (long long)unix_sec);
	}
	return ret;
}

bool app_time_get_local(struct tm *tm)
{
	int64_t utc;

	if (!app_time_get_utc(&utc)) {
		return false;
	}
	time_t t = (time_t)(utc + LOCAL_UTC_OFFSET_SEC);

	gmtime_r(&t, tm);
	return true;
}

int app_time_set_local_str(const char *date, const char *time)
{
	struct tm tm;
	int64_t utc;

	memset(&tm, 0, sizeof(tm));
	if (sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3 ||
	    sscanf(time, "%d:%d:%d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 3) {
		return -EINVAL;
	}
	if (tm.tm_year < 100 || tm.tm_year > 3000 ||
	    tm.tm_mon < 1 || tm.tm_mon > 12 ||
	    tm.tm_mday < 1 || tm.tm_mday > 31) {
		return -EINVAL;
	}
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;

	utc = (int64_t)timeutil_timegm(&tm) - LOCAL_UTC_OFFSET_SEC;
	return app_time_set_utc(utc);
}

const char *app_time_local_str(void)
{
	static char buf[24];
	struct tm tm;

	if (!app_time_get_local(&tm)) {
		return "----.--.-- --:--:--";
	}
	snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec);
	return buf;
}
