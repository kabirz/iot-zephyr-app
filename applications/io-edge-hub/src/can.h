/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 业务帧接口 (固件升级帧 0x101-0x105 由 can_fw_upgrade 库处理)
 */

#ifndef __CAN_H__
#define __CAN_H__

#include <stdint.h>

/* 发送一帧业务 CAN 数据 (id 从 holding_reg[CAN_ID] 或调用方指定) */
int mod_can_send(uint32_t id, const uint8_t *data, uint8_t len);

#endif /* __CAN_H__ */
