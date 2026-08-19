/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 固件升级互斥状态 - 三个通道 (UDP/CAN/WebSocket) 共享
 * 当其中一个通道正在升级时，其他通道的升级请求应被拒绝。
 */

#ifndef __FW_UPGRADE_STATE_H__
#define __FW_UPGRADE_STATE_H__

#include <stdbool.h>

/* 升级通道标识 */
enum fw_upgrade_channel {
	FW_UPGRADE_CHANNEL_UDP = 0,
	FW_UPGRADE_CHANNEL_CAN,
	FW_UPGRADE_CHANNEL_WS,
};

/**
 * @brief 尝试获取升级锁
 *
 * @param channel 请求升级的通道
 * @return true 获取成功，可以开始升级
 * @return false 已有其他通道正在升级
 */
bool fw_upgrade_try_lock(enum fw_upgrade_channel channel);

/**
 * @brief 释放升级锁
 *
 * @param channel 释放锁的通道
 */
void fw_upgrade_unlock(enum fw_upgrade_channel channel);

/**
 * @brief 检查是否有通道正在升级
 *
 * @return true 有通道正在升级
 * @return false 没有通道在升级
 */
bool fw_upgrade_is_active(void);

#endif /* __FW_UPGRADE_STATE_H__ */
