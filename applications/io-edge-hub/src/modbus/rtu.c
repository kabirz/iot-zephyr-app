/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus RTU Slave (RS485, USART2 + MAX485 DE=PA1)
 *
 *   - Zephyr modbus serial iface (设备树 modbus0 节点)
 *   - baud 从 holding_reg[RS485_BPS], unit_id 从 holding_reg[SLAVE_ID]
 *   - user_cb 共用 io_modbus_cbs (与 TCP 共享寄存器回调)
 */

#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <init.h>

LOG_MODULE_REGISTER(io_rtu, LOG_LEVEL_INF);

#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)

extern const struct modbus_user_callbacks io_modbus_cbs;

static int rtu_init(void)
{
	const char iface_name[] = { DEVICE_DT_NAME(MODBUS_NODE) };
	int iface = modbus_iface_get_by_name(iface_name);

	if (iface < 0) {
		LOG_ERR("RTU iface not found");
		return -ENODEV;
	}

	struct modbus_iface_param param = {
		.mode = MODBUS_MODE_RTU,
		.server = {
			.user_cb = (struct modbus_user_callbacks *)&io_modbus_cbs,
			.unit_id = (uint8_t)get_holding_reg(HOLDING_SLAVE_ID_IDX),
		},
		.serial = {
			.baud = get_holding_reg(HOLDING_RS485_BPS_IDX),
			.parity = UART_CFG_PARITY_NONE,
		},
	};

	int rc = modbus_init_server(iface, param);

	if (rc) {
		LOG_ERR("RTU init failed: %d", rc);
	} else {
		LOG_INF("Modbus RTU slave (id=%u, %u bps)",
			param.server.unit_id, param.serial.baud);
	}
	return rc;
}

/* priority 13: 在 settings load (11) 之后, 拿到持久化的 baud/slave_id */
SYS_INIT(rtu_init, APPLICATION, 13);
