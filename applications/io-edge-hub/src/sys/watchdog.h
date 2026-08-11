/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 硬件看门狗 (IWDG) 接口
 */

#ifndef __WATCHDOG_H__
#define __WATCHDOG_H__

/* 初始化 IWDG (30s 超时), 失败返回负 errno */
int watchdog_init(void);

/* 喂狗 (main 主循环周期调用, fs_littlefs.c mkfs 事件型调用) */
void watchdog_feed(void);

#endif /* __WATCHDOG_H__ */
