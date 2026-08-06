/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 网络链路测试 shell 命令 — 模拟数据源, 验证 UDP 双端口链路
 *
 *   gw info              查看网络配置 (IP/掩码固定/网关/端口)
 *   gw send <id> <hex..> 发送数据帧 [帧ID 2B BE][payload]
 *   gw ping [count=5]    发送 TEST_FRAME (0x777) 计数测试
 *   gw ip <addr>         设置 IP (并持久化)
 *   gw port <port>       设置数据端口 (并持久化)
 *
 * (从 gateway_udp_test/src/shell.c 移植, 去掉数据端口 echo 回显 — n2e-gw 的
 *  UDP 数据端口只做 nRF24↔UDP 透传, 无回显机制)
 */

#ifdef CONFIG_SHELL

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/storage/flash_map.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <n2e_gw.h>

LOG_MODULE_REGISTER(gw_net_shell, LOG_LEVEL_INF);

#define GW_SHELL_PAYLOAD_MAX 64

/* ================================================================
 * Shell handlers
 * ================================================================ */

static int cmd_gw_info(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* 显示 live IP (DHCP 分配或静态配置的当前地址) */
	char ip_str[NET_IPV4_ADDR_LEN] = "-";
	struct in_addr *live = gw_get_live_ipv4();

	if (live) {
		net_addr_ntop(AF_INET, live, ip_str, sizeof(ip_str));
	}
	shell_print(ctx, "ip:        %s (%s)", ip_str, gw_params.use_dhcp ? "DHCP" : "static");
	if (gw_params.use_dhcp) {
		shell_print(ctx, "netmask:   (from DHCP)");
		shell_print(ctx, "gateway:   (from DHCP)");
	} else {
		/* 静态模式: 掩码固定 /24, 网关 = IP 末段改 1 (运行时派生) */
		struct in_addr gw;

		if (net_addr_pton(AF_INET, gw_params.ip_addr, &gw) == 0) {
			((uint8_t *)&gw.s_addr)[3] = 1;
			net_addr_ntop(AF_INET, &gw, ip_str, sizeof(ip_str));
		} else {
			ip_str[0] = '-';
			ip_str[1] = '\0';
		}
		shell_print(ctx, "netmask:   255.255.255.0 (fixed)");
		shell_print(ctx, "gateway:   %s (derived)", ip_str);
	}
	shell_print(ctx, "data port: %d", gw_params.data_port);
	shell_print(ctx, "cfg port:  %d", CONFIG_UDP_FW_CONFIG_PORT);
	shell_print(ctx, "running:   %s", gw_params.running ? "yes" : "no");
	return 0;
}

static int cmd_gw_send(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_error(ctx, "usage: gw send <id_hex> <b0> [b1] ...  (payload hex bytes)");
		return -EINVAL;
	}

	long id = strtol(argv[1], NULL, 16);
	if (id < 0 || id > 0x7FF) {
		shell_error(ctx, "invalid frame id: %s (0-0x7FF)", argv[1]);
		return -EINVAL;
	}

	uint8_t buf[GW_SHELL_PAYLOAD_MAX];
	size_t off = 2;

	sys_put_be16((uint16_t)id, buf);

	for (int i = 2; i < argc && off < sizeof(buf); i++) {
		long v = strtol(argv[i], NULL, 16);

		if (v < 0 || v > 0xFF) {
			shell_error(ctx, "invalid byte: %s", argv[i]);
			return -EINVAL;
		}
		buf[off++] = (uint8_t)v;
	}

	gw_udp_send(buf, off);
	shell_print(ctx, "TX id=0x%03x len=%zu", (uint16_t)id, off);
	return 0;
}

static int cmd_gw_ping(const struct shell *ctx, size_t argc, char **argv)
{
	int count = 5;

	if (argc >= 2) {
		count = (int)strtol(argv[1], NULL, 10);
		if (count < 1) {
			count = 1;
		} else if (count > 1000) {
			count = 1000;
		}
	}

	/* TEST_FRAME (0x777) 帧格式: [0x07 0x77][seq] */
	uint8_t seq = 0;

	shell_print(ctx, "ping TEST_FRAME(0x777) x%d (loopback via peer)...", count);

	for (int i = 0; i < count; i++) {
		uint8_t buf[3];

		sys_put_be16(TEST_FRAME, buf);
		buf[2] = seq++;
		gw_udp_send(buf, sizeof(buf));
		shell_print(ctx, "  [%d] TX seq=%02x", i + 1, buf[2]);
		k_msleep(200);
	}

	shell_print(ctx, "done (check if host PC received)");
	return 0;
}

static int cmd_gw_ip(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		/* 显示 live IP (DHCP 模式下是服务器分配的地址) */
		struct in_addr *live = gw_get_live_ipv4();
		char ip_str[NET_IPV4_ADDR_LEN];

		if (live) {
			net_addr_ntop(AF_INET, live, ip_str, sizeof(ip_str));
			shell_print(ctx, "ip: %s (%s)", ip_str,
				    gw_params.use_dhcp ? "DHCP" : "static");
		} else {
			shell_print(ctx, "ip: (no address, stored=%s)", gw_params.ip_addr);
		}
		return 0;
	}
	if (gw_params.use_dhcp) {
		shell_error(ctx, "cannot set static IP in DHCP mode");
		return -EINVAL;
	}
	if (strlen(argv[1]) >= sizeof(gw_params.ip_addr)) {
		shell_error(ctx, "ip too long");
		return -EINVAL;
	}
	strncpy(gw_params.ip_addr, argv[1], sizeof(gw_params.ip_addr) - 1);
	gw_params.ip_addr[sizeof(gw_params.ip_addr) - 1] = '\0';
	persist_save_network_config();
	shell_print(ctx, "ip set to %s (takes effect after reboot)", gw_params.ip_addr);
	return 0;
}

static int cmd_gw_port(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(ctx, "data port: %d", gw_params.data_port);
		return 0;
	}
	long p = strtol(argv[1], NULL, 10);
	if (p < 1 || p > 65535) {
		shell_error(ctx, "invalid port: %s (1-65535)", argv[1]);
		return -EINVAL;
	}
	gw_params.data_port = (uint16_t)p;
	persist_save_network_config();
	shell_print(ctx, "data port set to %d (takes effect after reboot)", gw_params.data_port);
	return 0;
}

static int cmd_gw_reset(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* 擦除整个 settings 分区 (FCB 后端), 清除所有持久化配置.
	 * 擦除后 gw_params 内存值仍在, 但重启后 settings 为空, 代码用默认值重新初始化. */
	const struct flash_area *fa;
	int rc = flash_area_open(PARTITION_ID(cfg_partition), &fa);

	if (rc != 0) {
		shell_error(ctx, "open settings partition failed: %d", rc);
		return -EIO;
	}
	rc = flash_area_erase(fa, 0, fa->fa_size);
	flash_area_close(fa);
	if (rc != 0) {
		shell_error(ctx, "erase settings partition failed: %d", rc);
		return -EIO;
	}
	shell_print(ctx, "All settings cleared (rf24/ip/port/dhcp)");
	shell_print(ctx, "Reboot to apply defaults");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_gw_cmds,
	SHELL_CMD_ARG(info, NULL, "Show network config", cmd_gw_info, 1, 0),
	SHELL_CMD_ARG(send, NULL,
		      "Send frame <id_hex> <b0> [b1] ... (payload hex bytes)",
		      cmd_gw_send, 3, 16),
	SHELL_CMD_ARG(ping, NULL, "Send TEST_FRAME (0x777) [count=5]", cmd_gw_ping, 1, 1),
	SHELL_CMD_ARG(ip, NULL, "Get/set IP <addr>", cmd_gw_ip, 1, 1),
	SHELL_CMD_ARG(port, NULL, "Get/set data port <1-65535>", cmd_gw_port, 1, 1),
	SHELL_CMD_ARG(reset, NULL, "Clear all settings (reboot to apply)", cmd_gw_reset, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(gw, &sub_gw_cmds, "gateway UDP network test commands", NULL);

#endif /* CONFIG_SHELL */
