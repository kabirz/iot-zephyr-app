/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 应用命令处理 (配置端口 8600, 由 udp_fw_upgrade 库 RX 线程分发)
 *
 * 命令 (0x10+, 见 udp.h): SET/GET IP/MODBUS/SAMPLE/CAN/HIS, DISCOVER, FACTORY_RESET。
 * 固件升级命令 (0x01-0x05) 由库内部处理。回复经 udp_fw_reply() (库 RX 线程内同步
 * sendto, 缓冲 64B, 数据 <=63B)。SET 类命令改 holding_reg[] 后 holding_reg_save() 持久化。
 */

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/app_version.h>
#include <fw_gitver.h>
#include <zephyr/logging/log.h>
#include <udp_fw_upgrade.h>
#include <init.h>
#include "udp.h"

LOG_MODULE_REGISTER(io_udp, LOG_LEVEL_INF);

#define MODBUS_TCP_PORT 502

/* IP 合法性 (与 function.c 导出检查一致) */
static bool ip_valid(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	if (d == 0 || d == 0xFF) {
		return false;
	}
	if (a >= 224 && a <= 239) {
		return false;
	}
	return true;
}

static bool app_cmd_handler(uint8_t cmd, const uint8_t *data, size_t len,
			    void *user_data)
{
	ARG_UNUSED(user_data);

	switch (cmd) {
	case UDP_CMD_SET_IP: {
		uint8_t ok = 0;

		if (len >= 4 && ip_valid(data[0], data[1], data[2], data[3])) {
			update_holding_reg(HOLDING_IP_ADDR_1_IDX, data[0]);
			update_holding_reg(HOLDING_IP_ADDR_2_IDX, data[1]);
			update_holding_reg(HOLDING_IP_ADDR_3_IDX, data[2]);
			update_holding_reg(HOLDING_IP_ADDR_4_IDX, data[3]);
			holding_reg_save();
			LOG_INF("SET_IP %u.%u.%u.%u (reboot to apply)",
				data[0], data[1], data[2], data[3]);
			ok = 1;
		}
		udp_fw_reply(cmd, &ok, sizeof(ok));
		if (ok) {
			/* 回复发出后延迟重启, 让新 IP 生效 */
			set_reboot_status(true);
		}
		return true;
	}

	case UDP_CMD_GET_NET: {
		uint8_t buf[7];

		buf[0] = (uint8_t)get_holding_reg(HOLDING_IP_ADDR_1_IDX);
		buf[1] = (uint8_t)get_holding_reg(HOLDING_IP_ADDR_2_IDX);
		buf[2] = (uint8_t)get_holding_reg(HOLDING_IP_ADDR_3_IDX);
		buf[3] = (uint8_t)get_holding_reg(HOLDING_IP_ADDR_4_IDX);
		buf[4] = (uint8_t)get_holding_reg(HOLDING_SLAVE_ID_IDX);
		sys_put_be16(MODBUS_TCP_PORT, &buf[5]);
		udp_fw_reply(cmd, buf, sizeof(buf));
		return true;
	}

	case UDP_CMD_SET_MODBUS: {
		if (len >= 5) {
			update_holding_reg(HOLDING_SLAVE_ID_IDX, data[0]);
			update_holding_reg(HOLDING_RS485_BPS_IDX,
					   sys_get_be32(&data[1]));
			holding_reg_save();
		}
		uint8_t ok = (len >= 5) ? 1 : 0;

		udp_fw_reply(cmd, &ok, sizeof(ok));
		return true;
	}

	case UDP_CMD_GET_MODBUS: {
		uint8_t buf[5];

		buf[0] = (uint8_t)get_holding_reg(HOLDING_SLAVE_ID_IDX);
		sys_put_be32(get_holding_reg(HOLDING_RS485_BPS_IDX), &buf[1]);
		udp_fw_reply(cmd, buf, sizeof(buf));
		return true;
	}

	case UDP_CMD_SET_SAMPLE: {
		if (len >= 8) {
			update_holding_reg(HOLDING_DI_EN_IDX, sys_get_be16(&data[0]));
			update_holding_reg(HOLDING_AI_EN_IDX, sys_get_be16(&data[2]));
			update_holding_reg(HOLDING_DI_SI_IDX, sys_get_be16(&data[4]));
			update_holding_reg(HOLDING_AI_SI_IDX, sys_get_be16(&data[6]));
			holding_reg_save();
		}
		uint8_t ok = (len >= 8) ? 1 : 0;

		udp_fw_reply(cmd, &ok, sizeof(ok));
		return true;
	}

	case UDP_CMD_GET_SAMPLE: {
		uint8_t buf[8];

		sys_put_be16(get_holding_reg(HOLDING_DI_EN_IDX), &buf[0]);
		sys_put_be16(get_holding_reg(HOLDING_AI_EN_IDX), &buf[2]);
		sys_put_be16(get_holding_reg(HOLDING_DI_SI_IDX), &buf[4]);
		sys_put_be16(get_holding_reg(HOLDING_AI_SI_IDX), &buf[6]);
		udp_fw_reply(cmd, buf, sizeof(buf));
		return true;
	}

	case UDP_CMD_SET_CAN: {
		if (len >= 6) {
			update_holding_reg(HOLDING_CAN_ID_IDX, sys_get_be16(&data[0]));
			update_holding_reg(HOLDING_CAN_BPS_IDX, sys_get_be32(&data[2]));
			holding_reg_save();
		}
		uint8_t ok = (len >= 6) ? 1 : 0;

		udp_fw_reply(cmd, &ok, sizeof(ok));
		return true;
	}

	case UDP_CMD_GET_CAN: {
		uint8_t buf[6];

		sys_put_be16(get_holding_reg(HOLDING_CAN_ID_IDX), &buf[0]);
		sys_put_be32(get_holding_reg(HOLDING_CAN_BPS_IDX), &buf[2]);
		udp_fw_reply(cmd, buf, sizeof(buf));
		return true;
	}

	case UDP_CMD_DISCOVER: {
		char buf[63];
		int n = snprintf(buf, sizeof(buf),
				 "io-edge-hub %u.%u.%u.%u v%d.%d.%d_%s",
				 get_holding_reg(HOLDING_IP_ADDR_1_IDX),
				 get_holding_reg(HOLDING_IP_ADDR_2_IDX),
				 get_holding_reg(HOLDING_IP_ADDR_3_IDX),
				 get_holding_reg(HOLDING_IP_ADDR_4_IDX),
				 APP_VERSION_MAJOR, APP_VERSION_MINOR,
				 APP_PATCHLEVEL, FW_GIT_VERSION);

		udp_fw_reply(cmd, (uint8_t *)buf, (n > 0) ? (uint8_t)n : 0);
		return true;
	}

	case UDP_CMD_FACTORY_RESET: {
		uint8_t ok = (settings_factory_reset() == 0) ? 1 : 0;

		udp_fw_reply(cmd, &ok, sizeof(ok));
		if (ok) {
			k_msleep(100);
			sys_reboot(SYS_REBOOT_COLD);
		}
		return true;
	}

	case UDP_CMD_SET_HIS: {
		if (len >= 1) {
			update_holding_reg(HOLDING_HIS_SAVE_IDX, data[0]);
			history_enable_write(data[0] != 0);
			holding_reg_save();
		}
		uint8_t ok = (len >= 1) ? 1 : 0;

		udp_fw_reply(cmd, &ok, sizeof(ok));
		return true;
	}

	case UDP_CMD_GET_HIS: {
		uint8_t v = (uint8_t)get_holding_reg(HOLDING_HIS_SAVE_IDX);

		udp_fw_reply(cmd, &v, sizeof(v));
		return true;
	}

	default:
		return false;
	}
}

static int udp_app_init(void)
{
	udp_fw_set_app_handler(app_cmd_handler, NULL);
	udp_fw_allow_broadcast_cmd(UDP_CMD_DISCOVER);
	LOG_INF("UDP app handler registered (port %d)", CONFIG_UDP_FW_CONFIG_PORT);
	return 0;
}

/* 库 udp_fw_upgrade SYS_INIT priority 90; app handler 需在其前注册 */
SYS_INIT(udp_app_init, APPLICATION, 80);
