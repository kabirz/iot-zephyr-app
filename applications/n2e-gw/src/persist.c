/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings 持久化存储 - 使用 Zephyr settings 子系统 (FCB 后端)
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <n2e_gw.h>

LOG_MODULE_REGISTER(gw_persist, LOG_LEVEL_INF);

static int gw_persist_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	size_t name_len = settings_name_next(name, &next);

	if (!next && !strncmp(name, "rf24_addr", name_len)) {
		if (len == RF24_ADDR_LEN) {
			read_cb(cb_arg, gw_params.rf24_addr, RF24_ADDR_LEN);
		}
		return 0;
	}

	if (!next && !strncmp(name, "ip_addr", name_len)) {
		if (len < sizeof(gw_params.ip_addr)) {
			read_cb(cb_arg, gw_params.ip_addr, len);
			gw_params.ip_addr[len] = '\0';
		}
		return 0;
	}

	if (!next && !strncmp(name, "data_port", name_len)) {
		if (len == sizeof(uint16_t)) {
			read_cb(cb_arg, &gw_params.data_port, sizeof(uint16_t));
		}
		return 0;
	}

	if (!next && !strncmp(name, "use_dhcp", name_len)) {
		if (len == sizeof(uint8_t)) {
			read_cb(cb_arg, &gw_params.use_dhcp, sizeof(uint8_t));
		}
		return 0;
	}

	if (!next && !strncmp(name, "host_ip", name_len)) {
		if (len < sizeof(gw_params.host_ip)) {
			read_cb(cb_arg, gw_params.host_ip, len);
			gw_params.host_ip[len] = '\0';
		}
		return 0;
	}

	if (!next && !strncmp(name, "host_port", name_len)) {
		if (len == sizeof(uint16_t)) {
			read_cb(cb_arg, &gw_params.host_port, sizeof(uint16_t));
		}
		return 0;
	}

	return -ENOENT;
}

static int gw_persist_export(int (*cb)(const char *name, const void *value, size_t val_len))
{
	(void)cb("gw/rf24_addr", gw_params.rf24_addr, RF24_ADDR_LEN);
	(void)cb("gw/ip_addr", gw_params.ip_addr, strlen(gw_params.ip_addr));
	(void)cb("gw/data_port", &gw_params.data_port, sizeof(gw_params.data_port));
	(void)cb("gw/use_dhcp", &gw_params.use_dhcp, sizeof(gw_params.use_dhcp));
	(void)cb("gw/host_ip", gw_params.host_ip, strlen(gw_params.host_ip));
	(void)cb("gw/host_port", &gw_params.host_port, sizeof(gw_params.host_port));
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(gw_persist, "gw", NULL, gw_persist_set, NULL, gw_persist_export);

static int settings_backend_init(void)
{
	int rc = settings_subsys_init();

	if (rc) {
		LOG_ERR("settings_subsys_init failed: %d", rc);
	} else {
		LOG_INF("Settings subsystem initialized");
		/* 清除旧协议残留的历史键 (掩码/网关/信道已不再持久化).
		 * 不删除的话 settings_load 回放时会打 -ENOENT 错误日志. */
		settings_delete("gw/netmask");
		settings_delete("gw/gateway");
		settings_delete("gw/rf24_channel");
	}
	return rc;
}

void persist_save_rf24_config(void)
{
	settings_save_one("gw/rf24_addr", gw_params.rf24_addr, RF24_ADDR_LEN);
	LOG_INF("Saved rf24 addr=%02x%02x%02x%02x%02x", gw_params.rf24_addr[0],
		gw_params.rf24_addr[1], gw_params.rf24_addr[2], gw_params.rf24_addr[3],
		gw_params.rf24_addr[4]);
}

void persist_save_network_config(void)
{
	settings_save_one("gw/ip_addr", gw_params.ip_addr, strlen(gw_params.ip_addr));
	settings_save_one("gw/data_port", &gw_params.data_port, sizeof(gw_params.data_port));
	settings_save_one("gw/use_dhcp", &gw_params.use_dhcp, sizeof(gw_params.use_dhcp));
	settings_save_one("gw/host_ip", gw_params.host_ip, strlen(gw_params.host_ip));
	settings_save_one("gw/host_port", &gw_params.host_port, sizeof(gw_params.host_port));
	LOG_INF("Saved network: ip=%s port=%d dhcp=%d host=%s:%d", gw_params.ip_addr,
		gw_params.data_port, gw_params.use_dhcp, gw_params.host_ip,
		gw_params.host_port);
}

SYS_INIT(settings_backend_init, APPLICATION, 10);
