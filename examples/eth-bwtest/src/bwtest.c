/*
 * TCP bandwidth test endpoint, ported from eth-echo's src/bwtest.c.
 *
 *  - TCP server on port 9900
 *  - single connection; the first byte selects the mode:
 *      'R'  receive-only sink (board RX / host->board downlink capacity)
 *      any  echo (bidirectional throughput)
 *  - board logs received bytes / elapsed time / rate when the peer
 *    disconnects; host side rate is reported by host/tcp_send.py
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bwtest, LOG_LEVEL_INF);

#define BW_PORT		9900
#define BW_BUF_SIZE	4096
#define BW_THREAD_PRIO	15

static void bw_sink_session(int c)
{
	static uint8_t sink[BW_BUF_SIZE];
	ssize_t n;
	uint64_t total = 0U;
	uint32_t t0 = k_uptime_get_32();

	while ((n = zsock_recv(c, sink, sizeof(sink), 0)) > 0) {
		total += n;
	}

	uint32_t elapsed = k_uptime_get_32() - t0;

	if (elapsed > 0) {
		LOG_INF("BW sink done: %u KB in %u ms: %u KB/s (%u.%02u Mbit/s)",
			(uint32_t)(total / 1024U), elapsed,
			(uint32_t)((total / 1024U) * 1000U / elapsed),
			(uint32_t)(total * 8U / elapsed / 1000000U),
			(uint32_t)((total * 8U / elapsed / 1000U) % 1000U));
	} else {
		LOG_INF("BW sink done: %u KB (too fast to measure)", (uint32_t)(total / 1024U));
	}
}

static void bw_echo_session(int c)
{
	static uint8_t buf[BW_BUF_SIZE];
	int nodelay = 1;
	ssize_t n;

	/* Zephyr TCP 栈不开 NODELAY 时, 回环每段都要等一次 ACK */
	(void)zsock_setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
			       &nodelay, sizeof(nodelay));

	while ((n = zsock_recv(c, buf, sizeof(buf), 0)) > 0) {
		ssize_t off = 0;

		while (off < n) {
			ssize_t w = zsock_send(c, buf + off, n - off, 0);

			if (w < 0) {
				LOG_ERR("send failed: %d", errno);
				return;
			}
			off += w;
		}
	}
}

static void bw_thread(void *p1, void *p2, void *p3)
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

	LOG_INF("BW test TCP server on port %d ('R' = sink, else echo)", BW_PORT);

	while (true) {
		int c = zsock_accept(serv, NULL, NULL);

		if (c < 0) {
			continue;
		}
		LOG_INF("BW test client connected");

		/* 首字节选模式: 'R' = 纯收流, 其他 = 回环 */
		uint8_t mode = 0;

		if (zsock_recv(c, &mode, 1, 0) == 1 && mode != 'R') {
			bw_echo_session(c);
		} else {
			bw_sink_session(c);
		}

		LOG_INF("BW test client done");
		zsock_close(c);
	}
}

K_THREAD_DEFINE(bw_test, 4096, bw_thread, NULL, NULL, NULL,
		BW_THREAD_PRIO, 0, 0);
