/*
 * UDP performance benchmark shell commands, ported from
 * apps/applications/n2e-gw/src/net_shell.c (gw bench tx/rx/bidir).
 *
 *   bench tx [count=100] [size=64] [ip=192.168.12.100] [port=19602]
 *       board -> host one-way TX throughput (seq in payload for
 *       host-side loss accounting)
 *   bench rx [port=19601] [duration_ms=5000]
 *       host floods the board, board counts packets/bytes
 *   bench bidir [duration_ms=5000] [size=64] [rx_port=19601]
 *       one socket sending to host and draining its own RX
 *
 * Differences from the original: zsock_* API and a default host address
 * (this example has no persisted settings); the benchmark ports stay
 * 19601/19602 to keep away from the TCP test endpoints (4242/9900).
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(bench, LOG_LEVEL_INF);

#define BENCH_HOST_IP_DEF	"192.168.12.100"
#define BENCH_TX_COUNT_DEF	100
#define BENCH_TX_COUNT_MAX	1000
#define BENCH_TX_SIZE_DEF	64
#define BENCH_TX_SIZE_MAX	1024
#define BENCH_RX_DUR_DEF	5000
#define BENCH_RX_DUR_MAX	30000
#define BENCH_PORT_TX_DEF	19602
#define BENCH_PORT_RX_DEF	19601

static uint8_t bench_buf[BENCH_TX_SIZE_MAX];

static int cmd_bench_tx(const struct shell *ctx, size_t argc, char **argv)
{
	int count = BENCH_TX_COUNT_DEF;
	int size = BENCH_TX_SIZE_DEF;
	const char *ip = BENCH_HOST_IP_DEF;
	int port = BENCH_PORT_TX_DEF;

	if (argc >= 2) {
		count = (int)strtol(argv[1], NULL, 10);
		count = CLAMP(count, 1, BENCH_TX_COUNT_MAX);
	}
	if (argc >= 3) {
		size = (int)strtol(argv[2], NULL, 10);
		size = CLAMP(size, 1, BENCH_TX_SIZE_MAX);
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

	int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock < 0) {
		shell_error(ctx, "socket create failed: %d", errno);
		return -EIO;
	}

	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};

	if (zsock_inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
		shell_error(ctx, "invalid ip: %s", ip);
		zsock_close(sock);
		return -EINVAL;
	}

	memset(bench_buf, 0xAA, sizeof(bench_buf));
	shell_print(ctx, "TX bench: %d pkts x %d B -> %s:%d", count, size, ip, port);

	uint32_t t0 = k_uptime_get_32();
	int sent = 0;
	int fail = 0;

	for (int i = 0; i < count; i++) {
		sys_put_le32((uint32_t)i, bench_buf);
		int ret = zsock_sendto(sock, bench_buf, size, 0,
					(struct sockaddr *)&dst, sizeof(dst));
		if (ret < 0) {
			fail++;
			if (fail <= 3) {
				shell_warn(ctx, "  sendto failed @%d: errno %d",
					   i, errno);
			}
		} else {
			sent++;
		}
	}

	uint32_t elapsed = k_uptime_get_32() - t0;
	zsock_close(sock);

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
	int port = BENCH_PORT_RX_DEF;
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
		duration = CLAMP(duration, 100, BENCH_RX_DUR_MAX);
	}

	int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock < 0) {
		shell_error(ctx, "socket create failed: %d", errno);
		return -EIO;
	}

	struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};

	if (zsock_bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
		shell_error(ctx, "bind :%d failed: %d", port, errno);
		zsock_close(sock);
		return -EIO;
	}

	shell_print(ctx, "RX bench: listening on :%d for %d ms", port, duration);
	shell_print(ctx, "(send UDP to 192.168.12.200:%d from host)", port);

	uint32_t t0 = k_uptime_get_32();
	uint32_t deadline = t0 + (uint32_t)duration;
	uint32_t last_print = t0;
	uint32_t pkt_count = 0;
	uint32_t byte_count = 0;
	uint32_t max_gap_ms = 0;

	while (k_uptime_get_32() < deadline) {
		ssize_t n = zsock_recvfrom(sock, bench_buf, sizeof(bench_buf),
					   ZSOCK_MSG_DONTWAIT, NULL, NULL);
		if (n > 0) {
			pkt_count++;
			byte_count += (uint32_t)n;
		} else {
			uint32_t s0 = k_uptime_get_32();

			k_msleep(1);
			max_gap_ms = MAX(max_gap_ms, k_uptime_get_32() - s0);
		}
		uint32_t now = k_uptime_get_32();

		if (now - last_print >= 1000) {
			shell_print(ctx, "  ... %u pkts, %u B", pkt_count, byte_count);
			last_print = now;
		}
	}

	uint32_t elapsed = k_uptime_get_32() - t0;
	zsock_close(sock);

	shell_print(ctx, "done: %u pkts, %u B in %u ms", pkt_count, byte_count, elapsed);
	if (elapsed > 0 && pkt_count > 0) {
		shell_print(ctx, "rate: %u pkt/s, %u B/s",
			    pkt_count * 1000U / elapsed,
			    byte_count * 1000U / elapsed);
	}
	shell_print(ctx, "longest empty poll: %u ms", max_gap_ms);
	return 0;
}

static int cmd_bench_bidir(const struct shell *ctx, size_t argc, char **argv)
{
	int duration = BENCH_RX_DUR_DEF;
	int size = BENCH_TX_SIZE_DEF;
	int rx_port = BENCH_PORT_RX_DEF;

	if (argc >= 2) {
		duration = (int)strtol(argv[1], NULL, 10);
		duration = CLAMP(duration, 100, BENCH_RX_DUR_MAX);
	}
	if (argc >= 3) {
		size = (int)strtol(argv[2], NULL, 10);
		size = CLAMP(size, 1, BENCH_TX_SIZE_MAX);
	}
	if (argc >= 4) {
		rx_port = (int)strtol(argv[3], NULL, 10);
		if (rx_port < 1 || rx_port > 65535) {
			shell_error(ctx, "invalid port: %s", argv[3]);
			return -EINVAL;
		}
	}

	int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock < 0) {
		shell_error(ctx, "socket create failed: %d", errno);
		return -EIO;
	}

	struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_port = htons(rx_port),
	};

	if (zsock_bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
		shell_error(ctx, "bind :%d failed: %d", rx_port, errno);
		zsock_close(sock);
		return -EIO;
	}

	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port = htons(BENCH_PORT_TX_DEF),
	};

	if (zsock_inet_pton(AF_INET, BENCH_HOST_IP_DEF, &dst.sin_addr) != 1) {
		zsock_close(sock);
		return -EINVAL;
	}

	memset(bench_buf, 0xAA, sizeof(bench_buf));
	shell_print(ctx, "BIDIR: TX->%s:%d  RX on :%d  %d ms  %d B/pkt",
		    BENCH_HOST_IP_DEF, BENCH_PORT_TX_DEF, rx_port, duration, size);

	uint32_t t0 = k_uptime_get_32();
	uint32_t deadline = t0 + (uint32_t)duration;
	uint32_t last_print = t0;
	uint32_t tx_sent = 0, tx_fail = 0;
	uint32_t rx_pkts = 0, rx_bytes = 0;
	uint32_t seq = 0;

	while (k_uptime_get_32() < deadline) {
		sys_put_le32(seq++, bench_buf);
		int ret = zsock_sendto(sock, bench_buf, size, 0,
					(struct sockaddr *)&dst, sizeof(dst));
		if (ret < 0) {
			tx_fail++;
		} else {
			tx_sent++;
		}

		for (int drain = 0;
		     drain < 32 && k_uptime_get_32() < deadline;
		     drain++) {
			ssize_t n = zsock_recvfrom(sock, bench_buf,
						   sizeof(bench_buf),
						   ZSOCK_MSG_DONTWAIT, NULL, NULL);

			if (n <= 0) {
				break;
			}
			rx_pkts++;
			rx_bytes += (uint32_t)n;
		}

		uint32_t now = k_uptime_get_32();

		if (now - last_print >= 1000) {
			shell_print(ctx, "  ... TX %u, RX %u pkts", tx_sent, rx_pkts);
			last_print = now;
		}
	}

	uint32_t elapsed = k_uptime_get_32() - t0;
	zsock_close(sock);

	uint32_t tx_bytes = tx_sent * (uint32_t)size;

	shell_print(ctx, "TX: %u sent, %u failed, %u B", tx_sent, tx_fail, tx_bytes);
	shell_print(ctx, "RX: %u pkts, %u B", rx_pkts, rx_bytes);
	shell_print(ctx, "time: %u ms", elapsed);
	if (elapsed > 0) {
		shell_print(ctx, "TX rate: %u pkt/s, %u B/s",
			    tx_sent * 1000U / elapsed, tx_bytes * 1000U / elapsed);
		if (rx_pkts > 0) {
			shell_print(ctx, "RX rate: %u pkt/s, %u B/s",
				    rx_pkts * 1000U / elapsed,
				    rx_bytes * 1000U / elapsed);
		}
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_bench_cmds,
	SHELL_CMD_ARG(tx, NULL,
		      "bench tx [count=100] [size=64] [ip] [port=19602]",
		      cmd_bench_tx, 1, 4),
	SHELL_CMD_ARG(rx, NULL,
		      "bench rx [port=19601] [duration_ms=5000]",
		      cmd_bench_rx, 1, 2),
	SHELL_CMD_ARG(bidir, NULL,
		      "bench bidir [duration_ms=5000] [size=64] [rx_port=19601]",
		      cmd_bench_bidir, 1, 3),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(bench, &sub_bench_cmds, "UDP performance benchmark", NULL);
