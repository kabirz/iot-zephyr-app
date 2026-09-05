/* SPDX-License-Identifier: Apache-2.0
 * FDCAN1 (PA11/PA12, 500kbit/s): 正常模式, 接收所有标准帧并计数,
 * 前 8 帧逐帧打印, 之后每 100 帧采样打印 (避免总线繁忙时刷屏)。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

#include "can.h"

LOG_MODULE_REGISTER(apollo_can, LOG_LEVEL_INF);

#define CAN_NODE DT_NODELABEL(fdcan1)
static const struct device *const can_dev = DEVICE_DT_GET(CAN_NODE);

CAN_MSGQ_DEFINE(can_rx_msgq, 16);

static struct k_thread rx_thread;
static K_THREAD_STACK_DEFINE(rx_thread_stack, CONFIG_APOLLO_CAN_RX_STACK_SIZE);

static atomic_t rx_count;
static atomic_t tx_count;
static bool can_started;

static void can_rx_thread(void *p1, void *p2, void *p3)
{
	struct can_frame frame;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		if (k_msgq_get(&can_rx_msgq, &frame, K_FOREVER) != 0) {
			continue;
		}
		uint32_t n = atomic_inc(&rx_count) + 1;

		if (n <= 8 || (n % 100) == 0) {
			LOG_INF("CAN rx #%u id=0x%03x dlc=%u", n, frame.id, frame.dlc);
		}
	}
}

void apollo_can_start(void)
{
	struct can_filter filter = {
		.id = 0,
		.mask = 0,
		.flags = 0,	/* 所有标准帧 */
	};
	int ret;

	if (!device_is_ready(can_dev)) {
		LOG_WRN("FDCAN1 not ready (本变体未启用 CAN?)");
		return;
	}

	ret = can_start(can_dev);
	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("FDCAN1 start failed: %d", ret);
		return;
	}
	ret = can_add_rx_filter_msgq(can_dev, &can_rx_msgq, &filter);
	if (ret < 0) {
		LOG_ERR("FDCAN1 add filter failed: %d", ret);
		return;
	}

	k_thread_create(&rx_thread, rx_thread_stack,
			K_THREAD_STACK_SIZEOF(rx_thread_stack),
			can_rx_thread, NULL, NULL, NULL,
			10, 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread, "apollo_can_rx");

	can_started = true;
	LOG_INF("FDCAN1 up @%u kbit/s (normal mode)",
		(unsigned)DT_PROP(CAN_NODE, bitrate) / 1000);
}

int can_send_frame(uint32_t id, const uint8_t *data, size_t len)
{
	struct can_frame frame = {
		.id = id,
		.dlc = len,
		.flags = 0,
	};
	int ret;

	if (!device_is_ready(can_dev)) {
		return -ENODEV;
	}
	if (id > CAN_STD_ID_MASK || len > sizeof(frame.data)) {
		return -EINVAL;
	}
	memcpy(frame.data, data, len);

	ret = can_send(can_dev, &frame, K_MSEC(100), NULL, NULL);
	if (ret == 0) {
		atomic_inc(&tx_count);
	}
	return ret;
}

void can_get_stats(uint32_t *rx, uint32_t *tx)
{
	if (rx != NULL) {
		*rx = atomic_get(&rx_count);
	}
	if (tx != NULL) {
		*tx = atomic_get(&tx_count);
	}
}

bool can_is_up(void)
{
	return can_started;
}
