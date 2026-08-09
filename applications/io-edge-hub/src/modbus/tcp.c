/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus TCP Server (RAW ADU 模式 + 自定义 select() 多路复用)
 *
 *   - 使用 Zephyr modbus RAW ADU iface "RAW_0", user_cb = io_modbus_cbs
 *     (function.c 定义的 holding/input/coil 回调)
 *   - TCP socket 端口 502, select() 多路复用, 最多 3 客户端, 30s 会话超时
 *   - 网络链路断开 (net_link_is_up=false) 时拒绝新连接
 *   - 每收到请求调 heart_event_send() 重置心跳看门狗
 *
 * 流程 (参考 samples/subsys/modbus/tcp_server):
 *   recv 8B (MBAP+FC) → modbus_raw_get_header → recv data →
 *   modbus_raw_submit_rx → (server 处理, raw_tx_cb 回填响应) →
 *   k_sem_take → modbus_raw_put_header + send 响应
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/netinet/in.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/sys/select.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/logging/log.h>
#include <init.h>

LOG_MODULE_REGISTER(io_tcp, LOG_LEVEL_INF);

#define MODBUS_TCP_PORT		502
#define MB_TCP_MAX_CLIENTS	3
#define MB_TCP_SESSION_TIMEOUT	30000	/* ms */

extern const struct modbus_user_callbacks io_modbus_cbs;

static struct modbus_adu tmp_adu;
static K_SEM_DEFINE(received, 0, 1);
static int server_iface = -1;

/* raw_tx_cb: server 处理完 ADU 后回填响应, 唤醒主处理流程 */
static int server_raw_cb(const int iface, const struct modbus_adu *adu,
			 void *user_data)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(user_data);

	tmp_adu = *adu;
	k_sem_give(&received);
	return 0;
}

static int init_modbus_server(void)
{
	server_iface = modbus_iface_get_by_name("RAW_0");
	if (server_iface < 0) {
		LOG_ERR("RAW_0 iface not found");
		return -ENODEV;
	}

	struct modbus_iface_param param = {
		.mode = MODBUS_MODE_RAW,
		.server = {
			.user_cb = (struct modbus_user_callbacks *)&io_modbus_cbs,
			.unit_id = (uint8_t)get_holding_reg(HOLDING_SLAVE_ID_IDX),
		},
		.rawcb = { .raw_tx_cb = server_raw_cb, .user_data = NULL },
	};

	int rc = modbus_init_server(server_iface, param);

	if (rc) {
		LOG_ERR("modbus_init_server failed: %d", rc);
	}
	return rc;
}

static int reply_adu(int client, const struct modbus_adu *adu)
{
	uint8_t header[MODBUS_MBAP_AND_FC_LENGTH];

	modbus_raw_put_header(adu, header);
	if (send(client, header, sizeof(header), 0) < 0) {
		return -errno;
	}
	if (adu->length > 0 && send(client, adu->data, adu->length, 0) < 0) {
		return -errno;
	}
	return 0;
}

/* 处理一条客户端请求: 返回 0 继续, <0 关闭连接 */
static int handle_client(int client)
{
	uint8_t header[MODBUS_MBAP_AND_FC_LENGTH];
	int rc = recv(client, header, sizeof(header), MSG_WAITALL);

	if (rc <= 0) {
		return rc == 0 ? -ENOTCONN : -EIO;
	}

	modbus_raw_get_header(&tmp_adu, header);

	if (tmp_adu.length > 0) {
		rc = recv(client, tmp_adu.data, tmp_adu.length, MSG_WAITALL);
		if (rc <= 0) {
			return rc == 0 ? -ENOTCONN : -EIO;
		}
	}

	k_sem_reset(&received);
	if (modbus_raw_submit_rx(server_iface, &tmp_adu)) {
		LOG_ERR("submit raw ADU failed");
		return -EIO;
	}

	if (k_sem_take(&received, K_MSEC(1000)) != 0) {
		LOG_ERR("MODBUS RAW wait timeout");
		modbus_raw_set_server_failure(&tmp_adu);
	}

	return reply_adu(client, &tmp_adu);
}

static void mb_tcp_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (init_modbus_server() != 0) {
		return;
	}

	int serv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (serv < 0) {
		LOG_ERR("socket failed: %d", errno);
		return;
	}

	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(MODBUS_TCP_PORT),
	};

	int opt = 1;

	(void)setsockopt(serv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	if (bind(serv, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		LOG_ERR("bind failed: %d", errno);
		return;
	}
	if (listen(serv, MB_TCP_MAX_CLIENTS) < 0) {
		LOG_ERR("listen failed: %d", errno);
		return;
	}

	LOG_INF("Modbus TCP server on port %d", MODBUS_TCP_PORT);

	int clients[MB_TCP_MAX_CLIENTS];
	int64_t last_act[MB_TCP_MAX_CLIENTS];

	for (int i = 0; i < MB_TCP_MAX_CLIENTS; i++) {
		clients[i] = -1;
		last_act[i] = 0;
	}

	while (1) {
		fd_set rfds;
		int maxfd = serv;

		FD_ZERO(&rfds);
		FD_SET(serv, &rfds);

		for (int i = 0; i < MB_TCP_MAX_CLIENTS; i++) {
			if (clients[i] >= 0) {
				FD_SET(clients[i], &rfds);
				if (clients[i] > maxfd) {
					maxfd = clients[i];
				}
			}
		}

		struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
		int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);

		if (n > 0) {
			/* 新连接 (链路断开时拒绝) */
			if (FD_ISSET(serv, &rfds) && net_link_is_up()) {
				int c = accept(serv, NULL, NULL);

				if (c >= 0) {
					for (int i = 0; i < MB_TCP_MAX_CLIENTS; i++) {
						if (clients[i] < 0) {
							clients[i] = c;
							last_act[i] = k_uptime_get();
							LOG_INF("client %d connected", i);
							break;
						}
					}
					/* 超出上限: 直接拒绝 */
					bool found = false;

					for (int i = 0; i < MB_TCP_MAX_CLIENTS; i++) {
						if (clients[i] == c) {
							found = true;
							break;
						}
					}
					if (!found) {
						close(c);
					}
				}
			}

			/* 就绪客户端 */
			for (int i = 0; i < MB_TCP_MAX_CLIENTS; i++) {
				if (clients[i] >= 0 && FD_ISSET(clients[i], &rfds)) {
					int rc = handle_client(clients[i]);

					if (rc == 0) {
						last_act[i] = k_uptime_get();
						heart_event_send();
					} else {
						LOG_INF("client %d closed", i);
						close(clients[i]);
						clients[i] = -1;
					}
				}
			}
		}

		/* 会话超时检查 */
		int64_t now = k_uptime_get();

		for (int i = 0; i < MB_TCP_MAX_CLIENTS; i++) {
			if (clients[i] >= 0 &&
			    (now - last_act[i]) > MB_TCP_SESSION_TIMEOUT) {
				LOG_INF("client %d timeout", i);
				close(clients[i]);
				clients[i] = -1;
			}
		}
	}
}

K_THREAD_DEFINE(mb_tcp, CONFIG_IO_MODBUS_TCP_STACK, mb_tcp_thread,
		NULL, NULL, NULL, CONFIG_IO_MODBUS_TCP_PRIORITY, 0, 0);
