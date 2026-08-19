/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus TCP Server (RAW ADU 模式 + 自定义 select() 多路复用)
 *
 *   - 使用 Zephyr modbus RAW ADU iface "RAW_0", user_cb = io_modbus_cbs
 *     (function.c 定义的 holding/input/coil 回调)
 *   - TCP socket 端口 502, select() 多路复用, 无客户端数量限制
 *   - 会话超时 (IO_MODBUS_TCP_SESSION_TIMEOUT, 默认关闭)
 *   - 网络链路断开 (net_link_is_up=false) 时拒绝新连接
 *   - TCP Keepalive (SO_KEEPALIVE) 检测主站连接存活
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

#define MODBUS_TCP_PORT        502
#define MB_TCP_MAX_CLIENTS     16 /* listen backlog + 初始客户端槽位数 */
/* 会话超时须 > TCP Keepalive 总探测时间 (KEEPIDLE+KEEPCNT*KEEPINTVL = 30+3*5 = 45s),
 * 否则正常空闲主站会被应用层误踢而 keepalive 尚未生效。
 * 由 CONFIG_IO_MODBUS_TCP_SESSION_TIMEOUT 使能 (默认 n, 不踢空闲连接)。 */
#define MB_TCP_SESSION_TIMEOUT 60000 /* ms */
/* recv/send 超时压到亚秒级, 避免单慢/恶意客户端长时间阻塞 select 主循环
 * (本线程为单线程 select 多路复用, 阻塞会拖死全部客户端)。 */
#define MB_TCP_IO_TIMEOUT      500 /* ms: 客户端 recv/send 超时 */
#define MB_TCP_RESP_TIMEOUT    800 /* ms: 等待 modbus server 处理超时 */

extern const struct modbus_user_callbacks io_modbus_cbs;

static int server_iface = -1;
/* modbus server 的 unit_id 在启动时固定, 与 handle_client 校验保持一致 */
static uint8_t srv_unit_id;

/*
 * modbus server 处理在系统工作队列线程(异步, modbus_raw_submit_rx 内部
 * k_work_submit, 库仅保留一份 rx/tx ADU), 应用必须严格串行处理请求。
 * 响应经 raw_tx_cb 回填到 g_resp, mb_tcp 线程用 trans_id 匹配消费,
 * 避免多客户端请求的响应交叉污染。
 */
static struct modbus_adu g_resp;
static K_SEM_DEFINE(g_resp_sem, 0, 1);

/* raw_tx_cb: server(系统工作队列线程)处理完 ADU 后回填响应 */
static int server_raw_cb(const int iface, const struct modbus_adu *adu, void *user_data)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(user_data);

	g_resp = *adu;
	k_sem_give(&g_resp_sem);
	return 0;
}

static int init_modbus_server(void)
{
	server_iface = modbus_iface_get_by_name("RAW_0");
	if (server_iface < 0) {
		LOG_ERR("RAW_0 iface not found");
		return -ENODEV;
	}

	/* 缓存 unit_id: server 初始化后固定, 校验与 server 内部判定保持一致 */
	srv_unit_id = (uint8_t)get_holding_reg(HOLDING_SLAVE_ID_IDX);

	struct modbus_iface_param param = {
		.mode = MODBUS_MODE_RAW,
		.server =
			{
				.user_cb = (struct modbus_user_callbacks *)&io_modbus_cbs,
				.unit_id = srv_unit_id,
			},
		.rawcb = {.raw_tx_cb = server_raw_cb, .user_data = NULL},
	};

	int rc = modbus_init_server(server_iface, param);

	if (rc) {
		LOG_ERR("modbus_init_server failed: %d", rc);
	}
	return rc;
}

static int reply_adu(int client, const struct modbus_adu *adu)
{
	/* 头 (MBAP+unit+fc) 与数据合并为一次 send:
	 * 分开 send 会拆成两个 TCP 段, 部分上位机按"一段=一帧"解析会失败 */
	uint8_t buf[MODBUS_MBAP_AND_FC_LENGTH + CONFIG_MODBUS_BUFFER_SIZE];

	modbus_raw_put_header(adu, buf);
	memcpy(buf + MODBUS_MBAP_AND_FC_LENGTH, adu->data, adu->length);
	if (send(client, buf, MODBUS_MBAP_AND_FC_LENGTH + adu->length, 0) < 0) {
		return -errno;
	}
	return 0;
}

/* 循环读满指定字节数。MSG_WAITALL 在 SO_RCVTIMEO 下可能因超时返回短读,
 * 短读被当作成功会让后续解析读到未初始化字节 → 帧同步丢失。这里自己做循环,
 * 任何 recv <=0 都按断连/错误处理, 保证读到的字节数恰好 == want。
 * 返回 0 成功, <0 失败 (rc==0 → -ENOTCONN, 否则 -EIO)。 */
static int recv_full(int sock, uint8_t *buf, size_t want)
{
	size_t total = 0;

	while (total < want) {
		int rc = recv(sock, buf + total, want - total, 0);

		if (rc <= 0) {
			return rc == 0 ? -ENOTCONN : -EIO;
		}
		total += rc;
	}
	return 0;
}

/* 处理一条客户端请求: 返回 0 继续, <0 关闭连接 */
static int handle_client(int client)
{
	uint8_t header[MODBUS_MBAP_AND_FC_LENGTH];
	struct modbus_adu req;
	struct modbus_adu resp;
	int rc = recv_full(client, header, sizeof(header));

	if (rc < 0) {
		return rc;
	}

	modbus_raw_get_header(&req, header);

	/* 帧校验: 协议 ID 必须为 0, length 不得超过缓冲区 (防越界写) */
	if (req.proto_id != 0 || req.length > sizeof(req.data)) {
		LOG_WRN("bad MBAP frame (proto=%u len=%u)", req.proto_id, req.length);
		resp = req;
		modbus_raw_set_server_failure(&resp);
		return reply_adu(client, &resp);
	}

	if (req.length > 0) {
		rc = recv_full(client, req.data, req.length);
		if (rc < 0) {
			return rc;
		}
	}

	/* Modbus TCP 直连场景 unit_id 无寻址意义 (仅网关桥接串行总线时使用),
	 * 规范允许主站填 0x00/0xFF 等任意值, 服务器应正常响应。
	 * 因此不校验 unit_id, 而是改写为 server 的 unit_id 后提交, 绕过
	 * Zephyr server 内部的严格匹配 (modbus_server.c 对不匹配帧丢帧不回复)。
	 * 保留广播 (unit_id=0) 语义: 执行副作用但不回复。 */
	uint8_t orig_unit_id = req.unit_id;

	if (orig_unit_id != 0) {
		req.unit_id = srv_unit_id;
	}

	k_sem_reset(&g_resp_sem);
	if (modbus_raw_submit_rx(server_iface, &req)) {
		LOG_ERR("submit raw ADU failed");
		return -EIO;
	}

	/* 广播 (unit_id=0): Modbus 协议规定不回复任何响应。库执行完副作用
	 * (FC05/06/15/16 写 DO/参数) 后不会回调 raw_tx_cb, 故不等待响应,
	 * 直接返回保持连接, 避免等满超时后误回 SERVER_DEVICE_FAILURE 致主站重发。 */
	if (orig_unit_id == 0) {
		return 0;
	}

	/*
	 * 等待 server 异步处理完成。库回显请求 trans_id, 借此丢弃可能
	 * 残留的上一个请求响应, 保证回给客户端的一定是本次请求的响应。
	 */
	while (k_sem_take(&g_resp_sem, K_MSEC(MB_TCP_RESP_TIMEOUT)) == 0 &&
	       g_resp.trans_id != req.trans_id) {
		/* 残留的旧响应, 继续等待本次响应 */
	}

	if (g_resp.trans_id != req.trans_id) {
		LOG_ERR("MODBUS RAW wait timeout");
		resp = req;
		modbus_raw_set_server_failure(&resp);
		resp.unit_id = orig_unit_id; /* 回显客户端原始 unit_id */
		return reply_adu(client, &resp);
	}

	g_resp.unit_id = orig_unit_id; /* 回显客户端原始 unit_id, 不暴露内部改写 */
	return reply_adu(client, &g_resp);
}

/* 客户端节点 (动态链表) */
struct mb_client {
	int fd;
	int64_t last_act;
	struct mb_client *next;
};

static struct mb_client *client_list;
static int client_count;

static void client_remove(int fd)
{
	struct mb_client **pp = &client_list;

	while (*pp) {
		if ((*pp)->fd == fd) {
			struct mb_client *del = *pp;

			*pp = del->next;
			close(del->fd);
			k_free(del);
			client_count--;
			return;
		}
		pp = &(*pp)->next;
	}
}

static struct mb_client *client_add(int fd)
{
	struct mb_client *c = k_malloc(sizeof(*c));

	if (!c) {
		close(fd);
		return NULL;
	}
	c->fd = fd;
	c->last_act = k_uptime_get();
	c->next = client_list;
	client_list = c;
	client_count++;
	return c;
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
		close(serv);
		return;
	}
	if (listen(serv, MB_TCP_MAX_CLIENTS) < 0) {
		LOG_ERR("listen failed: %d", errno);
		close(serv);
		return;
	}

	LOG_INF("Modbus TCP server on port %d", MODBUS_TCP_PORT);

	while (1) {
		fd_set rfds;
		int maxfd = serv;

		FD_ZERO(&rfds);
		FD_SET(serv, &rfds);

		for (struct mb_client *c = client_list; c; c = c->next) {
			FD_SET(c->fd, &rfds);
			if (c->fd > maxfd) {
				maxfd = c->fd;
			}
		}

		struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
		int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);

		if (n > 0) {
			/* 新连接 (链路断开时拒绝) */
			if (FD_ISSET(serv, &rfds) && net_link_is_up()) {
				int c = accept(serv, NULL, NULL);

				if (c >= 0) {
					/* recv/send 超时兜底: 防止恶意/慢客户端
					 * 声明大长度却不发数据而挂死服务线程 */
					struct timeval tv = {
						.tv_sec = MB_TCP_IO_TIMEOUT / 1000,
						.tv_usec = (MB_TCP_IO_TIMEOUT % 1000) * 1000,
					};

					(void)setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv,
							 sizeof(tv));
					(void)setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv,
							 sizeof(tv));
					/* TCP Keepalive: 主站异常掉线时协议栈自动断开连接
					 * (探测参数由 CONFIG_NET_TCP_KEEPIDLE/INTVL/CNT 决定) */
					int ka = 1;

					(void)setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &ka,
							 sizeof(ka));

					if (client_add(c)) {
						LOG_INF("client connected (fd=%d, total=%d)", c,
							client_count);
					}
				}
			}

			/* 就绪客户端 */
			for (struct mb_client *c = client_list, *next; c; c = next) {
				next = c->next;
				if (FD_ISSET(c->fd, &rfds)) {
					int rc = handle_client(c->fd);

					if (rc == 0) {
						c->last_act = k_uptime_get();
					} else {
						LOG_INF("client disconnected (fd=%d)", c->fd);
						client_remove(c->fd);
					}
				}
			}
		}

		/* 会话超时检查 (可选, 默认关闭: 空闲主站连接由 TCP Keepalive 负责检测) */
#ifdef CONFIG_IO_MODBUS_TCP_SESSION_TIMEOUT
		int64_t now = k_uptime_get();

		for (struct mb_client *c = client_list, *next; c; c = next) {
			next = c->next;
			if ((now - c->last_act) > MB_TCP_SESSION_TIMEOUT) {
				LOG_INF("client timeout (fd=%d)", c->fd);
				client_remove(c->fd);
			}
		}
#endif
	}
}

K_THREAD_DEFINE(mb_tcp, CONFIG_IO_MODBUS_TCP_STACK, mb_tcp_thread, NULL, NULL, NULL,
		CONFIG_IO_MODBUS_TCP_PRIORITY, 0, 0);
