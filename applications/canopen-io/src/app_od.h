/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CANOPEN_IO_APP_OD_H
#define CANOPEN_IO_APP_OD_H

/* 安装厂家对象的 OD 扩展 (0x2000/0x2001 passthrough, 0x2002 DO 联动,
 * 0x2004 钳位+保存/重启触发). 幂等, 每个 CANopen 初始化周期调用一次. */
void app_od_init(void);

#endif /* CANOPEN_IO_APP_OD_H */
