/* SPDX-License-Identifier: Apache-2.0 */
#ifndef APOLLO_CAN_H
#define APOLLO_CAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* FDCAN1 (CAN 变体): 正常模式启动 + 收包计数/日志 + 发送接口 */

void apollo_can_start(void);

/* 发送一帧经典 CAN (标准 ID, <=8 字节); 返回 0 成功 */
int can_send_frame(uint32_t id, const uint8_t *data, size_t len);

void can_get_stats(uint32_t *rx, uint32_t *tx);

bool can_is_up(void);

#endif /* APOLLO_CAN_H */
