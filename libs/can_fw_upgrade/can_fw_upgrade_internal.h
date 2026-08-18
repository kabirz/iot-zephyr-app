/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 固件升级库内部共享接口 (can_fw_upgrade.c / can_fw_boot.c),
 * 不对应用暴露。
 */

#ifndef __CAN_FW_UPGRADE_INTERNAL_H__
#define __CAN_FW_UPGRADE_INTERNAL_H__

#include <zephyr/kernel.h>
#include <stdbool.h>

/* 0x106 bootloader 探测 (设备发): [0..3]='B''T''O''1', [4..6]=版本 M.m.p */
#define CAN_FW_BOOT_PROBE_TX	0x106
/* 0x107 探测响应 (上位机发, 任意 1B) */
#define CAN_FW_BOOT_ACK_RX	0x107
#define CAN_FW_BOOT_PROBE_MAGIC	0x42544F31U  /* "BTO1" */

/* 0x108 阶段记录帧 (仅 BOOT_WAIT 构建, data[0]=阶段码) */
#define CAN_FW_BOOT_TRACE_TX	0x108

enum can_fw_boot_trace {
	CAN_FW_TRACE_INIT_DONE = 1,	/* can_fw_init 完成 */
	CAN_FW_TRACE_HOOK_ENTER,	/* boot_go_hook 进入 */
	CAN_FW_TRACE_HOST_ACK,		/* 收到探测响应 */
	CAN_FW_TRACE_CONFIRMED,		/* CONFIRM, 将结束等待 */
	CAN_FW_TRACE_PROCEED_BOOT,	/* 等待结束, 继续启动 */
	CAN_FW_TRACE_FW_START = 6,	/* 收到 START_UPDATE (诊断: 帧已到达协议层) */
};

/* bootloader 等待状态 (RX 线程写, boot_go_hook 读) */
extern struct k_sem can_fw_boot_ack_sem;
extern volatile uint32_t can_fw_last_activity_ms;
extern volatile bool can_fw_confirmed;
/* RX 线程正在处理长命令 (擦 flash 等, 阻塞数秒): boot_go_hook 不得
 * 因 idle 超时退出并 can_stop() — 否则命令完成后的回复帧被掐死 */
extern volatile bool can_fw_rx_busy;
extern struct k_msgq can_fw_rx_msgq;

#endif /* __CAN_FW_UPGRADE_INTERNAL_H__ */
