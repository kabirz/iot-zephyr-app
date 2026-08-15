/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 临时网络带宽测试端点 (路线 B): TCP echo server
 *
 *  - 监听 CONFIG_IO_BW_TEST_PORT (默认 9900)
 *  - 单连接: 收多少回显多少 (echo), 用于测量协议栈原始 TCP 吞吐
 *  - Kconfig 开关 CONFIG_IO_BW_TEST, 测完即移除, 不影响正式固件
 *  - 上位机用 python: socat/pv 或自写脚本计速
 *
 * 注意: 仅用于带宽测量, 无认证, 切勿部署到正式环境。
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/netinet/in.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/sys/select.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(io_bwtest, LOG_LEVEL_INF);

#define BW_BUF_SIZE 4096

static void bw_echo_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int serv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (serv < 0) {
		LOG_ERR("socket failed: %d", errno);
		return;
	}

	int opt = 1;

	(void)setsockopt(serv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(CONFIG_IO_BW_TEST_PORT),
	};

	if (bind(serv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(serv, 1) < 0) {
		LOG_ERR("bind/listen failed: %d", errno);
		close(serv);
		return;
	}

	LOG_INF("BW test echo server on port %d", CONFIG_IO_BW_TEST_PORT);

	while (1) {
		int c = accept(serv, NULL, NULL);

		if (c < 0) {
			continue;
		}
		LOG_INF("BW test client connected");

		/* 模式: 首个字节 'R' = 只收不回 (测设备接收/下行);
		 * 其他 = echo 回显 (测双向) */
		uint8_t mode = 0;
		ssize_t first = recv(c, &mode, 1, 0);

		if (first != 1 || mode == 'R') {
			/* 只收模式: 吸流直到关闭 */
			static uint8_t sink[4096];
			ssize_t n;

			while ((n = recv(c, sink, sizeof(sink), 0)) > 0) {
				/* 丢弃 */
			}
			LOG_INF("BW sink done");
			close(c);
			continue;
		}

		/* echo 模式: 收多少回多少 */
		static uint8_t buf[4096];
		ssize_t n;

		while ((n = recv(c, buf, sizeof(buf), 0)) > 0) {
			ssize_t off = 0;

			while (off < n) {
				ssize_t w = send(c, buf + off, n - off, 0);

				if (w < 0) {
					goto done;
				}
				off += w;
			}
		}
done:
		LOG_INF("BW test client done");
		close(c);
	}
}

K_THREAD_DEFINE(bw_test, 4096, bw_echo_thread, NULL, NULL, NULL, 15, 0, 0);
