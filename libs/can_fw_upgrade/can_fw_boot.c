/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 固件升级库 - MCUboot 启动等待钩子 (boot_go_hook)
 *
 * 在镜像校验/交换前: PROBE_TIMEOUT_MS 窗口内周期发 0x106 探测帧,
 * 收到上位机 0x107 响应则进入升级等待 (IDLE_TIMEOUT_MS 内无固件帧则
 * 继续启动); CONFIRM 后结束等待, 由 boot_go 在本会话内直接完成 swap。
 * 返回前 can_stop(), 应用侧 SYS_INIT 会重新初始化 CAN。
 * 协议处理与 can_fw_upgrade.c 共用 (同源编译, RX 线程复用);
 * 阶段经 0x108 trace 帧广播 (串口丢失时的黑匣子)。
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/app_version.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>
#include "bootutil/bootutil.h"
#include "bootutil/boot_hooks.h"
#include "bootutil/fault_injection_hardening.h"
#include "can_fw_upgrade_internal.h"

LOG_MODULE_REGISTER(can_fw_boot, LOG_LEVEL_INF);

/* 探测帧重发间隔 */
#define CAN_FW_BOOT_PROBE_INTERVAL_MS	200
/* 等待轮询粒度 */
#define CAN_FW_BOOT_POLL_MS		50

static const struct device *const boot_can_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

static void send_boot_probe(void)
{
	struct can_frame frame = {
		.id = CAN_FW_BOOT_PROBE_TX,
		.dlc = can_bytes_to_dlc(8),
	};

	frame.data_32[0] = CAN_FW_BOOT_PROBE_MAGIC;
	frame.data[4] = APP_VERSION_MAJOR;
	frame.data[5] = APP_VERSION_MINOR;
	frame.data[6] = APP_PATCHLEVEL;
	frame.data[7] = 0;

	/* 总线上无主机时经典 CAN 帧层无应答, can_send 同步超时 100ms 返回 */
	(void)can_send(boot_can_dev, &frame, K_MSEC(100), NULL, NULL);
}

static void send_boot_trace(uint8_t phase)
{
	struct can_frame t = {
		.id = CAN_FW_BOOT_TRACE_TX,
		.dlc = can_bytes_to_dlc(1),
	};

	t.data[0] = phase;
	(void)can_send(boot_can_dev, &t, K_MSEC(100), NULL, NULL);
}

fih_ret boot_go_hook(struct boot_rsp *rsp)
{
	uint32_t deadline, next_probe;
	bool acked = false;

	(void)rsp;

	if (!device_is_ready(boot_can_dev)) {
		FIH_RET(FIH_BOOT_HOOK_REGULAR);
	}

	send_boot_trace(CAN_FW_TRACE_HOOK_ENTER);

	/* 清掉历史 sem 计数后再探测, 防止误判 */
	while (k_sem_take(&can_fw_boot_ack_sem, K_NO_WAIT) == 0) {
	}

	deadline = k_uptime_get_32() + CONFIG_CAN_FW_UPGRADE_PROBE_TIMEOUT_MS;
	next_probe = 0;
	while (!acked) {
		uint32_t now = k_uptime_get_32();

		if ((int32_t)(now - deadline) >= 0) {
			break;
		}
		if ((int32_t)(now - next_probe) >= 0) {
			send_boot_probe();
			next_probe = now + CAN_FW_BOOT_PROBE_INTERVAL_MS;
		}
		if (k_sem_take(&can_fw_boot_ack_sem,
			       K_MSEC(MIN(deadline - now,
					  CAN_FW_BOOT_POLL_MS))) == 0) {
			acked = true;
		}
	}

	if (acked) {
		send_boot_trace(CAN_FW_TRACE_HOST_ACK);
		LOG_INF("host detected, waiting CAN firmware (idle %us)",
			CONFIG_CAN_FW_UPGRADE_IDLE_TIMEOUT_MS / 1000);

		/* 丢弃 ACK 前滞留的旧帧 (msgq/驱动缓冲): 上位机为让运行中
		 * 应用重启进 bootloader 而发的 REBOOT 帧若在此被处理, 会经
		 * FW_CMD_REBOOT 的 BOOT_WAIT 分支置 can_fw_confirmed, 等待
		 * 循环立即退出, 后续 keyhash/START 全部丢失 */
		{
			struct can_frame stale;

			while (k_msgq_get(&can_fw_rx_msgq, &stale, K_NO_WAIT) == 0) {
			}
		}
		/* REBOOT 帧可能在 ACK 之前已被 RX 线程处理并置位, 一并复位 */
		can_fw_confirmed = false;

		can_fw_last_activity_ms = k_uptime_get_32();
		while (!can_fw_confirmed) {
			uint32_t now = k_uptime_get_32();

			if ((int32_t)(now - can_fw_last_activity_ms) >=
			    CONFIG_CAN_FW_UPGRADE_IDLE_TIMEOUT_MS &&
			    !can_fw_rx_busy) {
				/* idle 到期退出前必须确认 RX 线程不在长命令中
				 * (START 擦 slot0 阻塞数秒且期间无帧刷新活动),
				 * 否则 can_stop() 会掐死命令完成后的回复帧,
				 * 上位机表现为 START 15s 超时 */
				break;
			}
			k_msleep(CAN_FW_BOOT_POLL_MS);
		}

		if (can_fw_confirmed) {
			send_boot_trace(CAN_FW_TRACE_CONFIRMED);
			LOG_INF("firmware confirmed, swap in this session");
		} else {
			LOG_INF("no firmware activity, boot app");
		}
	}

	send_boot_trace(CAN_FW_TRACE_PROCEED_BOOT);

	(void)can_stop(boot_can_dev);

	FIH_RET(FIH_BOOT_HOOK_REGULAR);
}
