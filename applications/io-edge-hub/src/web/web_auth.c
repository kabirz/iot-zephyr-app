/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web 认证实现 (JWT HMAC-SHA256) — 见 web_auth.h
 *
 * JWT 密钥持久化到 FCB ("web/jwt_key"): 设备重启后浏览器免重登;
 * 凭据变更时密钥不变, 旧 JWT 仍有效直到过期 (24小时);
 * 出厂恢复 (擦 settings 分区) 后密钥与凭据全部回到初始状态.
 */

#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>

#include "web_json.h"
#include "web_auth.h"

LOG_MODULE_REGISTER(io_web_auth, LOG_LEVEL_INF);

/* 捕获 Authorization 头 (HTTP_SERVER_CAPTURE_HEADERS), Bearer token 校验用 */
HTTP_SERVER_REGISTER_HEADER_CAPTURE(auth_hdr, "Authorization");

#define WEB_USER_MAX	16	/* 含 '\0' */
#define WEB_PASS_MAX	16

#define JWT_SECRET_LEN	16	/* HMAC-SHA256 密钥长度 (128 bit) */
#define JWT_EXPIRY_SECS	(24 * 60 * 60)	/* JWT 有效期 24 小时 */

static char web_user[WEB_USER_MAX] = "admin";
static char web_pass[WEB_PASS_MAX] = "admin";
static uint8_t jwt_secret[JWT_SECRET_LEN];
static bool jwt_secret_ready;

/* ==================== base64url 编解码 ==================== */

static int base64url_encode(const void *data, size_t len,
			    char *out, size_t out_sz)
{
	size_t olen = 0;
	int rc = mbedtls_base64_encode((unsigned char *)out, out_sz, &olen,
				       data, len);

	if (rc != 0) {
		return -ENOMEM;
	}
	/* 转换为 URL-safe: '+' → '-', '/' → '_', 去掉 '=' */
	for (size_t i = 0; i < olen; i++) {
		if (out[i] == '+') {
			out[i] = '-';
		} else if (out[i] == '/') {
			out[i] = '_';
		} else if (out[i] == '=') {
			out[i] = '\0';
			olen = i;
			break;
		}
	}
	return (int)olen;
}

static int base64url_decode(const char *in, size_t in_len,
			    void *out, size_t out_sz)
{
	/* 还原标准 base64: '-' → '+', '_' → '/' */
	char tmp[256];
	size_t pad;

	if (in_len >= sizeof(tmp)) {
		return -ENOMEM;
	}
	memcpy(tmp, in, in_len);
	for (size_t i = 0; i < in_len; i++) {
		if (tmp[i] == '-') {
			tmp[i] = '+';
		} else if (tmp[i] == '_') {
			tmp[i] = '/';
		}
	}
	/* 补 '=' padding */
	pad = (4 - (in_len % 4)) % 4;
	for (size_t i = 0; i < pad; i++) {
		tmp[in_len + i] = '=';
	}
	tmp[in_len + pad] = '\0';

	size_t olen = 0;

	if (mbedtls_base64_decode((unsigned char *)out, out_sz, &olen,
				  (const unsigned char *)tmp, in_len + pad) != 0) {
		return -EINVAL;
	}
	return (int)olen;
}

/* ==================== HMAC-SHA256 ==================== */

static int hmac_sha256(const uint8_t *key, size_t key_len,
		       const uint8_t *data, size_t data_len,
		       uint8_t out[32])
{
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

	if (md == NULL) {
		return -ENOENT;
	}
	return mbedtls_md_hmac(md, key, key_len, data, data_len, out);
}

/* ==================== JWT 生成 / 验证 ==================== */

static int jwt_create(const char *sub, char *buf, size_t buf_sz)
{
	/* Header */
	static const char hdr[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
	char payload[128];
	int64_t now_s = k_uptime_get() / 1000;
	int n;

	n = snprintf(payload, sizeof(payload),
		      "{\"sub\":\"%s\",\"iat\":%lld,\"exp\":%lld}",
		      sub, (long long)now_s, (long long)(now_s + JWT_EXPIRY_SECS));
	if (n < 0 || n >= (int)sizeof(payload)) {
		return -ENOMEM;
	}

	/* 编码 header + payload */
	char b64_hdr[64], b64_payload[128];
	int hlen, plen;

	hlen = base64url_encode(hdr, sizeof(hdr) - 1, b64_hdr, sizeof(b64_hdr));
	plen = base64url_encode(payload, n, b64_payload, sizeof(b64_payload));
	if (hlen < 0 || plen < 0) {
		return -ENOMEM;
	}

	/* 签名: HMAC-SHA256(b64_hdr.b64_payload, secret) */
	char sig_input[256];
	int sig_input_len;

	memcpy(sig_input, b64_hdr, hlen);
	memcpy(sig_input + hlen, b64_payload, plen);
	sig_input_len = hlen + plen;

	uint8_t sig[32];
	int rc = hmac_sha256(jwt_secret, JWT_SECRET_LEN,
			    (const uint8_t *)sig_input, sig_input_len, sig);

	if (rc != 0) {
		return rc;
	}

	char b64_sig[64];
	int slen;

	slen = base64url_encode(sig, sizeof(sig), b64_sig, sizeof(b64_sig));
	if (slen < 0) {
		return -ENOMEM;
	}

	/* 组装 JWT: header.payload.signature */
	n = snprintf(buf, buf_sz, "%.*s.%.*s.%.*s",
		     hlen, b64_hdr, plen, b64_payload, slen, b64_sig);
	if (n < 0 || n >= (int)buf_sz) {
		return -ENOMEM;
	}
	return 0;
}

static int jwt_verify(const char *jwt, char *sub_out, size_t sub_out_sz)
{
	const char *p1, *p2;
	size_t l1, l2, l3;

	/* 分割三段: header.payload.signature */
	p1 = jwt;
	p2 = strchr(p1, '.');
	if (p2 == NULL) {
		return -EINVAL;
	}
	l1 = p2 - p1;
	p2++;

	const char *p3 = strchr(p2, '.');

	if (p3 == NULL) {
		return -EINVAL;
	}
	l2 = p3 - p2;
	p3++;
	l3 = strlen(p3);

	if (l1 == 0 || l2 == 0 || l3 == 0) {
		return -EINVAL;
	}

	/* 解码 header, 检查 alg */
	char hdr[64];
	int hlen = base64url_decode(p1, l1, hdr, sizeof(hdr) - 1);

	if (hlen < 0) {
		return -EINVAL;
	}
	hdr[hlen] = '\0';
	if (strstr(hdr, "\"alg\":\"HS256\"") == NULL) {
		return -EINVAL;
	}

	/* 重算签名并比较 */
	uint8_t sig_input[256];
	int sig_input_len;

	if (l1 + l2 > sizeof(sig_input)) {
		return -ENOMEM;
	}
	memcpy(sig_input, p1, l1 + l2);
	sig_input_len = l1 + l2;

	uint8_t expected_sig[32];
	int rc = hmac_sha256(jwt_secret, JWT_SECRET_LEN,
			    sig_input, sig_input_len, expected_sig);

	if (rc != 0) {
		return rc;
	}

	char b64_expected[64];
	int b64_len;

	b64_len = base64url_encode(expected_sig, sizeof(expected_sig),
				   b64_expected, sizeof(b64_expected));
	if (b64_len < 0 || (size_t)b64_len != l3 ||
	    memcmp(p3, b64_expected, l3) != 0) {
		return -EACCES;
	}

	/* 验证过期时间 */
	char payload[256];
	int plen = base64url_decode(p2, l2, payload, sizeof(payload) - 1);

	if (plen < 0) {
		return -EINVAL;
	}
	payload[plen] = '\0';

	int64_t now_s = k_uptime_get() / 1000;
	const char *exp_str = strstr(payload, "\"exp\":");

	if (exp_str == NULL) {
		return -EINVAL;
	}
	int64_t exp_val = strtoll(exp_str + 6, NULL, 10);

	if (now_s > exp_val) {
		return -EACCES;	/* 过期 */
	}

	/* 提取 sub (用户名) */
	if (sub_out != NULL && sub_out_sz > 0) {
		const char *sub_str = strstr(payload, "\"sub\":\"");

		if (sub_str != NULL) {
			sub_str += 7;
			const char *sub_end = strchr(sub_str, '"');

			if (sub_end != NULL) {
				size_t slen = sub_end - sub_str;

				if (slen >= sub_out_sz) {
					slen = sub_out_sz - 1;
				}
				memcpy(sub_out, sub_str, slen);
				sub_out[slen] = '\0';
			} else {
				sub_out[0] = '\0';
			}
		} else {
			sub_out[0] = '\0';
		}
	}
	return 0;
}

/* ==================== 凭据 + 密钥持久化 (settings "web") ==================== */

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
	if (settings_name_steq(name, "jwt_key", &next) && !next) {
		/* 持久化 JWT 密钥: 开机恢复, 设备重启不掉线 */
		uint8_t key[JWT_SECRET_LEN];
		ssize_t n = read_cb(cb_arg, key, sizeof(key));

		if (n == sizeof(key)) {
			memcpy(jwt_secret, key, sizeof(key));
			jwt_secret_ready = true;
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(web, "web", NULL, web_handle_set, NULL, NULL);

static int jwt_secret_init(void)
{
	int rc;

	/* 如果 settings 已加载密钥, 直接返回 */
	if (jwt_secret_ready) {
		return 0;
	}

	/* 首次启动: 生成随机密钥并持久化 */
	rc = sys_csrand_get(jwt_secret, sizeof(jwt_secret));
	if (rc != 0) {
		LOG_ERR("jwt secret gen failed: %d", rc);
		return rc;
	}
	rc = settings_save_one("web/jwt_key", jwt_secret, sizeof(jwt_secret));
	if (rc != 0) {
		LOG_ERR("jwt secret save failed: %d", rc);
		return rc;
	}
	jwt_secret_ready = true;
	LOG_INF("jwt secret generated and saved");
	return 0;
}

/* ==================== 公开接口 ==================== */

int web_auth_login(const char *user, char *pass, char *token_out)
{
	int rc;

	if (user == NULL || pass == NULL ||
	    strcmp(user, web_user) != 0 || strcmp(pass, web_pass) != 0) {
		return -EACCES;
	}

	rc = jwt_secret_init();
	if (rc != 0) {
		return rc;
	}

	return jwt_create(user, token_out, WEB_AUTH_TOKEN_MAX_LEN + 1);
}

bool web_auth_token_valid(const char *jwt)
{
	if (jwt == NULL || strlen(jwt) < 20) {
		return false;
	}
	return jwt_verify(jwt, NULL, 0) == 0;
}

int web_auth_check_request(const struct http_client_ctx *client,
			   const struct http_request_ctx *req)
{
	char tok[WEB_AUTH_TOKEN_MAX_LEN + 1];

	/* 1. Authorization: Bearer <jwt> */
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
	/* JWT 不需要主动失效: 密钥不变, 旧 JWT 有效直到过期 (24小时).
	 * 如需立即失效, 可重新生成密钥 (代价是所有会话断开). */
	LOG_INF("web credentials updated");
	return 0;
}
