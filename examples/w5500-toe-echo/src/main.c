/*
 * W5500 hardware TCP/IP offload (TOE) echo example.
 *
 * The chip's on-chip stack does all the protocol work; this app only opens
 * sockets (plain POSIX API). Static IPv4 comes from the devicetree node
 * (local-ip / netmask / gateway in boards/io_edge_f407vet6.overlay). From
 * the development machine:
 *
 *   ping 192.168.12.200          (ICMP handled inside the W5500)
 *   nc 192.168.12.200 4242       (TCP echo, one client at a time)
 *
 * Connection teardown is RST-based (Sn_CR=CLOSE): reconnecting right after a
 * dropped session always works — no TIME_WAIT, on purpose.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/unistd.h>
#include <arpa/inet.h>
#include <errno.h>

LOG_MODULE_REGISTER(w5500_toe_echo, LOG_LEVEL_INF);

#define ECHO_PORT 4242

int main(void)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(ECHO_PORT),
		.sin_addr = { .s_addr = htonl(INADDR_ANY) },
	};
	uint8_t buf[2048]; /* match the W5500 2 KiB socket buffers */
	int srv;
	int ret;

	k_sleep(K_SECONDS(1));

	srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (srv < 0) {
		LOG_ERR("socket() failed: %d", errno);
		return -1;
	}

	ret = bind(srv, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		LOG_ERR("bind() failed: %d", errno);
		return -1;
	}

	ret = listen(srv, 1);
	if (ret < 0) {
		LOG_ERR("listen() failed: %d", errno);
		return -1;
	}

	LOG_INF("TCP echo server on port %d (W5500 TOE)", ECHO_PORT);

	while (true) {
		struct sockaddr_in peer = { 0 };
		socklen_t peer_len = sizeof(peer);
		int conn = accept(srv, (struct sockaddr *)&peer, &peer_len);
		char ip[16];
		int n;

		if (conn < 0) {
			LOG_WRN("accept() failed: %d", errno);
			k_sleep(K_MSEC(100));
			continue;
		}

		inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
		LOG_DBG("connected: %s:%u", ip, ntohs(peer.sin_port));

		while ((n = recv(conn, buf, sizeof(buf), 0)) > 0) {
			int off = 0;

			while (off < n) {
				int m = send(conn, buf + off, n - off, 0);

				if (m < 0) {
					off = -1;
					break;
				}
				off += m;
			}
			if (off < 0) {
				break;
			}
		}

		LOG_DBG("peer gone (recv=%d errno=%d), closing", n, n < 0 ? errno : 0);
		close(conn); /* hard close: instantly re-accept the next client */
	}

	return 0;
}
