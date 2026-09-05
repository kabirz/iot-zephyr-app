/* SPDX-License-Identifier: Apache-2.0 */
#ifndef APOLLO_DISPLAY_H
#define APOLLO_DISPLAY_H

#include <stdbool.h>

/* 启动 LCD 双页 UI (LVGL + ILI9341/FMC), 生成独立刷新线程 */
int apollo_display_start(void);

/* 背光开关 (PB5, 高 = 亮) */
int apollo_display_backlight_set(bool on);

#endif /* APOLLO_DISPLAY_H */
