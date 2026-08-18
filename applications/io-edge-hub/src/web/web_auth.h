/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web 认证 (JWT token 制):
 *   POST /api/login {"user","pass"} → 校验凭据, 签发 JWT (HMAC-SHA256)
 *   其余 API 携带 Authorization: Bearer <jwt>; 文件下载走
 *   ?token=<jwt> 查询串 (浏览器 <a> 无法自定义请求头);
 *   WebSocket 连接后通过首条消息 {"cmd":"auth","token":"<jwt>"} 认证.
 *
 * 凭据存 FCB settings ("web/user" "web/pass"), 默认 admin/admin,
 * 可经 WS "cfg" 命令 (需已认证) 修改.
 * JWT 密钥持久化到 FCB ("web/jwt_key"), 设备重启后浏览器免重登;
 * 出厂恢复 (擦 settings 分区) 后凭据与密钥全部回到初始状态.
 */

#ifndef __WEB_AUTH_H__
#define __WEB_AUTH_H__

#include <stddef.h>
#include <zephyr/net/http/server.h>

#define WEB_AUTH_TOKEN_MAX_LEN	256	/* JWT 最大长度 (不含 '\0') */

/* 校验凭据; 通过则签发 JWT (写出 WEB_AUTH_TOKEN_MAX_LEN+1 字节).
 * 返回 0 成功, -EACCES 凭据错误 */
int web_auth_login(const char *user, char *pass,
		   char *token_out /* [WEB_AUTH_TOKEN_MAX_LEN + 1] */);

/* JWT 是否有效 (签名正确且未过期) */
bool web_auth_token_valid(const char *jwt);

/* 认证检查 (按优先级):
 *   1. Authorization: Bearer <jwt> 头 (需 HTTP_SERVER_CAPTURE_HEADERS)
 *   2. client->url_buffer 完整 URL 的 ?token= 查询串 (GET/POST 通吃,
 *      不依赖头捕获机制)
 *   3. req->data 中的查询串 (GET 请求 populate 后即查询串)
 * 返回 0=通过, -EACCES=未认证 */
int web_auth_check_request(const struct http_client_ctx *client,
			   const struct http_request_ctx *req);

/* 修改凭据并持久化 (user/pass 均需 1-15 可打印字符).
 * 返回 0 成功, -EINVAL 参数非法, <0 settings 写失败 */
int web_auth_set_cred(const char *user, const char *pass);

#endif /* __WEB_AUTH_H__ */
