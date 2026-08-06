/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 总线收发 + 心跳线程 + 手柄状态上报 (0x1E3 BE)
 * + 扫描仪数据解析 (0x263/0x363/0x463)
 * CAN 接收与固件升级由 can_fw_upgrade 库自包含管理;
 * 本模块仅注册业务帧 callback 并提供业务帧发送。
 */

#include <zephyr/kernel.h>
#include <mod-can.h>
#include <display.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <rf24.h>
#include <common.h>
#include <persist.h>
#include <can_fw_upgrade.h>
LOG_MODULE_REGISTER(mod_can, LOG_LEVEL_INF);

static const struct device *can_dev;	/* 由 mod_can_init() 从 can_fw_set_app_handler() 获取 */

/* ================================================================
 * CAN 发送
 * ================================================================ */
static void mod_cantx_callback(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (error) {
		LOG_ERR("CAN tx error: %d", error);
	}
}

int mod_can_send(struct can_frame *frame)
{
	if (!can_dev) {
		return -ENODEV;
	}

	return can_send(can_dev, frame, K_MSEC(100), mod_cantx_callback, NULL);
}

/* ================================================================
 * 扫描仪 CAN 数据解析 — 0x263/0x363/0x463, 大端序
 * ================================================================ */
void mod_can_parse_scanner(struct can_frame *frame)
{
	scanner_data_t *s = &global_params.scanner;

	switch (frame->id) {
	case OVERBREAK_LASER:
		/* Byte 0: flags (bit0=overbreak_valid, bit1=laser_valid)
		 * Byte 1: reserved
		 * Byte 2-3: overbreak_value (int16_t BE)
		 * Byte 4-7: laser_value (uint32_t BE)
		 */
		s->overbreak_valid = (frame->data[0] & 0x01) ? 1 : -1;
		s->laser_valid = (frame->data[0] & 0x02) ? 1 : -1;
		s->overbreak_value = (int16_t)sys_get_be16(&frame->data[2]);
		s->laser_distance = (int32_t)sys_get_be32(&frame->data[4]);
		if (global_params.log) {
			LOG_INF("Overbreak: valid=%d val=%d, Laser: valid=%d val=%d", s->overbreak_valid,
				s->overbreak_value, s->laser_valid, s->laser_distance);
		}
		break;

	case COORD_XY:
		/* Byte 0-3: coordX (int32_t BE)
		 * Byte 4-7: coordY (int32_t BE)
		 */
		s->coord_x = (int32_t)sys_get_be32(&frame->data[0]);
		s->coord_y = (int32_t)sys_get_be32(&frame->data[4]);
		s->coord_xy_valid = 1;
		if (global_params.log) {
			LOG_INF("CoordXY: X=%d Y=%d", s->coord_x, s->coord_y);
		}
		return;

	case COORD_Z:
		/* Byte 0-3: coordZ (int32_t BE)
		 * Byte 4: flags (bit0: coordz_valid)
		 * Byte 5-7: reserved
		 *
		 * Z 坐标有效标志同时设置 X/Y/Z 三个坐标的有效性
		 */
		s->coord_z = (int32_t)sys_get_be32(&frame->data[0]);
		s->coord_z_valid = (frame->data[4] & 0x01) ? 1 : -1;
		s->coord_xy_valid = s->coord_z_valid;
		if (global_params.log) {
			LOG_INF("CoordZ: Z=%d valid=%d", s->coord_z, s->coord_z_valid);
		}
		return;

	default:
		return;
	}

	/* 解析完成后刷新扫描仪数据显示 */
	mod_display_scanner(s);
}

/* ================================================================
 * nRF24 配置 CAN 命令 (0x110)
 *
 * CMD GET_CONFIG (0x02): Byte 0=cmd, Byte 1-7=reserved
 * RESP (0x111): [cmd 1B][addr 5B][reserved 2B]  (信道固定为 1, 不返回)
 * ================================================================ */

static void rf24_send_config_resp(uint8_t cmd)
{
	uint8_t buf[8] = {0};

	buf[0] = cmd;
	memcpy(&buf[1], global_params.rf24_addr, RF24_ADDR_LEN);

	struct can_frame resp = {
		.id = RF24_CONFIG_RESP,
		.dlc = can_bytes_to_dlc(8),
	};

	memcpy(resp.data, buf, 8);
	int ret = mod_can_send(&resp);

	if (ret == 0) {
		LOG_INF("RF24 RESP: cmd=0x%02x addr=%02x%02x%02x%02x%02x",
			cmd,
			global_params.rf24_addr[0], global_params.rf24_addr[1],
			global_params.rf24_addr[2], global_params.rf24_addr[3],
			global_params.rf24_addr[4]);
	} else {
		LOG_ERR("RF24 RESP send failed: %d", ret);
	}
}

static void mod_can_rf24_config(struct can_frame *frame)
{
	if (frame->dlc < 1) {
		return;
	}

	uint8_t cmd = frame->data[0];

	switch (cmd) {
	case RF24_CMD_GET_CONFIG:
		rf24_send_config_resp(cmd);
		break;
	default:
		LOG_WRN("RF24 CONFIG: unknown cmd 0x%02x", cmd);
		break;
	}
}

/* ================================================================
 * 业务帧 handler (注入 can_fw_upgrade 库)
 * 库 RX 线程收到非固件升级帧时调用。
 * ================================================================ */
static bool mod_can_app_rx(struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (frame->id) {
	case OVERBREAK_LASER:
	case COORD_XY:
	case COORD_Z:
		mod_can_parse_scanner(frame);
		return true;
	case RF24_CONFIG_CMD:
		mod_can_rf24_config(frame);
		return true;
	default:
		return false;
	}
}

static int mod_can_init(void)
{
	can_dev = can_fw_set_app_handler(mod_can_app_rx, NULL);
	return 0;
}

SYS_INIT(mod_can_init, APPLICATION, 10);
