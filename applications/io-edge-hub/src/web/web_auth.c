/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web 认证实现 — 见 web_auth.h
 */

#include <string.h>
#include <strings.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>

#include "web_json.h"
#include "web_auth.h"

LOG_MODULE_REGISTER(io_web_auth, LOG_LEVEL_INF);

/* 捕获 Authorization 头 (HTTP_SERVER_CAPTURE_HEADERS), Bearer token 校验用 */
HTTP_SERVER_REGISTER_HEADER_CAPTURE(auth_hdr, "Authorization");

#define WEB_USER_MAX	16	/* 含 '\0' */
#define WEB_PASS_MAX	16

/* token 槽位: 环形覆盖, 最多 4 个并发会话 (多标签页/多主机) */
#define TOKEN_SLOTS	4

static char web_user[WEB_USER_MAX] = "admin";
static char web_pass[WEB_PASS_MAX] = "admin";

struct token_slot {
	uint8_t bin[WEB_AUTH_TOKEN_HEX_LEN / 2];
	bool used;
};
static struct token_slot tokens[TOKEN_SLOTS];
static int token_next;

/* ==================== token 签发 / 校验 ==================== */

static void bin_to_hex(const uint8_t *bin, size_t len, char *hex)
{
	static const char d[] = "0123456789abcdef";

	for (size_t i = 0; i < len; i++) {
		hex[2 * i] = d[bin[i] >> 4];
		hex[2 * i + 1] = d[bin[i] & 0xf];
	}
	hex[2 * len] = '\0';
}

int web_auth_login(const char *user, const char *pass, char *token_out)
{
	if (user == NULL || pass == NULL ||
	    strcmp(user, web_user) != 0 || strcmp(pass, web_pass) != 0) {
		return -EACCES;
	}

	/* 覆盖最旧槽位 */
	struct token_slot *t = &tokens[token_next];

	if (sys_csrand_get(t->bin, sizeof(t->bin)) != 0) {
		LOG_ERR("token random gen failed");
		return -ENOENT;
	}
	t->used = true;
	token_next = (token_next + 1) % TOKEN_SLOTS;

	bin_to_hex(t->bin, sizeof(t->bin), token_out);
	return 0;
}

bool web_auth_token_valid(const char *token_hex)
{
	if (token_hex == NULL || strlen(token_hex) != WEB_AUTH_TOKEN_HEX_LEN) {
		return false;
	}

	uint8_t bin[WEB_AUTH_TOKEN_HEX_LEN / 2];

	if (hex2bin(token_hex, WEB_AUTH_TOKEN_HEX_LEN, bin, sizeof(bin)) !=
	    sizeof(bin)) {
		return false;
	}

	for (int i = 0; i < TOKEN_SLOTS; i++) {
		if (tokens[i].used &&
		    memcmp(tokens[i].bin, bin, sizeof(bin)) == 0) {
			return true;
		}
	}
	return false;
}

int web_auth_check_request(const struct http_client_ctx *client,
			   const struct http_request_ctx *req)
{
	char tok[WEB_AUTH_TOKEN_HEX_LEN + 1];

	/* 1. Authorization: Bearer <token> */
	if (req != NULL) {
		for (size_t i = 0; i < req->header_count; i++) {
			const char *n = req->headers[i].name;
			const char *v = req->headers[i].value;

			if (n != NULL && v != NULL &&
			    strncasecmp(n, "Authorization", 13) == 0 &&
			    strncmp(v, "Bearer ", 7) == 0 &&
			    web_auth_token_valid(v + 7)) {
				return 0;
			}
		}
	}

	/* 2. 完整 URL 的 ?token= 查询串 (POST 的 req->data 是 body,
	 *    查询串只在 client->url_buffer; 不依赖头捕获机制).
	 *    url_query_get 只认纯查询串, 先截掉路径部分 */
	if (client != NULL) {
		const char *q = strchr((const char *)client->url_buffer, '?');

		if (q != NULL &&
		    url_query_get(q, "token", tok, sizeof(tok)) &&
		    web_auth_token_valid(tok)) {
			return 0;
		}
	}

	/* 3. req->data 即查询串的请求 (GET / 下载) */
	if (req != NULL && req->data != NULL &&
	    url_query_get((const char *)req->data, "token", tok, sizeof(tok)) &&
	    web_auth_token_valid(tok)) {
		return 0;
	}

	return -EACCES;
}

/* ==================== 凭据持久化 (settings "web") ==================== */

static int web_handle_set(const char *name, size_t len,
			  settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(name, "user", &next) && !next) {
		ssize_t n = read_cb(cb_arg, &web_user, sizeof(web_user) - 1);

		if (n > 0) {
			web_user[n] = '\0';
		}
		return 0;
	}
	if (settings_name_steq(name, "pass", &next) && !next) {
		ssize_t n = read_cb(cb_arg, &web_pass, sizeof(web_pass) - 1);

		if (n > 0) {
			web_pass[n] = '\0';
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(web, "web", NULL, web_handle_set, NULL, NULL);

static bool cred_valid(const char *s)
{
	size_t n = strlen(s);

	if (n < 1 || n >= WEB_USER_MAX) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) {
			return false;
		}
	}
	return true;
}

int web_auth_set_cred(const char *user, const char *pass)
{
	if (!cred_valid(user) || !cred_valid(pass)) {
		return -EINVAL;
	}

	int rc = settings_save_one("web/user", user, strlen(user) + 1);

	if (rc != 0) {
		return rc;
	}
	rc = settings_save_one("web/pass", pass, strlen(pass) + 1);
	if (rc != 0) {
		return rc;
	}

	strcpy(web_user, user);
	strcpy(web_pass, pass);
	LOG_INF("web credentials updated");
	return 0;
}
