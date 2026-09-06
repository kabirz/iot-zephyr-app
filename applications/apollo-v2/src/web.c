/* SPDX-License-Identifier: Apache-2.0
 * Web 服务 (端口 80): 信息面板 + 配置
 *   GET  /          内嵌中文页面 (2s 轮询 /api/info)
 *   GET  /api/info  设备信息 JSON
 *   POST /api/ctl   控制 {"led0":1,"led1":0,"beep":1,"bl":0} (字段可选)
 *   POST /api/time  设时间 {"time":"2026-09-05 21:30:00"} (本地 UTC+8)
 *   POST /api/reboot 热重启
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/logging/log.h>

#include "version.h"
#include "board_io.h"
#include "app_time.h"
#include "storage.h"
#include "net.h"
#include "display.h"
#include "font_qspi.h"
#ifdef CONFIG_APOLLO_CAN
#include "can.h"
#endif

LOG_MODULE_REGISTER(apollo_web, LOG_LEVEL_INF);

#define WEB_PORT 80

/* ==================== 首页 (内嵌 HTML) ==================== */

static const uint8_t index_html[] = {
#include "web_index_html.inc"
};

static struct http_resource_detail_static index_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "text/html",
	},
	.static_data = index_html,
	.static_data_len = sizeof(index_html) - 1,
};

/* ==================== JSON 小工具 ==================== */

static bool json_get_int(const char *js, size_t len, const char *key, int32_t *out)
{
	char pat[24];
	int n = snprintf(pat, sizeof(pat), "\"%s\":", key);

	if (n <= 0 || (size_t)n >= sizeof(pat)) {
		return false;
	}
	for (size_t i = 0; i + (size_t)n < len; i++) {
		if (memcmp(js + i, pat, n) == 0) {
			*out = (int32_t)strtol(js + i + n, NULL, 10);
			return true;
		}
	}
	return false;
}

static bool json_get_str(const char *js, size_t len, const char *key,
			 char *out, size_t outsz)
{
	char pat[24];
	int n = snprintf(pat, sizeof(pat), "\"%s\":\"", key);

	if (n <= 0 || (size_t)n >= sizeof(pat)) {
		return false;
	}
	for (size_t i = 0; i + (size_t)n < len; i++) {
		if (memcmp(js + i, pat, n) == 0) {
			size_t o = 0;

			for (size_t j = i + n; j < len && o + 1 < outsz; j++) {
				if (js[j] == '"') {
					out[o] = '\0';
					return true;
				}
				out[o++] = js[j];
			}
		}
	}
	return false;
}

/* ==================== GET /api/info ==================== */

int web_build_info_json(char *json, size_t bufsz)
{
	const struct apollo_storage_info *si = storage_info();
	const char *ip = net_ip_str();
	int adc = board_io_adc_mv();
	uint16_t als = 0, ps = 0;
	bool als_ok = board_io_als_read(&als) == 0;
	bool ps_ok = board_io_ps_read(&ps) == 0;
	uint32_t k[4];
#ifdef CONFIG_APOLLO_CAN
	uint32_t rx = 0, tx = 0;

	can_get_stats(&rx, &tx);
#endif
	for (int i = 0; i < 4; i++) {
		if (!board_io_key_count(i, &k[i])) {
			k[i] = 0;
		}
	}

	int n = snprintf(json, bufsz,
		"{\"fw\":\"%s\","
		"\"build\":\"%s %s\","
		"\"uptime_s\":%u,"
		"\"time\":\"%s\","
		"\"ip\":\"%s\","
		"\"net\":\"%s\","
		"\"qspi\":%s,\"sdram\":%s,"
		"\"sd\":%s,\"qspi_fs\":%s,"
		"\"font\":%s,"
		"\"adc_mv\":%d,"
		"\"als\":%s%d,\"ps\":%s%d,"
		"\"keys\":[%u,%u,%u,%u]"
#ifdef CONFIG_APOLLO_CAN
		",\"can_rx\":%u,\"can_tx\":%u"
#endif
		"}",
		APOLLO_FW_VERSION, __DATE__, __TIME__,
		k_uptime_get_32() / 1000,
		app_time_local_str(),
		ip ? ip : "",
		net_ip_source() == NET_IP_DHCP ? "dhcp" :
			net_ip_source() == NET_IP_STATIC_FALLBACK ? "static" : "down",
		si->qspi_ok ? "true" : "false",
		si->sdram_ok ? "true" : "false",
		si->sd_mounted ? "true" : "false",
		si->qspi_fs_mounted ? "true" : "false",
		apollo_font_qspi_ready() ? "true" : "false",
		adc,
		als_ok ? "" : "\"err\":", als, ps_ok ? "" : "\"err\":", ps,
		k[0], k[1], k[2], k[3]
#ifdef CONFIG_APOLLO_CAN
		, rx, tx
#endif
		);
	return MIN(n, (int)bufsz);
}

static int info_handler(struct http_client_ctx *client,
			enum http_transaction_status status,
			const struct http_request_ctx *req,
			struct http_response_ctx *rsp, void *user_data)
{
	static char json[768];

	ARG_UNUSED(client);
	ARG_UNUSED(req);
	ARG_UNUSED(user_data);
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	rsp->status = HTTP_200_OK;
	rsp->body = json;
	rsp->body_len = web_build_info_json(json, sizeof(json));
	rsp->final_chunk = true;
	return 0;
}

/* ==================== POST /api/ctl ==================== */

static int ctl_handler(struct http_client_ctx *client,
		       enum http_transaction_status status,
		       const struct http_request_ctx *req,
		       struct http_response_ctx *rsp, void *user_data)
{
	int32_t v;
	int applied = 0;

	ARG_UNUSED(client);
	ARG_UNUSED(user_data);
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	const char *body = req->data;
	size_t body_len = req->data_len;

	if (json_get_int(body, body_len, "led0", &v)) {
		board_io_led_set(0, v != 0);
		applied++;
	}
	if (json_get_int(body, body_len, "led1", &v)) {
		board_io_led_set(1, v != 0);
		applied++;
	}
	if (json_get_int(body, body_len, "beep", &v)) {
		board_io_beeper_set(v != 0);
		applied++;
	}
	if (json_get_int(body, body_len, "bl", &v)) {
		apollo_display_backlight_set(v != 0);
		applied++;
	}

	rsp->status = HTTP_200_OK;
	rsp->body = applied > 0 ? "{\"ok\":true}" : "{\"ok\":false}";
	rsp->body_len = applied > 0 ? 14 : 15;
	rsp->final_chunk = true;
	return 0;
}

/* ==================== POST /api/time ==================== */

static int api_time_handler(struct http_client_ctx *client,
			    enum http_transaction_status status,
			    const struct http_request_ctx *req,
			    struct http_response_ctx *rsp, void *user_data)
{
	char ts[32], date[16], hms[16];

	ARG_UNUSED(client);
	ARG_UNUSED(user_data);
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	if (json_get_str(req->data, req->data_len, "time", ts, sizeof(ts)) &&
	    sscanf(ts, "%15s %15s", date, hms) == 2 &&
	    app_time_set_local_str(date, hms) == 0) {
		rsp->status = HTTP_200_OK;
		rsp->body = "{\"ok\":true}";
		rsp->body_len = 13;
	} else {
		rsp->status = HTTP_400_BAD_REQUEST;
		rsp->body = "{\"ok\":false}";
		rsp->body_len = 14;
	}
	rsp->final_chunk = true;
	return 0;
}

/* ==================== POST /api/reboot ==================== */

static int reboot_handler(struct http_client_ctx *client,
			  enum http_transaction_status status,
			  const struct http_request_ctx *req,
			  struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(req);
	ARG_UNUSED(user_data);
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	rsp->status = HTTP_200_OK;
	rsp->body = "{\"ok\":true}";
	rsp->body_len = 13;
	rsp->final_chunk = true;
	/* 稍等片刻让响应先发出去 */
	k_sleep(K_SECONDS(1));
	sys_reboot(SYS_REBOOT_WARM);
	return 0;
}

/* ==================== 资源注册 ==================== */

#define DYNAMIC_DETAIL(_name, _methods)                                        \
	static struct http_resource_detail_dynamic _name = {                   \
		.common = {                                                    \
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,                    \
			.bitmask_of_supported_http_methods = (_methods),      \
			.content_type = "application/json",                   \
		},                                                             \
		.cb = _name##_handler,                                         \
		.user_data = NULL,                                             \
	}

DYNAMIC_DETAIL(info, BIT(HTTP_GET));
DYNAMIC_DETAIL(ctl, BIT(HTTP_POST));
DYNAMIC_DETAIL(api_time, BIT(HTTP_POST));
DYNAMIC_DETAIL(reboot, BIT(HTTP_POST));

static uint16_t web_port = WEB_PORT;

HTTP_SERVICE_DEFINE(apollo_web, NULL, &web_port,
		    CONFIG_HTTP_SERVER_MAX_CLIENTS, 10, NULL, NULL, NULL);

HTTP_RESOURCE_DEFINE(res_index, apollo_web, "/", &index_detail);
HTTP_RESOURCE_DEFINE(res_info, apollo_web, "/api/info", &info);
HTTP_RESOURCE_DEFINE(res_ctl, apollo_web, "/api/ctl", &ctl);
HTTP_RESOURCE_DEFINE(res_time, apollo_web, "/api/time", &api_time);
HTTP_RESOURCE_DEFINE(res_reboot, apollo_web, "/api/reboot", &reboot);

/* /ws: WebSocket 实时推送 + 固件升级 (ws.c) */
extern struct http_resource_detail_websocket apollo_ws_detail;
HTTP_RESOURCE_DEFINE(res_ws, apollo_web, "/ws", &apollo_ws_detail);

/* 由 net.c 在 IP 就绪后调用 (SYS_INIT 期 iface 未 up, bind 行为未定义) */
int apollo_web_start(void)
{
	static bool started;

	if (started) {
		return 0;
	}
	started = true;
	int rc = http_server_start();

	if (rc != 0) {
		LOG_ERR("http_server_start failed: %d", rc);
		return rc;
	}
	LOG_INF("web server on port %u", WEB_PORT);
	return 0;
}
