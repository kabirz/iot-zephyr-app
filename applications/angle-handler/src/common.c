/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 通用链路发送：按 connect_type 分派 CAN / 2.4G (nRF24L01+)。
 * - send_handler_state：事件驱动发送手柄状态帧 (HANDLER_STATE)，由 gpio/adc 调用。
 * - heart_thread：周期发送心跳保活帧 (COBID_HEATBEAT)，CAN/RF24 共用。
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <common.h>
#include <mod-can.h>
#include <rf24.h>

LOG_MODULE_REGISTER(common_link, LOG_LEVEL_INF);

/* 手柄状态帧 (HANDLER_STATE=0x1E3)：[x 2B BE][y 2B BE][btn][0xFF...]
 * CAN 用 8 字节 dlc，RF24 用 7 字节 payload（无 CAN 的末字节 0xFF）。
 */
int send_handler_state(const uint8_t *data, uint8_t len)
{
	/* 最后防线: 休眠中禁止任何链路发送 (断电后 nRF24 SPI / CAN 操作会异常) */
	if (atomic_get(&global_params.sleeping)) {
		return -EBUSY;
	}

	if (global_params.connect_type == CAN_TYPE) {
		struct can_frame frame = {
			.id = HANDLER_STATE,
			.dlc = can_bytes_to_dlc(len),
		};
		memcpy(frame.data, data, len);
		return mod_can_send(&frame);
	} else {
		return rf24_data_send(HANDLER_STATE, data, len) ? 0 : -EIO;
	}
}

/* 周期心跳保活：发送 0x763 心跳帧，按 connect_type 走 CAN/RF24。
 * 等 CAN_EVENT 或 RF24_EVENT 任一置位（connect_switch 切换时置位）。
 */
static void heart_thread(void)
{
	while (true) {
		k_event_wait(&global_params.event, CAN_EVENT | RF24_EVENT, false, K_FOREVER);
		if (atomic_get(&global_params.sleeping)) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}
		/* 心跳保活帧：参照 CAN heart，帧 ID 0x763 (COBID_HEATBEAT)，数据 1 字节 (0x05) */
		uint8_t heartbeat = 5;
		if (global_params.connect_type == CAN_TYPE) {
			struct can_frame frame = {
				.id = COBID_HEATBEAT,
				.dlc = can_bytes_to_dlc(1),
			};
			frame.data[0] = heartbeat;
			mod_can_send(&frame);
		} else {
			rf24_data_send(COBID_HEATBEAT, &heartbeat, 1);
		}
		k_sleep(K_MSEC(global_params.report_period));
	}
}
K_THREAD_DEFINE(thread_heart, 1024, heart_thread, NULL, NULL, NULL, 11, 0, 0);
