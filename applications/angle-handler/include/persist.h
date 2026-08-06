/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings 持久化存储接口 - 使用 Zephyr settings 子系统 (FCB 后端)
 * 存储到外部 SPI Flash (GD25Q80) 的 cfg_partition
 */

#ifndef __PERSIST_H__
#define __PERSIST_H__

/** 保存当前 connect_type 到 settings */
void persist_save_connect_type(void);

/** 保存当前 rf24_addr 到 settings (信道固定为 1, 不持久化) */
void persist_save_rf24_config(void);

#endif
