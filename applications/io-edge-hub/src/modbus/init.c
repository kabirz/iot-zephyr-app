/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 系统初始化: settings 加载 + 心跳看门狗
 *
 *   - settings_subsys_init + settings_load (priority 11): 从 FCB 恢复
 *     holding_reg[] 持久化参数 (IP/Modbus/采样/心跳 等), 供后续 RTU/TCP/网络读取
 *   - heart_poll 线程: Modbus TCP 通信心跳, 超时无通信 (heart_event_send)
 *     且心跳使能时, 安全清零 DO 输出 (工业安全)
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <init.h>

LOG_MODULE_REGISTER(io_init, LOG_LEVEL_INF);

/* ==================== Settings 加载 ==================== */
static int modbus_settings_init(void)
{
	int rc = settings_subsys_init();

	if (rc) {
		LOG_ERR("settings_subsys_init failed: %d", rc);
		return rc;
	}

	rc = settings_load();
	if (rc) {
		LOG_ERR("settings_load failed: %d", rc);
		return rc;
	}

	LOG_INF("settings loaded (slave_id=%u ip=%u.%u.%u.%u)",
		get_holding_reg(HOLDING_SLAVE_ID_IDX),
		get_holding_reg(HOLDING_IP_ADDR_1_IDX),
		get_holding_reg(HOLDING_IP_ADDR_2_IDX),
		get_holding_reg(HOLDING_IP_ADDR_3_IDX),
		get_holding_reg(HOLDING_IP_ADDR_4_IDX));
	return 0;
}

/* priority 11: 在 dio/adc (12) 和 rtu (13) 之前加载持久化参数 */
SYS_INIT(modbus_settings_init, APPLICATION, 11);

/* ==================== 心跳看门狗 ==================== */
static K_SEM_DEFINE(heart_sem, 0, 1);

void heart_event_send(void)
{
	k_sem_give(&heart_sem);
}

static void heart_poll(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		uint32_t timeout = get_holding_reg(HOLDING_HEART_TIMEOUT_IDX);

		if (timeout < 500) {
			timeout = 500;
		}

		if (k_sem_take(&heart_sem, K_MSEC(timeout)) != 0) {
			/* 超时无 Modbus 通信: 心跳使能则安全断开所有 DO */
			if (get_holding_reg(HOLDING_HEART_EN_IDX)) {
				LOG_WRN("heartbeat timeout, clear DO");
				update_holding_reg(HOLDING_HEART_IDX, 0);
				update_holding_reg(HOLDING_DO_IDX, 0);
				mb_set_do(0);
			}
		}
	}
}

K_THREAD_DEFINE(heart, CONFIG_IO_HEART_STACK_SIZE, heart_poll,
		NULL, NULL, NULL, 12, 0, 0);
