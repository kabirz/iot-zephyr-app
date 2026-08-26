/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 计测器 IO 数量与 DO 访问接口 (通道引脚在 overlay /zephyr,user 节点)
 */
#ifndef CANOPEN_IO_IO_H
#define CANOPEN_IO_IO_H

#include <stdint.h>

#define CO_IO_DI_NUM 16
#define CO_IO_DO_NUM 8
#define CO_IO_AI_NUM 4

/* 写 DO 位图并联动 LED (同步更新 OD 0x2002 并触发 TPDO2) */
void co_io_set_do(uint16_t val);
uint16_t co_io_get_do(void);

#endif /* CANOPEN_IO_IO_H */
