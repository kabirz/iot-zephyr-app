/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 应用命令码 (配置端口 8600, app handler 处理 0x10+; 0x01-0x05 由库处理)
 */

#ifndef __UDP_H__
#define __UDP_H__

enum udp_app_cmd {
	UDP_CMD_SET_IP		= 0x10,
	UDP_CMD_GET_IP		= 0x11,
	UDP_CMD_SET_MODBUS	= 0x12,
	UDP_CMD_GET_MODBUS	= 0x13,
	UDP_CMD_SET_TIME	= 0x14,
	UDP_CMD_FACTORY_RESET	= 0x19,
};

#endif /* __UDP_H__ */
