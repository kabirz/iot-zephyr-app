/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web 服务: HTTP (端口 80) + REST API + WebSocket 实时推送
 *
 *   - 页面: / (gzip 压缩 SPA, CMake 期由 index.html 生成)
 *   - 查询: /api/info  设备详情   /api/io  实时 IO   /api/regs  寄存器
 *           /api/history  历史文件列表
 *   - 下载: /api/history/download?name=data_*.raw (LittleFS 流式下载)
 *   - 控制: /api/do /api/reg /api/time /api/save /api/reboot
 *           /api/history/delete (写路径与 Modbus 回调共用 io_write_*,
 *           副作用与 FC05/FC06 完全一致)
 *   - 升级: 走 WebSocket (ws_io.c) fw_start/fw_data/fw_end 命令
 *   - 实时: /ws  WebSocket (ws_io.c), 1s 推送 DI/DO/AI 快照
 *
 * HTTP/1.1 only (HTTP/2 关闭省 RAM); 动态资源 holder 机制保证
 * 同一资源同时只有一个客户端 (升级/下载状态机依赖此约束)。
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/app_version.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/net_if.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>
#include <fw_gitver.h>
#include <init.h>

#include "web_json.h"
#include "ws_io.h"
#include "watchdog.h"

LOG_MODULE_REGISTER(io_web, LOG_LEVEL_INF);

#ifndef CONFIG_FLASH_SIZE
#define CONFIG_FLASH_SIZE 0
#endif
#ifndef CONFIG_SRAM_SIZE
#define CONFIG_SRAM_SIZE 0
#endif

/* ==================== 静态页面 (gzip) ==================== */

static const uint8_t index_html_gz[] = {
#include "web_index_html_gz.inc"
};

static struct http_resource_detail_static index_html_detail = {
	.common =
		{
			.type = HTTP_RESOURCE_TYPE_STATIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),
			.content_encoding = "gzip",
			.content_type = "text/html",
		},
	.static_data = index_html_gz,
	.static_data_len = sizeof(index_html_gz),
};

/* ==================== 通用响应辅助 ==================== */

#define JSON_OK      "{\"ok\":true}"
#define JSON_BAD_REQ "{\"ok\":false,\"err\":\"bad request\"}"

static void respond_json_ok(struct http_response_ctx *rsp)
{
	rsp->status = HTTP_200_OK;
	rsp->body = JSON_OK;
	rsp->body_len = sizeof(JSON_OK) - 1;
	rsp->final_chunk = true;
}

static void respond_json_err(struct http_response_ctx *rsp, const char *err)
{
	static char err_buf[96];

	snprintf(err_buf, sizeof(err_buf), "{\"ok\":false,\"err\":\"%s\"}", err);
	rsp->status = HTTP_400_BAD_REQUEST;
	rsp->body = err_buf;
	rsp->body_len = strlen(err_buf);
	rsp->final_chunk = true;
}

/* POST body 累积器 (JSON 控制命令都很小, 单缓冲足够;
 * holder 保证单客户端串行访问) */
#define BODY_MAX 128

static char body_buf[BODY_MAX];
static size_t body_len;
static bool body_done; /* 上一事务已处理完, 新事务首块先清缓冲 */

static void body_reset(void)
{
	body_len = 0;
	body_done = false;
}

/* 事务结束标记 (FINAL 处理完后调用): 下一次 body_append 先清旧数据,
 * 否则残留的旧 JSON 会被再次解析 (点 DO 控错通道/失灵的元凶) */
static void body_finalize(void)
{
	body_done = true;
}

static bool body_append(const char *data, size_t len)
{
	if (body_done) {
		body_len = 0;
		body_done = false;
	}
	if (body_len + len > BODY_MAX) {
		return false;
	}
	memcpy(body_buf + body_len, data, len);
	body_len += len;
	body_buf[body_len] = '\0';
	return true;
}

/* ==================== GET /api/info ==================== */

/* 设备详情 JSON 构造 (HTTP handler 与 WS 推送共用), 声明见 web_cmds.h */
int web_build_info_json(char *json, size_t bufsz)
{
	/* MAC / 网络状态 */
	char mac_str[18] = "00:00:00:00:00:00";
	struct net_if *iface = net_if_get_default();

	if (iface != NULL) {
		const struct net_linkaddr *ll = net_if_get_link_addr(iface);

		if (ll->len == 6) {
			snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
				 ll->addr[0], ll->addr[1], ll->addr[2], ll->addr[3], ll->addr[4],
				 ll->addr[5]);
		}
	}

	unsigned long long lfs_free = 0, lfs_total = 0;
	struct fs_statvfs st;

	if (fs_statvfs("/lfs1", &st) == 0) {
		lfs_free = (unsigned long long)st.f_bfree * st.f_bsize;
		lfs_total = (unsigned long long)st.f_blocks * st.f_bsize;
	}

	/* snprintf may return more than bufsz if truncated; clamp to buffer size */
	int n = snprintf(
		json, bufsz,
		"{\"t\":\"info\","
		"\"version\":\"v%d.%d.%d_%s\","
		"\"build\":\"%s %s\","
		"\"board\":\"%s\","
		"\"hclk_mhz\":%u,"
		"\"flash_kb\":%d,\"sram_kb\":%d,"
		"\"mac\":\"%s\","
		"\"ip\":\"%u.%u.%u.%u\","
		"\"slave_id\":%u,\"rs485_baud\":%u,"
		"\"can_id\":%u,\"can_baud\":%u,"
		"\"uptime_ms\":%lld,\"time\":%lld,"
		"\"hist_en\":%u,"
		"\"lfs_free\":%llu,\"lfs_total\":%llu,"
		"\"net_up\":%s,"
		"\"di_ms\":%u,\"ai_ms\":%u}",
		APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_PATCHLEVEL, FW_GIT_VERSION, __DATE__,
		__TIME__, CONFIG_BOARD_TARGET, CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U,
		CONFIG_FLASH_SIZE, CONFIG_SRAM_SIZE, mac_str,
		get_holding_reg(HOLDING_IP_OCTET1_IDX), get_holding_reg(HOLDING_IP_OCTET2_IDX),
		get_holding_reg(HOLDING_IP_OCTET3_IDX), get_holding_reg(HOLDING_IP_OCTET4_IDX),
		get_holding_reg(HOLDING_SLAVE_ID_IDX), get_holding_reg(HOLDING_RS485_BAUDRATE_IDX),
		get_holding_reg(HOLDING_CAN_ID_IDX), get_holding_reg(HOLDING_CAN_BAUDRATE_IDX),
		(long long)k_uptime_get(), (long long)time(NULL),
		get_holding_reg(HOLDING_HISTORY_ENABLE_IDX) != 0, lfs_free, lfs_total,
		net_link_is_up() ? "true" : "false", get_holding_reg(HOLDING_DI_SAMPLE_MS_IDX),
		get_holding_reg(HOLDING_AI_SAMPLE_MS_IDX));
	return (n > (int)bufsz) ? (int)bufsz : n;
}

static int info_handler(struct http_client_ctx *client, enum http_transaction_status status,
			const struct http_request_ctx *req, struct http_response_ctx *rsp,
			void *user_data)
{
	static char json[640];

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	rsp->status = HTTP_200_OK;
	rsp->body = json;
	rsp->body_len = web_build_info_json(json, sizeof(json));
	rsp->final_chunk = true;
	return 0;
}

/* ==================== GET /api/io ==================== */

static int io_handler(struct http_client_ctx *client, enum http_transaction_status status,
		      const struct http_request_ctx *req, struct http_response_ctx *rsp,
		      void *user_data)
{
	static char json[256];

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	rsp->status = HTTP_200_OK;
	rsp->body = json;
	rsp->body_len = ws_io_build_status(json, sizeof(json));
	rsp->final_chunk = true;
	return 0;
}

/* ==================== GET /api/regs ==================== */

/* 寄存器全量 JSON 构造 (HTTP handler 与 WS 推送共用), 声明见 web_cmds.h */
int web_build_regs_json(char *json, size_t bufsz)
{
	int n = snprintf(json, bufsz, "{\"t\":\"regs\",\"holding\":[");

	for (int i = 0; i < CONFIG_MODBUS_HOLDING_REGISTER_NUMBERS; i++) {
		/* io_read_holding: 时间戳寄存器返回实时时间 (与 FC03 一致),
		 * 否则 web 上时间戳恒为 0 且不动 */
		n += snprintf(json + n, bufsz - n, "%s%u", i ? "," : "", io_read_holding(i));
	}
	n += snprintf(json + n, bufsz - n, "],\"input\":[");
	for (int i = 0; i < CONFIG_MODBUS_INPUT_REGISTER_NUMBERS; i++) {
		n += snprintf(json + n, bufsz - n, "%s%u", i ? "," : "", get_input_reg(i));
	}
	n += snprintf(json + n, bufsz - n, "]}");
	return (n > (int)bufsz) ? (int)bufsz : n;
}

static int regs_handler(struct http_client_ctx *client, enum http_transaction_status status,
			const struct http_request_ctx *req, struct http_response_ctx *rsp,
			void *user_data)
{
	static char json[256];

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	rsp->status = HTTP_200_OK;
	rsp->body = json;
	rsp->body_len = web_build_regs_json(json, sizeof(json));
	rsp->final_chunk = true;
	return 0;
}

/* ==================== POST 控制命令 ==================== */

/* 公共执行器 (HTTP POST 与 WebSocket 命令共用):
 * do  → io_write_do_bit (FC05 同路径)
 * reg → io_write_holding (FC06 同路径, 0x11 重启改为延迟重启保住响应) */
int web_cmd_exec_do(int32_t index, int32_t value)
{
	if (index < 0 || index >= DO_NUM) {
		return -EINVAL;
	}
	return io_write_do_bit((uint16_t)index, value != 0);
}

int web_cmd_exec_reg(int32_t addr, int32_t value)
{
	if (addr < 0 || addr >= CONFIG_MODBUS_HOLDING_REGISTER_NUMBERS || value < 0 ||
	    value > 0xFFFF) {
		return -EINVAL;
	}
	if (addr == HOLDING_REBOOT_IDX) {
		/* 立即重启会掐断 HTTP/WS 响应, 改走延迟重启 (主循环刷日志后复位) */
		if (value) {
			set_reboot_status(true);
		}
		return 0;
	}
	return io_write_holding((uint16_t)addr, (uint16_t)value);
}

/* ==================== 系统配置命令 (WS "cfg", 字段均可选) ====================
 * {"cmd":"cfg","ip":"192.168.12.101","rs485":9600,"sid":1,
 *  "can_bps":250,"can_id":273}
 * 校验通过才写入 holding_reg; 失败时 *err 指向静态错误描述 */
int web_cmd_exec_cfg(const char *json, size_t len, const char **err)
{
	static const char *e_bad_ip = "invalid ip";
	static const char *e_bad_rs = "invalid rs485 baud";
	static const char *e_bad_sid = "invalid slave id";
	static const char *e_bad_can = "invalid can baud";
	static const char *e_bad_cid = "invalid can id";

	char ip_str[16];
	int32_t v;

	/* IP: 点分十进制字符串, 复用 UDP/Modbus 同一套合法性规则 */
	if (json_get_str(json, len, "ip", ip_str, sizeof(ip_str))) {
		uint32_t a, b, c, d;

		if (sscanf(ip_str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 ||
		    c > 255 || d > 255 ||
		    !ip_addr_valid((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d)) {
			*err = e_bad_ip;
			return -EINVAL;
		}
		io_write_holding(HOLDING_IP_OCTET1_IDX, (uint16_t)a);
		io_write_holding(HOLDING_IP_OCTET2_IDX, (uint16_t)b);
		io_write_holding(HOLDING_IP_OCTET3_IDX, (uint16_t)c);
		io_write_holding(HOLDING_IP_OCTET4_IDX, (uint16_t)d);
	}

	/* RS485 波特率: 常用 Modbus 范围 */
	if (json_get_i32(json, len, "rs485", &v)) {
		if (v < 1200 || v > 115200) {
			*err = e_bad_rs;
			return -EINVAL;
		}
		io_write_holding(HOLDING_RS485_BAUDRATE_IDX, (uint16_t)v);
	}

	/* Modbus slave id: 1-247 */
	if (json_get_i32(json, len, "sid", &v)) {
		if (v < 1 || v > 247) {
			*err = e_bad_sid;
			return -EINVAL;
		}
		io_write_holding(HOLDING_SLAVE_ID_IDX, (uint16_t)v);
	}

	/* CAN 波特率 (寄存器存 x1000): 常用档位 */
	if (json_get_i32(json, len, "can_bps", &v)) {
		if (v != 50 && v != 100 && v != 125 && v != 250 && v != 500 && v != 800 &&
		    v != 1000) {
			*err = e_bad_can;
			return -EINVAL;
		}
		io_write_holding(HOLDING_CAN_BAUDRATE_IDX, (uint16_t)v);
	}

	/* CAN ID: 标准帧 1-0x7FF */
	if (json_get_i32(json, len, "can_id", &v)) {
		if (v < 1 || v > 0x7FF) {
			*err = e_bad_cid;
			return -EINVAL;
		}
		io_write_holding(HOLDING_CAN_ID_IDX, (uint16_t)v);
	}

	return 0;
}

static int api_do_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *req, struct http_response_ctx *rsp,
			  void *user_data)
{
	int32_t index = 0, value = 0;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		body_reset();
		return 0;
	}
	if (!body_append(req->data, req->data_len)) {
		respond_json_err(rsp, "body too large");
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	if (json_get_i32(body_buf, body_len, "index", &index) &&
	    json_get_i32(body_buf, body_len, "value", &value) &&
	    web_cmd_exec_do(index, value) == 0) {
		respond_json_ok(rsp);
	} else {
		respond_json_err(rsp, "invalid index/value");
	}
	body_finalize();
	return 0;
}

static int reg_handler(struct http_client_ctx *client, enum http_transaction_status status,
		       const struct http_request_ctx *req, struct http_response_ctx *rsp,
		       void *user_data)
{
	int32_t addr = -1, value = 0;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		body_reset();
		return 0;
	}
	if (!body_append(req->data, req->data_len)) {
		respond_json_err(rsp, "body too large");
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	if (json_get_i32(body_buf, body_len, "addr", &addr) &&
	    json_get_i32(body_buf, body_len, "value", &value) &&
	    web_cmd_exec_reg(addr, value) == 0) {
		respond_json_ok(rsp);
	} else {
		respond_json_err(rsp, "invalid addr/value");
	}
	body_finalize();
	return 0;
}

static int api_time_handler(struct http_client_ctx *client, enum http_transaction_status status,
			    const struct http_request_ctx *req, struct http_response_ctx *rsp,
			    void *user_data)
{
	int32_t ts = 0;

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		body_reset();
		return 0;
	}
	if (!body_append(req->data, req->data_len)) {
		respond_json_err(rsp, "body too large");
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	if (json_get_i32(body_buf, body_len, "ts", &ts) && set_timestamp((time_t)ts)) {
		respond_json_ok(rsp);
	} else {
		respond_json_err(rsp, "invalid timestamp");
	}
	body_finalize();
	return 0;
}

static int save_handler(struct http_client_ctx *client, enum http_transaction_status status,
			const struct http_request_ctx *req, struct http_response_ctx *rsp,
			void *user_data)
{
	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		body_finalize();
		holding_reg_save();
		respond_json_ok(rsp);
	}
	return 0;
}

static int reboot_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *req, struct http_response_ctx *rsp,
			  void *user_data)
{
	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		set_reboot_status(true);
		LOG_INF("web reboot requested");
		respond_json_ok(rsp);
	}
	return 0;
}

/* ==================== GET /api/history ==================== */

#define HIST_DIR "/lfs1"

/* 文件名合法性: 仅允许 data_ 前续 + 字母数字/._-, 杜绝路径穿越 */
static bool hist_name_valid(const char *name)
{
	size_t n = 0;

	if (strncmp(name, "data_", 5) != 0) {
		return false;
	}
	for (; name[n] != '\0'; n++) {
		char c = name[n];
		bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
			  (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '-';

		if (!ok) {
			return false;
		}
	}
	return n > 5 && n < 32;
}

static int history_handler(struct http_client_ctx *client, enum http_transaction_status status,
			   const struct http_request_ctx *req, struct http_response_ctx *rsp,
			   void *user_data)
{
	static char json[640];
	struct fs_dir_t dir;
	struct fs_dirent ent;
	int n = 0;

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	history_sync();

	n += snprintf(json + n, sizeof(json) - n, "{\"files\":[");

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, HIST_DIR) == 0) {
		bool first = true;

		while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
			if (ent.type != FS_DIR_ENTRY_FILE || !hist_name_valid(ent.name)) {
				continue;
			}
			n += snprintf(json + n, sizeof(json) - n, "%s{\"name\":\"%s\",\"size\":%u}",
				      first ? "" : ",", ent.name, (unsigned)ent.size);
			first = false;
			if (n >= (int)sizeof(json) - 64) {
				break;
			}
		}
		fs_closedir(&dir);
	}
	n += snprintf(json + n, sizeof(json) - n, "]}");

	rsp->status = HTTP_200_OK;
	rsp->body = json;
	rsp->body_len = n;
	rsp->final_chunk = true;
	return 0;
}

/* ==================== GET /api/history/download?name=xxx ==================== */

/* 流式下载状态 (holder 保证单客户端独占该资源) */
static struct {
	struct fs_file_t fp;
	bool open;
	bool started;
	size_t remain;
} dl;

#define DL_CHUNK 512

static int download_handler(struct http_client_ctx *client, enum http_transaction_status status,
			    const struct http_request_ctx *req, struct http_response_ctx *rsp,
			    void *user_data)
{
	static char chunk[DL_CHUNK];
	static struct http_header hdrs[2];
	static char disp[64];

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		if (dl.open) {
			fs_close(&dl.fp);
			dl.open = false;
		}
		dl.started = false;
		return 0;
	}

	if (!dl.started) {
		char name[32];

		dl.started = true;
		history_sync();
		if (req->data == NULL ||
		    !url_query_get((const char *)req->data, "name", name, sizeof(name)) ||
		    !hist_name_valid(name)) {
			respond_json_err(rsp, "invalid file name");
			rsp->final_chunk = true;
			return 0;
		}

		char path[48];
		struct fs_dirent ent;

		snprintf(path, sizeof(path), "%s/%s", HIST_DIR, name);
		if (fs_stat(path, &ent) != 0 || ent.type != FS_DIR_ENTRY_FILE) {
			respond_json_err(rsp, "file not found");
			rsp->final_chunk = true;
			return 0;
		}

		fs_file_t_init(&dl.fp);
		if (fs_open(&dl.fp, path, FS_O_READ) != 0) {
			respond_json_err(rsp, "open failed");
			rsp->final_chunk = true;
			return 0;
		}
		dl.open = true;
		dl.remain = ent.size;

		/* 响应头: 二进制流 + 附件下载 */
		snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
		hdrs[0].name = "Content-Type";
		hdrs[0].value = "application/octet-stream";
		hdrs[1].name = "Content-Disposition";
		hdrs[1].value = disp;
		rsp->headers = hdrs;
		rsp->header_count = 2;
		rsp->status = HTTP_200_OK;
	}

	/* 分块读发, http1 循环回调本 handler 直到 final_chunk */
	if (!dl.open) {
		rsp->final_chunk = true;
		return 0;
	}

	ssize_t len = fs_read(&dl.fp, chunk, sizeof(chunk));

	if (len < 0) {
		fs_close(&dl.fp);
		dl.open = false;
		rsp->body = NULL;
		rsp->body_len = 0;
		rsp->final_chunk = true;
		return 0;
	}

	rsp->body = chunk;
	rsp->body_len = len;
	dl.remain -= len;
	if (len == 0 || dl.remain == 0) {
		fs_close(&dl.fp);
		dl.open = false;
		rsp->final_chunk = true;
	} else {
		rsp->final_chunk = false;
	}
	return 0;
}

/* ==================== POST /api/history/delete ==================== */

static int hist_del_handler(struct http_client_ctx *client, enum http_transaction_status status,
			    const struct http_request_ctx *req, struct http_response_ctx *rsp,
			    void *user_data)
{
	char name[32];

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		body_reset();
		return 0;
	}
	if (!body_append(req->data, req->data_len)) {
		respond_json_err(rsp, "body too large");
		return 0;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	if (json_get_str(body_buf, body_len, "name", name, sizeof(name)) && hist_name_valid(name)) {
		char path[48];

		snprintf(path, sizeof(path), "%s/%s", HIST_DIR, name);
		if (fs_unlink(path) == 0) {
			LOG_INF("history %s deleted (web)", name);
			respond_json_ok(rsp);
			body_finalize();
			return 0;
		}
	}
	body_finalize();
	respond_json_err(rsp, "delete failed");
	return 0;
}

/* ==================== 资源注册 ==================== */

#define DYNAMIC_DETAIL(_name, _methods)                                                            \
	static struct http_resource_detail_dynamic _name = {                                       \
		.common =                                                                          \
			{                                                                          \
				.type = HTTP_RESOURCE_TYPE_DYNAMIC,                                \
				.bitmask_of_supported_http_methods = (_methods),                   \
				.content_type = "application/json",                                \
			},                                                                         \
		.cb = _name##_handler,                                                             \
		.user_data = NULL,                                                                 \
	}

DYNAMIC_DETAIL(info, BIT(HTTP_GET));
DYNAMIC_DETAIL(io, BIT(HTTP_GET));
DYNAMIC_DETAIL(regs, BIT(HTTP_GET));
DYNAMIC_DETAIL(api_do, BIT(HTTP_POST));
DYNAMIC_DETAIL(reg, BIT(HTTP_POST));
DYNAMIC_DETAIL(api_time, BIT(HTTP_POST));
DYNAMIC_DETAIL(save, BIT(HTTP_POST));
DYNAMIC_DETAIL(reboot, BIT(HTTP_POST));
DYNAMIC_DETAIL(history, BIT(HTTP_GET));
DYNAMIC_DETAIL(download, BIT(HTTP_GET));
DYNAMIC_DETAIL(hist_del, BIT(HTTP_POST));

static uint16_t io_web_port = CONFIG_IO_WEB_PORT;

HTTP_SERVICE_DEFINE(io_web, NULL, &io_web_port, CONFIG_HTTP_SERVER_MAX_CLIENTS, 10, NULL, NULL,
		    NULL);

HTTP_RESOURCE_DEFINE(res_index, io_web, "/", &index_html_detail);
HTTP_RESOURCE_DEFINE(res_info, io_web, "/api/info", &info);
HTTP_RESOURCE_DEFINE(res_io, io_web, "/api/io", &io);
HTTP_RESOURCE_DEFINE(res_regs, io_web, "/api/regs", &regs);
HTTP_RESOURCE_DEFINE(res_do, io_web, "/api/do", &api_do);
HTTP_RESOURCE_DEFINE(res_reg, io_web, "/api/reg", &reg);
HTTP_RESOURCE_DEFINE(res_time, io_web, "/api/time", &api_time);
HTTP_RESOURCE_DEFINE(res_save, io_web, "/api/save", &save);
HTTP_RESOURCE_DEFINE(res_reboot, io_web, "/api/reboot", &reboot);
HTTP_RESOURCE_DEFINE(res_history, io_web, "/api/history", &history);
HTTP_RESOURCE_DEFINE(res_download, io_web, "/api/history/download", &download);
HTTP_RESOURCE_DEFINE(res_hist_del, io_web, "/api/history/delete", &hist_del);
HTTP_RESOURCE_DEFINE(res_ws, io_web, "/ws", &ws_io_detail);

/* ==================== 启动 ==================== */

/* 由 main() 在网络链路就绪后调用 (SYS_INIT 阶段 iface 尚未 up,
 * 过早 bind/listen 行为未定义, 官方 sample 同样在 net up 后启动) */
int io_web_start(void)
{
	int rc = http_server_start();

	if (rc != 0) {
		LOG_ERR("http_server_start failed: %d", rc);
		return rc;
	}
	LOG_INF("web server on port %u (ws /ws)", io_web_port);
	return 0;
}
