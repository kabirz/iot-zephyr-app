/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebSocket 实时通道 (/ws) 接口声明
 */

#ifndef __WS_IO_H__
#define __WS_IO_H__

#include <stddef.h>
#include <zephyr/net/http/server.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 构造 IO 实时状态 JSON (WS 推送帧与 GET /api/io 共用):
 * {"di":[x16],"do":[x8],"ai":[x4],"di_en":n,"ai_en":n,"ms":uptime_ms}
 * 返回字符串长度 (不含 '\0') */
int ws_io_build_status(char *buf, size_t bufsz);

/* WebSocket 升级回调 (HTTP_RESOURCE_TYPE_WEBSOCKET 的 cb) */
int ws_io_setup(int ws_socket, struct http_request_ctx *request_ctx, void *user_data);

/* /ws 资源描述 (httpd.c 的 HTTP_RESOURCE_DEFINE 引用) */
extern struct http_resource_detail_websocket ws_io_detail;

#ifdef __cplusplus
}
#endif

#endif /* __WS_IO_H__ */
