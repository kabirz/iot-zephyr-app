/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 固件升级互斥状态 - 多通道 (UDP/WebSocket/CANopen SDO) 共享
 * (移植自 io-edge-hub src/fw_upgrade_state.c, CAN 通道改为 SDO 注册)
 */

#include "fw_upgrade_state.h"
#include <zephyr/kernel.h>
#ifdef CONFIG_UDP_FW_UPGRADE
#include <udp_fw_upgrade.h>
#endif

static enum fw_upgrade_channel current_channel = FW_UPGRADE_CHANNEL_UDP;
static bool upgrade_active = false;
static struct k_mutex lock;

#ifdef CONFIG_UDP_FW_UPGRADE
/* UDP 固件升级库开始前申请升级锁 (拒绝其他通道并发升级) */
static bool fw_pre_start_hook(void)
{
	return fw_upgrade_try_lock(FW_UPGRADE_CHANNEL_UDP);
}
#endif

static int fw_upgrade_state_init(void)
{
	k_mutex_init(&lock);
#ifdef CONFIG_UDP_FW_UPGRADE
	udp_fw_add_pre_start_hook(fw_pre_start_hook);
#endif
	return 0;
}

SYS_INIT(fw_upgrade_state_init, APPLICATION, CONFIG_CANOPEN_IO_INIT_PRIORITY_FW_UPGRADE);

bool fw_upgrade_try_lock(enum fw_upgrade_channel channel)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (upgrade_active) {
		k_mutex_unlock(&lock);
		return false;
	}
	upgrade_active = true;
	current_channel = channel;
	k_mutex_unlock(&lock);
	return true;
}

void fw_upgrade_unlock(enum fw_upgrade_channel channel)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (current_channel == channel) {
		upgrade_active = false;
	}
	k_mutex_unlock(&lock);
}

bool fw_upgrade_is_active(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	bool active = upgrade_active;

	k_mutex_unlock(&lock);
	return active;
}
