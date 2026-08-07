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
 *   gw bench tx [n] [sz] [ip] [port]   单向 TX 吞吐基准
 *   gw bench rx [port] [duration_ms]   单向 RX 吞吐基准
 *
 * (从 gateway_udp_test/src/shell.c 移植, 去掉数据端口 echo 回显 — n2e-gw 的
 *  UDP 数据端口只做 nRF24↔UDP 透传, 无回显机制)
 */

#ifdef CONFIG_SHELL

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/storage/flash_map.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
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

/* ================================================================
 * UDP 性能测试 (gw bench tx / gw bench rx)
 * ================================================================
 * 单向吞吐量基准, 用于验证 W5500 网络收发性能及配置通道抗饿死能力.
 *
 * bench tx [count=100] [size=64] [ip] [port]
 *   直接 socket sendto 发包 (绕过 gw_udp_send/msgq), 测量原始 TX 吞吐.
 *   包载荷: [seq 4B LE][0xAA 填充], 上位机可用任意 UDP 收包工具统计.
 *
 * bench rx [port=data_port+1] [duration_ms=5000]
 *   临时 socket 绑定 port 收包统计; 上位机用 UDP 发包工具连续发包.
 * ================================================================ */
#define BENCH_TX_COUNT_DEF	100
#define BENCH_TX_COUNT_MAX	1000
#define BENCH_TX_SIZE_DEF	64
#define BENCH_TX_SIZE_MAX	1024
#define BENCH_RX_DUR_DEF	5000
#define BENCH_RX_DUR_MAX	30000

static uint8_t bench_buf[BENCH_TX_SIZE_MAX];

static int cmd_bench_tx(const struct shell *ctx, size_t argc, char **argv)
{
	int count = BENCH_TX_COUNT_DEF;
	int size = BENCH_TX_SIZE_DEF;
	const char *ip = gw_params.host_ip;
	int port = gw_params.host_port;

	if (argc >= 2) {
		count = (int)strtol(argv[1], NULL, 10);
		if (count < 1) {
			count = 1;
		}
		if (count > BENCH_TX_COUNT_MAX) {
			count = BENCH_TX_COUNT_MAX;
		}
	}
	if (argc >= 3) {
		size = (int)strtol(argv[2], NULL, 10);
		if (size < 1) {
			size = 1;
		}
		if (size > BENCH_TX_SIZE_MAX) {
			size = BENCH_TX_SIZE_MAX;
		}
	}
	if (argc >= 4) {
		ip = argv[3];
	}
	if (argc >= 5) {
		port = (int)strtol(argv[4], NULL, 10);
		if (port < 1 || port > 65535) {
			shell_error(ctx, "invalid port: %s", argv[4]);
			return -EINVAL;
		}
	}

	if (!gw_net_link_up) {
		shell_error(ctx, "net link down");
		return -EIO;
	}

	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock < 0) {
		shell_error(ctx, "socket create failed: %d", errno);
		return -EIO;
	}

	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};

	if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
		shell_error(ctx, "invalid ip: %s", ip);
		close(sock);
		return -EINVAL;
	}

	memset(bench_buf, 0xAA, sizeof(bench_buf));
	shell_print(ctx, "TX bench: %d pkts x %d B -> %s:%d", count, size, ip, port);

	uint32_t t0 = k_uptime_get_32();
	int sent = 0;
	int fail = 0;

	for (int i = 0; i < count; i++) {
		sys_put_le32((uint32_t)i, bench_buf);   /* seq 供接收端统计丢包 */
		int ret = sendto(sock, bench_buf, size, 0,
				 (struct sockaddr *)&dst, sizeof(dst));
		if (ret < 0) {
			fail++;
			if (fail <= 3) {
				shell_warn(ctx, "  sendto failed @%d: errno %d", i, errno);
			}
		} else {
			sent++;
		}
	}

	uint32_t elapsed = k_uptime_get_32() - t0;
	close(sock);

	uint32_t total_bytes = (uint32_t)sent * (uint32_t)size;

	shell_print(ctx, "done: %d sent, %d failed", sent, fail);
	shell_print(ctx, "data: %u B in %u ms", total_bytes, elapsed);
	if (elapsed > 0) {
		shell_print(ctx, "rate: %u pkt/s, %u B/s",
			    (uint32_t)sent * 1000U / elapsed,
			    total_bytes * 1000U / elapsed);
	}
	return 0;
}

static int cmd_bench_rx(const struct shell *ctx, size_t argc, char **argv)
{
	int port = gw_params.data_port + 1;
	int duration = BENCH_RX_DUR_DEF;

	if (argc >= 2) {
		port = (int)strtol(argv[1], NULL, 10);
		if (port < 1 || port > 65535) {
			shell_error(ctx, "invalid port: %s", argv[1]);
			return -EINVAL;
		}
	}
	if (argc >= 3) {
		duration = (int)strtol(argv[2], NULL, 10);
		if (duration < 100) {
			duration = 100;
		}
		if (duration > BENCH_RX_DUR_MAX) {
			duration = BENCH_RX_DUR_MAX;
		}
	}

	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock < 0) {
		shell_error(ctx, "socket create failed: %d", errno);
		return -EIO;
	}

	struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};

	if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
		shell_error(ctx, "bind :%d failed: %d", port, errno);
		close(sock);
		return -EIO;
	}

	shell_print(ctx, "RX bench: listening on :%d for %d ms", port, duration);
	shell_print(ctx, "(send UDP to device_ip:%d from host)", port);

	uint32_t t0 = k_uptime_get_32();
	uint32_t deadline = t0 + (uint32_t)duration;
	uint32_t last_print = t0;
	uint32_t pkt_count = 0;
	uint32_t byte_count = 0;

	while (k_uptime_get_32() < deadline) {
		ssize_t n = recvfrom(sock, bench_buf, sizeof(bench_buf),
				     MSG_DONTWAIT, NULL, NULL);
		if (n > 0) {
			pkt_count++;
			byte_count += (uint32_t)n;
		} else {
			k_msleep(1);
		}
		uint32_t now = k_uptime_get_32();

		if (now - last_print >= 1000) {
			shell_print(ctx, "  ... %u pkts, %u B", pkt_count, byte_count);
			last_print = now;
		}
	}

	uint32_t elapsed = k_uptime_get_32() - t0;

	close(sock);

	shell_print(ctx, "done: %u pkts, %u B in %u ms", pkt_count, byte_count, elapsed);
	if (elapsed > 0 && pkt_count > 0) {
		shell_print(ctx, "rate: %u pkt/s, %u B/s",
			    pkt_count * 1000U / elapsed,
			    byte_count * 1000U / elapsed);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_bench_cmds,
	SHELL_CMD_ARG(tx, NULL,
		      "TX benchmark [count=100] [size=64] [ip] [port]",
		      cmd_bench_tx, 1, 4),
	SHELL_CMD_ARG(rx, NULL,
		      "RX benchmark [port=data_port+1] [duration_ms=5000]",
		      cmd_bench_rx, 1, 2),
	SHELL_SUBCMD_SET_END);

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
	SHELL_CMD(bench, &sub_bench_cmds, "UDP performance benchmark", NULL),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(gw, &sub_gw_cmds, "gateway UDP network test commands", NULL);

#endif /* CONFIG_SHELL */
