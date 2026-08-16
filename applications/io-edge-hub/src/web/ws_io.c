/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebSocket 实时通道 (/ws) — 页面数据全部走这里, HTTP 只留控制/文件操作
 *
 * 推送帧 (JSON 文本, 按 "t" 字段分发):
 *   {"t":"io",...}    IO 快照 (DI/DO/AI + 使能), 1s 周期
 *   {"t":"regs",...}  寄存器全量 (holding/input), 1s 周期
 *   {"t":"info",...}  设备详情 (版本/存储/链路), 10s 周期
 *   连接建立后立即推 io + regs + info 各一帧
 *
 * 接收命令 (与 HTTP POST 共用执行器, 同 Modbus 副作用路径):
 *   {"cmd":"do","index":n,"value":0/1}
 *   {"cmd":"reg","addr":n,"value":v}
 *   {"cmd":"time","ts":unix}
 *   {"cmd":"cfg","ip":"a.b.c.d","rs485":n,"sid":n,"can_bps":n,"can_id":n}
 *   {"cmd":"save"}	参数持久化到 FCB
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/websocket.h>
#include <zephyr/logging/log.h>

#include "web_json.h"
#include "ws_io.h"
#include "web_cmds.h"
#include <init.h>

LOG_MODULE_REGISTER(io_ws, LOG_LEVEL_INF);

#define WS_RX_BUF_SIZE	256
#define WS_TX_BUF_SIZE	640	/* info 帧最大 (~500B) */
#define WS_PUSH_MS	1000	/* io / regs 推送周期 */
#define WS_INFO_MS	10000	/* info 推送周期 */

/* ==================== IO 快照 JSON (推送帧 / GET /api/io 共用) ==================== */

int ws_io_build_status(char *buf, size_t bufsz)
{
	uint16_t di = get_input_reg(INPUT_DI_IDX);
	uint16_t do_v = get_holding_reg(HOLDING_DO_IDX);
	uint16_t di_en = get_holding_reg(HOLDING_DI_ENABLE_IDX);
	uint16_t ai_en = get_holding_reg(HOLDING_AI_ENABLE_IDX);
	int n = 0;

	n += snprintf(buf + n, bufsz - n, "{\"t\":\"io\",\"di\":[");
	for (int i = 0; i < DI_NUM; i++) {
		n += snprintf(buf + n, bufsz - n, "%s%u", i ? "," : "",
			      (di >> i) & 1);
	}
	n += snprintf(buf + n, bufsz - n, "],\"do\":[");
	for (int i = 0; i < DO_NUM; i++) {
		n += snprintf(buf + n, bufsz - n, "%s%u", i ? "," : "",
			      (do_v >> i) & 1);
	}
	n += snprintf(buf + n, bufsz - n, "],\"ai\":[");
	for (int i = 0; i < AI_NUM; i++) {
		n += snprintf(buf + n, bufsz - n, "%s%u", i ? "," : "",
			      get_input_reg(INPUT_AI0_IDX + i));
	}
	n += snprintf(buf + n, bufsz - n,
		      "],\"di_en\":%u,\"ai_en\":%u,\"ms\":%lld}",
		      di_en, ai_en, (long long)k_uptime_get());
	return n;
}

/* ==================== 连接槽位 + 处理线程 ==================== */

struct ws_slot {
	int sock;
	struct k_thread thread;
	bool in_use;
	char rx_buf[WS_RX_BUF_SIZE];
	char tx_buf[WS_TX_BUF_SIZE];
};

static K_THREAD_STACK_ARRAY_DEFINE(ws_stacks, CONFIG_IO_WEB_WS_HANDLERS,
				   CONFIG_IO_WEB_WS_STACK_SIZE);
static struct ws_slot ws_slots[CONFIG_IO_WEB_WS_HANDLERS];

static int ws_get_free_slot(void)
{
	for (int i = 0; i < CONFIG_IO_WEB_WS_HANDLERS; i++) {
		if (!ws_slots[i].in_use) {
			return i;
		}
	}
	return -1;
}

/* 处理一条客户端 JSON 命令, 回 ack (do 命令回改变后的完整快照) */
static void ws_handle_cmd(struct ws_slot *s, const char *cmd, size_t len)
{
	int32_t index = 0, addr = 0, value = 0, ts = 0;
	int n;

	if (strncmp(cmd, "\"do\"", 4) == 0 &&
	    json_get_i32(cmd, len, "index", &index) &&
	    json_get_i32(cmd, len, "value", &value)) {
		int rc = web_cmd_exec_do(index, value);

		if (rc == 0) {
			n = ws_io_build_status(s->tx_buf, sizeof(s->tx_buf));
		} else {
			n = snprintf(s->tx_buf, sizeof(s->tx_buf),
				     "{\"ok\":false,\"err\":\"bad index\"}");
		}
	} else if (strncmp(cmd, "\"reg\"", 5) == 0 &&
		   json_get_i32(cmd, len, "addr", &addr) &&
		   json_get_i32(cmd, len, "value", &value)) {
		n = snprintf(s->tx_buf, sizeof(s->tx_buf), "{\"ok\":%s}",
			     web_cmd_exec_reg(addr, value) == 0 ? "true" : "false");
	} else if (strncmp(cmd, "\"time\"", 6) == 0 &&
		   json_get_i32(cmd, len, "ts", &ts)) {
		n = snprintf(s->tx_buf, sizeof(s->tx_buf), "{\"ok\":%s}",
			     set_timestamp((time_t)ts) ? "true" : "false");
	} else if (strncmp(cmd, "\"cfg\"", 5) == 0) {
		/* 系统配置: 字段可选, 校验失败回具体原因 */
		const char *err = "invalid";

		if (web_cmd_exec_cfg(cmd, len, &err) == 0) {
			n = snprintf(s->tx_buf, sizeof(s->tx_buf), "{\"ok\":true}");
		} else {
			n = snprintf(s->tx_buf, sizeof(s->tx_buf),
				     "{\"ok\":false,\"err\":\"%s\"}", err);
		}
	} else if (strncmp(cmd, "\"save\"", 6) == 0) {
		holding_reg_save();
		n = snprintf(s->tx_buf, sizeof(s->tx_buf), "{\"ok\":true}");
	} else {
		n = snprintf(s->tx_buf, sizeof(s->tx_buf),
			     "{\"ok\":false,\"err\":\"unknown cmd\"}");
	}
	(void)websocket_send_msg(s->sock, s->tx_buf, n,
				 WEBSOCKET_OPCODE_DATA_TEXT, false, true,
				 1000);
}

static void ws_thread(void *p1, void *p2, void *p3)
{
	struct ws_slot *s = p1;
	int64_t last_push = 0;
	int64_t last_info = 0;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("ws[%p] connected", (void *)s);

	/* 避开握手竞态窗口: 101 刚发出时浏览器可能尚未完成 WS 建立,
	 * 立即推送首帧会被部分客户端当作非法帧导致 RST */
	k_msleep(300);
	last_push = k_uptime_get() - WS_PUSH_MS;	/* 立即推首帧 */
	last_info = k_uptime_get() - WS_INFO_MS;

	while (true) {
		uint32_t type = 0;
		uint64_t remaining = 0;
		int len = websocket_recv_msg(s->sock, s->rx_buf,
					     sizeof(s->rx_buf) - 1, &type,
					     &remaining, 200);

		if (len == -EAGAIN) {
			/* 超时: 到推送周期则发快照 */
		} else if (len < 0) {
			LOG_INF("ws closed (%d)", len);
			break;
		} else if (len > 0) {
			s->rx_buf[len] = '\0';
			if (type == WEBSOCKET_OPCODE_DATA_TEXT) {
				const char *cmd = json_find_value(s->rx_buf, len, "cmd");

				if (cmd != NULL) {
					ws_handle_cmd(s, cmd, len - (cmd - s->rx_buf));
				}
			}
		}

		if (k_uptime_get() - last_push >= WS_PUSH_MS) {
			/* io + regs 帧共用周期, 一次 recv 超时窗口内顺序发出 */
			int n = ws_io_build_status(s->tx_buf, sizeof(s->tx_buf));

			if (websocket_send_msg(s->sock, s->tx_buf, n,
					       WEBSOCKET_OPCODE_DATA_TEXT,
					       false, true, 500) < 0) {
				LOG_INF("ws send failed, closing");
				break;
			}
			n = web_build_regs_json(s->tx_buf, sizeof(s->tx_buf));
			if (websocket_send_msg(s->sock, s->tx_buf, n,
					       WEBSOCKET_OPCODE_DATA_TEXT,
					       false, true, 500) < 0) {
				LOG_INF("ws send failed, closing");
				break;
			}
			last_push = k_uptime_get();
		}

		if (k_uptime_get() - last_info >= WS_INFO_MS) {
			int n = web_build_info_json(s->tx_buf, sizeof(s->tx_buf));

			if (websocket_send_msg(s->sock, s->tx_buf, n,
					       WEBSOCKET_OPCODE_DATA_TEXT,
					       false, true, 500) < 0) {
				LOG_INF("ws send failed, closing");
				break;
			}
			last_info = k_uptime_get();
		}
	}

	websocket_unregister(s->sock);
	s->sock = -1;
	s->in_use = false;
	LOG_INF("ws[%p] released", (void *)s);
}

/* HTTP 服务器 WebSocket 升级回调.
 * 注意: websocket_register 的解析缓冲是资源级共享的, 并发连接会互相
 * 踩踏, 因此同一时刻只接受 1 条连接 (多余的被拒绝, 前端自动降级轮询). */
int ws_io_setup(int ws_socket, struct http_request_ctx *request_ctx,
		void *user_data)
{
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	int slot = ws_get_free_slot();

	if (slot < 0) {
		LOG_WRN("ws busy, rejecting (single connection limit)");
		return -ENOENT;
	}

	struct ws_slot *s = &ws_slots[slot];

	s->sock = ws_socket;
	s->in_use = true;

	k_thread_create(&s->thread, ws_stacks[slot],
			K_THREAD_STACK_SIZEOF(ws_stacks[slot]),
			ws_thread, s, NULL, NULL, 8, 0, K_NO_WAIT);
#ifdef CONFIG_THREAD_NAME
	char name[12];

	snprintf(name, sizeof(name), "ws_%d", slot);
	k_thread_name_set(&s->thread, name);
#endif
	return 0;
}

/* ==================== /ws 资源 (httpd.c 注册) ==================== */

static uint8_t ws_data_buffer[WS_RX_BUF_SIZE];

struct http_resource_detail_websocket ws_io_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_WEBSOCKET,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = ws_io_setup,
	.data_buffer = ws_data_buffer,
	.data_buffer_len = sizeof(ws_data_buffer),
	.user_data = NULL,
};

static int ws_slots_init(void)
{
	for (int i = 0; i < CONFIG_IO_WEB_WS_HANDLERS; i++) {
		ws_slots[i].sock = -1;
	}
	return 0;
}
SYS_INIT(ws_slots_init, APPLICATION, 60);
