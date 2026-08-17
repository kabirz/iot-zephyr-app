/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 应用命令处理 (配置端口 8600, 由 udp_fw_upgrade 库 RX 线程分发)
 *
 * 命令 (见 udp.h): SET/GET IP、SET/GET MODBUS (slave_id+rs485_baud)、SET_TIME,
 * FACTORY_RESET。固件升级命令 (0x01-0x05) 由库内部处理。回复经
 * udp_fw_reply() (库 RX 线程内同步 sendto, 缓冲 64B, 数据 <=63B)。
 * SET 类命令改 holding_reg[] 后 holding_reg_save() 持久化。
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <udp_fw_upgrade.h>
#include <init.h>
#include "udp.h"

LOG_MODULE_REGISTER(io_udp, LOG_LEVEL_INF);

static bool app_cmd_handler(uint8_t cmd, const uint8_t *data, size_t len,
			    void *user_data)
{
	ARG_UNUSED(user_data);

	switch (cmd) {
	case UDP_CMD_SET_IP: {
		uint8_t ok = 0;

		if (len >= 4 && ip_addr_valid(data[0], data[1], data[2], data[3])) {
			update_holding_reg(HOLDING_IP_OCTET1_IDX, data[0]);
			update_holding_reg(HOLDING_IP_OCTET2_IDX, data[1]);
			update_holding_reg(HOLDING_IP_OCTET3_IDX, data[2]);
			update_holding_reg(HOLDING_IP_OCTET4_IDX, data[3]);
			holding_reg_save();
			LOG_INF("SET_IP %u.%u.%u.%u (manual reboot required to apply)",
				data[0], data[1], data[2], data[3]);
			ok = 1;
		}
		udp_fw_reply(cmd, &ok, sizeof(ok));
		return true;
	}

	case UDP_CMD_GET_IP: {
		uint8_t buf[4];

		buf[0] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET1_IDX);
		buf[1] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET2_IDX);
		buf[2] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET3_IDX);
		buf[3] = (uint8_t)get_holding_reg(HOLDING_IP_OCTET4_IDX);
		udp_fw_reply(cmd, buf, sizeof(buf));
		return true;
	}

	case UDP_CMD_SET_MODBUS: {
		/* slave_id(1B) + rs485_baud(2B): 与 holding_reg 16 位存储宽度一致
		 * (原 4B 协议会把 >65535 的波特率截断成错误值) */
		if (len >= 3) {
			update_holding_reg(HOLDING_SLAVE_ID_IDX, data[0]);
			update_holding_reg(HOLDING_RS485_BAUDRATE_IDX, sys_get_be16(&data[1]));
			holding_reg_save();
		}
		uint8_t ok = (len >= 3) ? 1 : 0;

		udp_fw_reply(cmd, &ok, sizeof(ok));
		return true;
	}

	case UDP_CMD_GET_MODBUS: {
		uint8_t buf[3];

		buf[0] = (uint8_t)get_holding_reg(HOLDING_SLAVE_ID_IDX);
		sys_put_be16(get_holding_reg(HOLDING_RS485_BAUDRATE_IDX), &buf[1]);
		udp_fw_reply(cmd, buf, sizeof(buf));
		return true;
	}

	case UDP_CMD_SET_TIME: {
		/* unix 时间戳 (4B 大端) → set_timestamp 设置 RTC + 系统时钟 */
		uint8_t ok = 0;

		if (len >= 4) {
			ok = set_timestamp((time_t)sys_get_be32(data)) ? 1 : 0;
		}
		udp_fw_reply(cmd, &ok, sizeof(ok));
		return true;
	}

	case UDP_CMD_FACTORY_RESET: {
		uint8_t ok = (settings_factory_reset() == 0) ? 1 : 0;

		udp_fw_reply(cmd, &ok, sizeof(ok));
		if (ok) {
			/* 排空 deferred 日志缓冲再重启 */
			history_sync();
#ifdef CONFIG_LOG
			while (log_process()) {
			}
#endif
			k_msleep(100);
			sys_reboot(SYS_REBOOT_COLD);
		}
		return true;
	}

	default:
		return false;
	}
}

static int udp_app_init(void)
{
	udp_fw_set_app_handler(app_cmd_handler, NULL);
	udp_fw_allow_broadcast_cmd(UDP_CMD_GET_IP);
	LOG_INF("UDP app handler registered (port %d)", CONFIG_UDP_FW_CONFIG_PORT);
	return 0;
}

/* 库 udp_fw_upgrade SYS_INIT priority 90; app handler 需在其前注册 */
SYS_INIT(udp_app_init, APPLICATION, 80);
