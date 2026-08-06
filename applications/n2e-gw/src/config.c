/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 配置管理 - 读取/保存网关配置
 */

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <n2e_gw.h>

LOG_MODULE_REGISTER(gw_config, LOG_LEVEL_INF);

void gw_config_load(void)
{
	settings_load();
	LOG_INF("Config loaded");
}

void gw_config_save(void)
{
	persist_save_rf24_config();
	persist_save_network_config();
}
