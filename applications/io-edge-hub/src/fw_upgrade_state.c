/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 固件升级互斥状态 - 三个通道 (UDP/CAN/WebSocket) 共享
 */

#include "fw_upgrade_state.h"
#include <zephyr/kernel.h>
#include <udp_fw_upgrade.h>
#include <can_fw_upgrade.h>

static enum fw_upgrade_channel current_channel = FW_UPGRADE_CHANNEL_UDP;
static bool upgrade_active = false;
static struct k_mutex lock;

static bool fw_pre_start_hook(void)
{
	return fw_upgrade_try_lock(FW_UPGRADE_CHANNEL_UDP);
}

static int fw_upgrade_state_init(void)
{
	k_mutex_init(&lock);
	udp_fw_add_pre_start_hook(fw_pre_start_hook);
	can_fw_add_pre_start_hook(fw_pre_start_hook);
	return 0;
}

SYS_INIT(fw_upgrade_state_init, APPLICATION, CONFIG_IO_INIT_PRIORITY_FW_UPGRADE);

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
