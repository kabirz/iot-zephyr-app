/*
 * Bandwidth test endpoint, ported from io-edge-hub's src/bwtest.c
 * (temporary product measurement code) to this board bring-up example.
 *
 *  - TCP server on port 9900
 *  - single connection; the first byte selects the mode:
 *      'R'  receive-only sink (device downlink / RX capacity)
 *      any  echo (bidirectional throughput)
 *  - run from a host with the scripts in scripts/ or any TCP client
 *
 * Differences from the original: zsock_* calls (this fork's example has
 * no POSIX socket names), TCP_NODELAY on the echo socket (the Zephyr
 * stack otherwise serialises the echo one ACK wait per segment) and
 * errno logging on send failure (ENOBUFS pool exhaustion is the
 * interesting case on this stack).
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bwtest, LOG_LEVEL_INF);

#define BW_PORT		9900
#define BW_BUF_SIZE	4096
#define BW_THREAD_PRIO	15

static void bw_echo_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int serv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (serv < 0) {
		LOG_ERR("socket failed: %d", errno);
		return;
	}

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(BW_PORT),
	};

	if (zsock_bind(serv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    zsock_listen(serv, 1) < 0) {
		LOG_ERR("bind/listen failed: %d", errno);
		zsock_close(serv);
		return;
	}

	LOG_INF("BW test echo server on port %d", BW_PORT);

	while (true) {
		int c = zsock_accept(serv, NULL, NULL);

		if (c < 0) {
			continue;
		}
		LOG_INF("BW test client connected");

		/* Mode: first byte 'R' = receive-only sink (device RX);
		 * anything else = echo (bidirectional). */
		uint8_t mode = 0;
		ssize_t first = zsock_recv(c, &mode, 1, 0);

		if (first != 1 || mode == 'R') {
			static uint8_t sink[BW_BUF_SIZE];
			ssize_t n;
			uint32_t total = 0U;
			uint32_t t0 = k_uptime_get_32();

			while ((n = zsock_recv(c, sink, sizeof(sink), 0)) > 0) {
				total += n;
			}
			LOG_INF("BW sink done: %u bytes in %u ms (%u KB/s)",
				total, k_uptime_get_32() - t0,
				(total / 1024U) /
					MAX(1U, (k_uptime_get_32() - t0) / 1000U));
			zsock_close(c);
			continue;
		}

		static uint8_t buf[BW_BUF_SIZE];
		int nodelay = 1;

		(void)zsock_setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
				       &nodelay, sizeof(nodelay));

		ssize_t n;
		while ((n = zsock_recv(c, buf, sizeof(buf), 0)) > 0) {
			ssize_t off = 0;

			while (off < n) {
				ssize_t w = zsock_send(c, buf + off, n - off, 0);

				if (w < 0) {
					LOG_ERR("send failed: %d", errno);
					goto done;
				}
				off += w;
			}
		}
done:
		LOG_INF("BW test client done");
		zsock_close(c);
	}
}

K_THREAD_DEFINE(bw_test, 4096, bw_echo_thread, NULL, NULL, NULL,
		BW_THREAD_PRIO, 0, 0);
