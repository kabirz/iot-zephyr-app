/*
 * Copyright (c) 2026 Titan Mini contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Titan Mini 双核 RPMsg echo (CM85 主核):
 *  - 启动时经 ipc0 收发: CM33 开机发 "hello from cm33", 本侧回显
 *  - shell 命令 'cm33 send <msg>': 发给 CM33, CM33 echo 回来打印
 */

#include <zephyr/kernel.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <string.h>

LOG_MODULE_REGISTER(titan_rpmsg, LOG_LEVEL_INF);

static const struct device *ipc0_dev = DEVICE_DT_GET(DT_NODELABEL(ipc0));
static struct ipc_ept ept;
static K_SEM_DEFINE(bound_sem, 0, 1);

static void ep_bound(void *priv)
{
	k_sem_give(&bound_sem);
	LOG_INF("CM33 endpoint bound");
}

static void ep_recv(const void *data, size_t len, void *priv)
{
	char buf[64];

	len = MIN(len, sizeof(buf) - 1);
	memcpy(buf, data, len);
	buf[len] = '\0';
	LOG_INF("CM33 says: %s", buf);
}

static struct ipc_ept_cfg ep_cfg = {
	.name = "titan",
	.cb = {
		.bound = ep_bound,
		.received = ep_recv,
	},
};

static int cmd_cm33_send(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: cm33 send <msg>");
		return -EINVAL;
	}

	int ret = ipc_service_send(&ept, argv[1], strlen(argv[1]));

	if (ret < 0) {
		shell_error(sh, "send failed: %d", ret);
		return ret;
	}
	shell_print(sh, "sent %zu bytes to CM33", strlen(argv[1]));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cm33,
	SHELL_CMD_ARG(send, NULL, "cm33 send <msg>", cmd_cm33_send, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(cm33, &sub_cm33, "CM33 inter-core commands", NULL);

int main(void)
{
	int ret;

	if (!device_is_ready(ipc0_dev)) {
		LOG_ERR("ipc0 not ready");
		return -ENODEV;
	}

	ret = ipc_service_open_instance(ipc0_dev);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("ipc open failed: %d", ret);
		return ret;
	}

	ret = ipc_service_register_endpoint(ipc0_dev, &ept, &ep_cfg);
	if (ret < 0) {
		LOG_ERR("endpoint register failed: %d", ret);
		return ret;
	}

	LOG_INF("waiting for CM33...");
	k_sem_take(&bound_sem, K_FOREVER);

	/* 握手: 主动发一条, CM33 会 echo 回来 */
	ret = ipc_service_send(&ept, "hello from cm85", 16);
	if (ret < 0) {
		LOG_WRN("handshake send failed: %d", ret);
	}

	LOG_INF("RPMSG ready, try: cm33 send ping");
	return 0;
}
