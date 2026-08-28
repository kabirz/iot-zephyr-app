/*
 * Copyright (c) 2026 GD32H7xx Zephyr bring-up
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN2 self-tests (classic loopback, CAN-FD 64-byte loopback), then
 * normal classic mode for interop with the Linux SocketCAN side.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(can_loopback, LOG_LEVEL_INF);

#define CAN_NODE DT_NODELABEL(can2)
static const struct device *const can_dev = DEVICE_DT_GET(CAN_NODE);

static struct k_sem rx_done;
static struct can_frame last_rx;

static void rx_cb(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	memcpy(&last_rx, frame, sizeof(*frame));
	k_sem_give(&rx_done);
}

static void tx_cb(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	LOG_INF("tx done: %d", error);
}

/* one classic 8-byte loopback round-trip */
static int test_classic_loopback(void)
{
	struct can_filter filter = {
		.id = 0x123,
		.mask = 0x7FF,
		.flags = 0,
	};
	struct can_frame tx = {
		.id = 0x123,
		.dlc = 8,
		.data = {1, 2, 3, 4, 5, 6, 7, 8},
	};
	int filter_id;
	int ret;

	ret = can_set_mode(can_dev, CAN_MODE_LOOPBACK);
	if (ret != 0) {
		LOG_ERR("set loopback mode failed: %d", ret);
		return ret;
	}
	ret = can_start(can_dev);
	if (ret != 0) {
		LOG_ERR("can_start failed: %d", ret);
		return ret;
	}

	filter_id = can_add_rx_filter(can_dev, rx_cb, NULL, &filter);
	if (filter_id < 0) {
		LOG_ERR("add filter failed: %d", filter_id);
		return filter_id;
	}

	ret = can_send(can_dev, &tx, K_MSEC(200), tx_cb, NULL);
	if (ret != 0) {
		LOG_ERR("loopback send failed: %d", ret);
		return ret;
	}

	ret = k_sem_take(&rx_done, K_MSEC(500));
	if (ret == 0 && last_rx.id == tx.id && last_rx.dlc == tx.dlc &&
	    memcmp(last_rx.data, tx.data, can_dlc_to_bytes(tx.dlc)) == 0) {
		LOG_INF("CLASSIC LOOPBACK OK: id=0x%03x dlc=%u data ok",
			last_rx.id, last_rx.dlc);
		ret = 0;
	} else {
		LOG_ERR("CLASSIC LOOPBACK FAILED (ret=%d id=0x%x dlc=%u)",
			ret, last_rx.id, last_rx.dlc);
		ret = -EIO;
	}

	can_remove_rx_filter(can_dev, filter_id);
	(void)can_stop(can_dev);

	return ret;
}

/* one CAN-FD 64-byte loopback round-trip with bit rate switching */
static int test_fd_loopback(void)
{
	struct can_filter filter = {
		.id = 0x456,
		.mask = 0x7FF,
		.flags = 0,
	};
	struct can_frame tx = {
		.id = 0x456,
		.dlc = 0x0f, /* DLC 15 == 64 bytes */
		.flags = CAN_FRAME_FDF | CAN_FRAME_BRS,
	};
	int filter_id;
	int ret;

	for (uint8_t i = 0; i < sizeof(tx.data); i++) {
		tx.data[i] = i;
	}

	ret = can_set_mode(can_dev, CAN_MODE_LOOPBACK | CAN_MODE_FD);
	if (ret != 0) {
		LOG_ERR("set fd loopback mode failed: %d", ret);
		return ret;
	}
	ret = can_start(can_dev);
	if (ret != 0) {
		LOG_ERR("can_start (fd) failed: %d", ret);
		return ret;
	}

	filter_id = can_add_rx_filter(can_dev, rx_cb, NULL, &filter);
	if (filter_id < 0) {
		LOG_ERR("add filter failed: %d", filter_id);
		return filter_id;
	}

	ret = can_send(can_dev, &tx, K_MSEC(200), tx_cb, NULL);
	if (ret != 0) {
		LOG_ERR("fd loopback send failed: %d", ret);
		return ret;
	}

	ret = k_sem_take(&rx_done, K_MSEC(500));
	if (ret == 0 && last_rx.id == tx.id && last_rx.dlc == tx.dlc &&
	    (last_rx.flags & (CAN_FRAME_FDF | CAN_FRAME_BRS)) ==
		    (CAN_FRAME_FDF | CAN_FRAME_BRS) &&
	    memcmp(last_rx.data, tx.data, sizeof(tx.data)) == 0) {
		LOG_INF("CAN-FD LOOPBACK OK: id=0x%03x dlc=%u (%u bytes), "
			"FDF+BRS, data[0..63] ok",
			last_rx.id, last_rx.dlc, can_dlc_to_bytes(last_rx.dlc));
		ret = 0;
	} else {
		LOG_ERR("CAN-FD LOOPBACK FAILED (ret=%d id=0x%x dlc=%u flags=0x%x)",
			ret, last_rx.id, last_rx.dlc, last_rx.flags);
		ret = -EIO;
	}

	can_remove_rx_filter(can_dev, filter_id);
	(void)can_stop(can_dev);

	return ret;
}

int main(void)
{
	struct can_filter filter = {
		.id = 0,
		.mask = 0,
		.flags = 0,
	};
	int filter_id;
	int ret;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN2 device not ready");
		return 0;
	}

	k_sem_init(&rx_done, 0, 1);

	(void)test_classic_loopback();
	(void)test_fd_loopback();

	ret = can_set_mode(can_dev, CAN_MODE_NORMAL);
	if (ret == 0) {
		ret = can_start(can_dev);
	}
	if (ret != 0) {
		LOG_ERR("back to normal mode failed: %d", ret);
		return 0;
	}
	filter_id = can_add_rx_filter(can_dev, rx_cb, NULL, &filter);
	ARG_UNUSED(filter_id);

	LOG_INF("CAN2 in normal mode @1Mbps classic, use the can shell to talk to Linux can0");

	return 0;
}
