/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 系统初始化: settings 加载
 *
 *   - settings_subsys_init + settings_load (priority 11): 从 FCB 恢复
 *     holding_reg[] 持久化参数 (IP/Modbus/采样/历史 等), 供后续 RTU/TCP/网络读取
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <init.h>

LOG_MODULE_REGISTER(io_init, LOG_LEVEL_INF);

/* ==================== Settings 加载 ==================== */
static int modbus_settings_init(void)
{
	int rc = settings_subsys_init();

	if (rc) {
		LOG_ERR("settings_subsys_init failed: %d", rc);
		return rc;
	}

	rc = settings_load();
	if (rc) {
		LOG_ERR("settings_load failed: %d", rc);
		return rc;
	}

	/* settings 恢复后同步历史开关, 否则重启后 history_enabled 仍为 false,
	 * 已使能的历史记录实际不会写入 (function.c 写回调/UDP 才会触发该函数) */
	history_enable_write(get_holding_reg(HOLDING_HIS_SAVE_IDX) != 0);

	LOG_INF("settings loaded (slave_id=%u ip=%u.%u.%u.%u)",
		get_holding_reg(HOLDING_SLAVE_ID_IDX),
		get_holding_reg(HOLDING_IP_ADDR_1_IDX),
		get_holding_reg(HOLDING_IP_ADDR_2_IDX),
		get_holding_reg(HOLDING_IP_ADDR_3_IDX),
		get_holding_reg(HOLDING_IP_ADDR_4_IDX));
	return 0;
}

/* priority 11: 在 dio/adc (12) 和 rtu (13) 之前加载持久化参数 */
SYS_INIT(modbus_settings_init, APPLICATION, 11);
