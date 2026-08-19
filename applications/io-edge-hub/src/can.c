/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 业务帧处理 (CAN1)
 *
 *   - can_fw_upgrade 库 (SYS_INIT) 初始化 CAN + 拥有 RX 线程, 处理固件升级帧
 *     (0x101-0x105); 其他帧分发给本模块的 mod_can_app_rx。
 *   - 业务帧 ID = holding_reg[CAN_ID] (默认 0x0111)。
 *   - 提供 mod_can_send() 供应用发送业务帧。
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>
#include <can_fw_upgrade.h>
#include <init.h>
#include "can.h"

LOG_MODULE_REGISTER(io_can, LOG_LEVEL_INF);

static const struct device *can_dev;

static void mod_can_tx_cb(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	if (error) {
		LOG_WRN("CAN tx error: %d", error);
	}
}

int mod_can_send(uint32_t id, const uint8_t *data, uint8_t len)
{
	if (can_dev == NULL || len > 8) {
		return -EINVAL;
	}

	struct can_frame frame = {
		.id = id,
		.dlc = can_bytes_to_dlc(len),
	};

	memcpy(frame.data, data, len);
	return can_send(can_dev, &frame, K_MSEC(100), mod_can_tx_cb, NULL);
}

/* 业务帧回调 (库 RX 线程上下文) */
static bool mod_can_app_rx(struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(user_data);

	if (frame->id == get_holding_reg(HOLDING_CAN_ID_IDX)) {
		LOG_DBG("CAN business frame id=0x%03x dlc=%u", frame->id, frame->dlc);
		/* 业务帧处理预留 (本项目 CAN 主要用于固件升级) */
		return true;
	}
	return false;
}

static int can_app_init(void)
{
	can_dev = can_fw_set_app_handler(mod_can_app_rx, NULL);
	if (can_dev == NULL) {
		LOG_ERR("CAN app handler registration failed");
		return -ENODEV;
	}
	LOG_INF("CAN app handler ready (bus id=0x%03x)",
		get_holding_reg(HOLDING_CAN_ID_IDX));
	return 0;
}

SYS_INIT(can_app_init, APPLICATION, CONFIG_IO_INIT_PRIORITY_CAN);
