/*
 * Ethernet bring-up test for the GD32H759 BTB board.
 *
 * Brings up the ENET0 MAC with a static IPv4 setup and runs a TCP echo
 * server on port 4242.  From the development machine:
 *
 *   ping 192.168.12.200
 *   nc 192.168.12.200 4242      (everything sent is echoed back)
 *
 * The net shell (`net iface`, `net stats`, `net ping`) is also available.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(eth_echo, LOG_LEVEL_INF);

#define ECHO_PORT 4242

int main(void)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(ECHO_PORT),
		.sin_addr = { .s_addr = htonl(INADDR_ANY) },
	};
	int srv;
	int ret;

	/* give the USB CDC console time to settle so early logs survive */
	k_sleep(K_SECONDS(1));

	srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (srv < 0) {
		LOG_ERR("zsock_socket() failed: %d", errno);
		return -1;
	}

	ret = zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		LOG_ERR("zsock_bind() failed: %d", errno);
		return -1;
	}

	ret = zsock_listen(srv, 2);
	if (ret < 0) {
		LOG_ERR("zsock_listen() failed: %d", errno);
		return -1;
	}

	LOG_INF("TCP echo server listening on port %d", ECHO_PORT);

	while (true) {
		int client = zsock_accept(srv, NULL, 0);
		uint8_t buf[512];

		if (client < 0) {
			LOG_WRN("zsock_accept() failed: %d", errno);
			k_msleep(100);
			continue;
		}

		LOG_INF("client connected");
		while (true) {
			int n = zsock_recv(client, buf, sizeof(buf), 0);

			if (n <= 0) {
				break;
			}
			if (zsock_send(client, buf, n, 0) < 0) {
				break;
			}
		}
		LOG_INF("client disconnected");
		zsock_close(client);
	}

	return 0;
}
