/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 系统初始化: settings 加载 (移植自 io-edge-hub src/settings/init.c,
 * 去除 CAN 固件升级波特率注入; CiA 302 固件下载见 fw_download.c)
 *
 *   - settings_subsys_init + settings_load (priority 8): 从 FCB 恢复
 *     holding_reg[] 持久化参数 (IP/Modbus/采样/历史 等), 供后续 RTU/TCP/
 *     网络/CANopen OD 桥接读取。CANopenNode 自身 OD 参数 (0x1017 等) 由
 *     canopen_storage 在 main 内加载。
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <time.h>
#include <init.h>
#ifdef CONFIG_UDP_FW_UPGRADE
#include <udp_fw_upgrade.h>
#endif

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

	/* 用当前 RTC 时间覆盖 timestamp 寄存器, 保证 Modbus 主站读到正确值 */
	time_t now = time(NULL);
	update_holding_reg(HOLDING_TIMESTAMP_HI_IDX, (uint16_t)((uint32_t)now >> 16));
	update_holding_reg(HOLDING_TIMESTAMP_LO_IDX, (uint16_t)(uint32_t)now);

	/* settings 恢复后同步历史开关, 否则重启后 history_enabled 仍为 false,
	 * 已使能的历史记录实际不会写入 */
	history_enable_write(get_holding_reg(HOLDING_HISTORY_ENABLE_IDX) != 0);

	/* 固件升级重启前刷出文件系统缓存 (history 等异步写入) */
#ifdef CONFIG_UDP_FW_UPGRADE
	udp_fw_add_pre_reboot_hook(history_sync);
#endif

	LOG_INF("settings loaded (slave_id=%u ip=%u.%u.%u.%u)",
		get_holding_reg(HOLDING_SLAVE_ID_IDX), get_holding_reg(HOLDING_IP_OCTET1_IDX),
		get_holding_reg(HOLDING_IP_OCTET2_IDX), get_holding_reg(HOLDING_IP_OCTET3_IDX),
		get_holding_reg(HOLDING_IP_OCTET4_IDX));
	return 0;
}

/* priority 8: 在 dio/adc (12) 和 rtu (13) 之前加载持久化参数 */
SYS_INIT(modbus_settings_init, APPLICATION, CONFIG_CANOPEN_IO_INIT_PRIORITY_SETTINGS);
