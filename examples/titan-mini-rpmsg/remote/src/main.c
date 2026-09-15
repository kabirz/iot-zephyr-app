/*
 * Copyright (c) 2026 Titan Mini contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Titan Mini 双核 RPMsg echo (CM33 副核):
 * 收到任意消息原样 echo 回去; 绑定成功后先发一条开机问候.
 */

#include <zephyr/kernel.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <string.h>

static const struct device *ipc0_dev = DEVICE_DT_GET(DT_NODELABEL(ipc0));
static struct ipc_ept ept;
static K_SEM_DEFINE(bound_sem, 0, 1);

static void ep_recv(const void *data, size_t len, void *priv)
{
	char buf[64];

	len = MIN(len, sizeof(buf) - 1);
	memcpy(buf, data, len);
	buf[len] = '\0';
	printk("CM33 RX (%zu bytes): %s\n", len, buf);

	/* echo 回 CM85 */
	(void)ipc_service_send(&ept, data, len);
}

static void ep_bound(void *priv)
{
	k_sem_give(&bound_sem);
}

static struct ipc_ept_cfg ep_cfg = {
	.name = "titan",
	.cb = {
		.bound = ep_bound,
		.received = ep_recv,
	},
};

int main(void)
{
	printk("CM33 (secondary core) booted, console on uart2\n");

	if (!device_is_ready(ipc0_dev)) {
		return -ENODEV;
	}

	int ret = ipc_service_open_instance(ipc0_dev);

	if (ret < 0 && ret != -EALREADY) {
		return ret;
	}

	ret = ipc_service_register_endpoint(ipc0_dev, &ept, &ep_cfg);
	if (ret < 0) {
		return ret;
	}

	k_sem_take(&bound_sem, K_FOREVER);

	/* 开机问候: CM85 会收到并打印 */
	static const char hello[] = "hello from cm33";

	(void)ipc_service_send(&ept, hello, sizeof(hello) - 1);

	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
