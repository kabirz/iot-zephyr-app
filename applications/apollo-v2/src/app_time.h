/* SPDX-License-Identifier: Apache-2.0 */
#ifndef APOLLO_APP_TIME_H
#define APOLLO_APP_TIME_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* RTC 时间管理: RTC 存 UTC, 显示/设置走本地 (UTC+8) —— V1 修复 #10 */

/* 读取 RTC 为 UTC Unix 秒; RTC 无效 (年份 < 2025) 返回 false */
bool app_time_get_utc(int64_t *unix_sec);

/* UTC 秒写入 RTC (同时等 LSE 走时) */
int app_time_set_utc(int64_t unix_sec);

/* 本地时间 (UTC+8) 填入 *tm; 返回 false 表示 RTC 未设置 */
bool app_time_get_local(struct tm *tm);

/* 解析 "YYYY-MM-DD HH:MM:SS" 为本地时间并写入 RTC (按 UTC+8 折算) */
int app_time_set_local_str(const char *date, const char *time);

/* 当前本地时间字符串 "YYYY-MM-DD HH:MM:SS" (未设置时返回占位串) */
const char *app_time_local_str(void);

#endif /* APOLLO_APP_TIME_H */
