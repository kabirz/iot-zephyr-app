/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * FTP server (RFC 959, single-thread select, max 3 clients)
 *   - PASV 被动 + PORT 主动数据连接
 *   - TYPE A (ASCII CR/LF 转换) / TYPE I (二进制)
 *   - select 多路复用控制命令; RETR/STOR/LIST 传输时该会话独占
 *   - 120s 空闲超时; 路径规范化(.. 防护); LIST 标准 Unix ls -l
 *   - 命令: USER PASS SYST FEAT TYPE PWD CWD CDUP PORT PASV
 *            LIST NLST RETR STOR APPE DELE MKD RMD RNFR RNTO SIZE REST NOOP QUIT
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/posix/netinet/in.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/sys/select.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/logging/log.h>
#include <init.h>
#include "fs_littlefs.h"
#include "ftp.h"

LOG_MODULE_REGISTER(io_ftp, LOG_LEVEL_INF);

#define FTP_MAX_CLIENTS         3
#define FTP_SESSION_TIMEOUT_SEC 120
/* 控制/数据连接 socket 超时: 防止慢速/恶意客户端逐字节滴水长时间冻结
 * 整个 FTP 线程 (单线程 select 多路复用, 一冻全冻)。 */
#define FTP_CTRL_TIMEOUT_MS     10000 /* 控制连接 recv 超时 */
#define FTP_DATA_TIMEOUT_MS     15000 /* 数据连接 recv/send 超时 (大文件传输兜底) */

struct ftp_session {
	int ctrl;
	bool authed;
	bool anon;
	int data_listen;   /* PASV 监听 */
	bool data_is_port; /* PORT 主动模式 */
	struct sockaddr_in port_addr;
	char cwd[64];
	bool type_ascii;
	uint32_t rest;
	char rename_from[128];
	bool rename_pending;
	bool pending_cr; /* ASCII 上传: 上一块末尾的 \r 待与下一块 \n 合并 */
	int64_t last_activity;
	char buf[FTP_BUF_SIZE];
};

static __dtcm_bss_section struct ftp_session sessions[FTP_MAX_CLIENTS];

/* 数据连接整段发送: send() 在 TX 缓冲紧张时只接受部分字节, 忽略返回值
 * 会静默丢数据 (大文件传输内容错位的根因), 必须重试至发完 */
static int send_all(int s, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = send(s, p, len, 0);

		if (n <= 0) {
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static void ftp_send(int s, const char *msg){
	char buf[FTP_BUF_SIZE];
	int len = snprintf(buf, sizeof(buf), "%s\r\n", msg);

	(void)send(s, buf, len, 0);
}

static int ftp_sendf(int s, const char *fmt, ...)
{
	char buf[FTP_BUF_SIZE];
	va_list ap;

	va_start(ap, fmt);
	int len = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (len < 0) {
		return 0;
	}
	if (len + 2 >= (int)sizeof(buf)) {
		len = sizeof(buf) - 3;
	}
	buf[len++] = '\r';
	buf[len++] = '\n';
	return send(s, buf, len, 0);
}

/* 给 socket 设置 recv/send 超时 (毫秒) */
static void set_sock_timeout(int s, int timeout_ms)
{
	struct timeval tv = {
		.tv_sec = timeout_ms / 1000,
		.tv_usec = (timeout_ms % 1000) * 1000,
	};

	(void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* 数据连接超时设置 (供 open_data 统一调用) */
static void set_data_sock_timeout(int s)
{
	set_sock_timeout(s, FTP_DATA_TIMEOUT_MS);
}

/* 规范化客户端路径: 处理 绝对/相对 + . / .., 栈式防护 .. 越界 */
static void norm_path(char *out, size_t outlen, const char *cwd, const char *input)
{
	char tmp[160];
	const char *base = (input[0] == '/') ? "" : cwd;

	snprintf(tmp, sizeof(tmp), "%s/%s", base, input);

	const char *p = tmp;
	char parts[16][48];
	int n = 0;

	while (*p) {
		while (*p == '/') {
			p++;
		}
		if (!*p) {
			break;
		}
		const char *start = p;

		while (*p && *p != '/') {
			p++;
		}
		size_t len = p - start;

		if (len == 1 && start[0] == '.') {
			continue;
		}
		if (len == 2 && start[0] == '.' && start[1] == '.') {
			if (n > 0) {
				n--;
			}
			continue;
		}
		if (n < 16) {
			if (len > 47) {
				len = 47;
			}
			memcpy(parts[n], start, len);
			parts[n][len] = '\0';
			n++;
		}
	}

	size_t pos = 0;

	if (pos < outlen - 1) {
		out[pos++] = '/';
	}
	for (int i = 0; i < n && pos < outlen - 1; i++) {
		size_t pl = strlen(parts[i]);

		if (pos + pl + 1 >= outlen) {
			break;
		}
		memcpy(out + pos, parts[i], pl);
		pos += pl;
		out[pos++] = '/';
	}
	if (pos > 1 && out[pos - 1] == '/') {
		pos--;
	}
	out[pos] = '\0';
}

static void fs_path(char *out, size_t outlen, const char *cwd, const char *client_path)
{
	char norm[128];

	norm_path(norm, sizeof(norm), cwd, client_path);
	snprintf(out, outlen, "%s%s", FTP_ROOT, norm);
}

static void get_local_ip(uint8_t *ip)
{
	struct net_if *iface = net_if_get_default();

	memset(ip, 0, 4);
	if (iface) {
		struct in_addr *a =
			(struct in_addr *)net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

		if (a) {
			memcpy(ip, &a->s_addr, 4);
		}
	}
}

/* RETR ASCII: \n -> \r\n (out 容量需 >= 2*len) */
static size_t ascii_crlf(char *out, const char *in, size_t len)
{
	size_t o = 0;

	for (size_t i = 0; i < len; i++) {
		if (in[i] == '\n') {
			out[o++] = '\r';
		}
		out[o++] = in[i];
	}
	return o;
}

/* STOR ASCII: \r\n -> \n (原地缩短)。
 * pending_cr: 跨块合并标记 —— 若上一块末字节是 \r 而下一块首字节是 \n,
 * 原地算法会在块边界丢失合并; 用 *pending_cr 跨调用记录状态。
 * 调用方须在传输开始前置 *pending_cr=false, 传输结束后读取状态。 */
static size_t ascii_strip_cr(char *buf, size_t len, bool *pending_cr)
{
	size_t o = 0;
	size_t start = 0;

	/* 上一块末尾的 \r 与本块开头的 \n 合并 */
	if (*pending_cr && len > 0 && buf[0] == '\n') {
		start = 1;
	}
	*pending_cr = false;

	for (size_t i = start; i < len; i++) {
		if (buf[i] == '\r' && i + 1 < len && buf[i + 1] == '\n') {
			continue;
		}
		buf[o++] = buf[i];
	}
	/* 本块末字节若是 \r, 留待下一块判定是否跟 \n */
	if (len > start && buf[len - 1] == '\r') {
		if (o > 0) {
			o--; /* 暂不输出末尾 \r */
		}
		*pending_cr = true;
	}
	return o;
}

static const char *const ftp_months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
					 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* 历史文件名 data_MMDD_HHMM.raw -> 真实创建时间 (Zephyr fs_dirent 不暴露 mtime) */
static bool parse_hist_time(const char *name, int *mon, int *day, int *hour, int *min)
{
	unsigned int M, D, H, m;

	if (strncmp(name, "data_", 5) != 0) {
		return false;
	}
	if (sscanf(name + 5, "%2u%2u_%2u%2u", &M, &D, &H, &m) != 4) {
		return false;
	}
	if (M < 1 || M > 12 || D < 1 || D > 31 || H > 23 || m > 59) {
		return false;
	}
	*mon = M;
	*day = D;
	*hour = H;
	*min = m;
	return true;
}

/* ls -l 时间字段 "Mon DD HH:MM": 历史文件取真实时间, 其他用当前 RTC */
static void format_ls_time(char *out, size_t len, const char *name)
{
	int mon, day, hour, minute;

	if (parse_hist_time(name, &mon, &day, &hour, &minute)) {
		snprintf(out, len, "%s %2d %02d:%02d", ftp_months[mon - 1], day, hour, minute);
		return;
	}
	time_t t = time(NULL);
	struct tm *lt = gmtime(&t);

	/* RTC 未同步时 gmtime 可能返回 NULL, 避免解引用 NULL 触发 HardFault */
	if (lt == NULL) {
		snprintf(out, len, "%s %2d %02d:%02d", ftp_months[0], 1, 0, 0);
		return;
	}
	snprintf(out, len, "%s %2d %02d:%02d", ftp_months[lt->tm_mon], lt->tm_mday, lt->tm_hour,
		 lt->tm_min);
}

static void cmd_pasv(struct ftp_session *s)
{
	if (s->data_listen >= 0) {
		close(s->data_listen);
	}
	s->data_is_port = false;
	s->data_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s->data_listen < 0) {
		ftp_send(s->ctrl, "425 Cannot open passive connection");
		return;
	}

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = 0,
	};

	if (bind(s->data_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(s->data_listen, 1) < 0) {
		close(s->data_listen);
		s->data_listen = -1;
		ftp_send(s->ctrl, "425 Passive bind failed");
		return;
	}

	socklen_t alen = sizeof(addr);

	getsockname(s->data_listen, (struct sockaddr *)&addr, &alen);

	uint8_t ip[4];
	uint16_t port = ntohs(addr.sin_port);

	get_local_ip(ip);
	ftp_sendf(s->ctrl, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)", ip[0], ip[1], ip[2],
		  ip[3], (port >> 8) & 0xFF, port & 0xFF);
}

/* EPSV: 扩展被动模式 (RFC 2428), 回 229 (|||port|) */
static void cmd_epsv(struct ftp_session *s)
{
	if (s->data_listen >= 0) {
		close(s->data_listen);
	}
	s->data_is_port = false;
	s->data_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s->data_listen < 0) {
		ftp_send(s->ctrl, "425 Cannot open passive connection");
		return;
	}
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = 0,
	};
	if (bind(s->data_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(s->data_listen, 1) < 0) {
		close(s->data_listen);
		s->data_listen = -1;
		ftp_send(s->ctrl, "425 Passive bind failed");
		return;
	}
	socklen_t alen = sizeof(addr);

	getsockname(s->data_listen, (struct sockaddr *)&addr, &alen);
	ftp_sendf(s->ctrl, "229 Entering Extended Passive Mode (|||%u|)", ntohs(addr.sin_port));
}

/* PORT h1,h2,h3,h4,p1,p2 -> 主动模式目标地址 */
static void cmd_port(struct ftp_session *s, const char *arg)
{
	unsigned int h[4], p[2];

	if (sscanf(arg, "%u,%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3], &p[0], &p[1]) != 6) {
		ftp_send(s->ctrl, "501 Syntax error in parameters");
		return;
	}
	if (s->data_listen >= 0) {
		close(s->data_listen);
		s->data_listen = -1;
	}
	uint8_t ip[4] = {h[0], h[1], h[2], h[3]};

	memset(&s->port_addr, 0, sizeof(s->port_addr));
	s->port_addr.sin_family = AF_INET;
	memcpy(&s->port_addr.sin_addr.s_addr, ip, 4);
	s->port_addr.sin_port = htons((p[0] << 8) | p[1]);
	s->data_is_port = true;
	ftp_send(s->ctrl, "200 PORT command successful");
}

/* EPRT |1|ipv4|port| -> 扩展主动模式 (RFC 2428, 仅支持 IPv4) */
static void cmd_eprt(struct ftp_session *s, const char *arg)
{
	unsigned int proto;
	char addr[64];
	unsigned int port;

	if (sscanf(arg, "|%u|%63[^|]|%u|", &proto, addr, &port) != 3 || proto != 1) {
		ftp_send(s->ctrl, "522 Network protocol not supported, use (1)");
		return;
	}
	if (s->data_listen >= 0) {
		close(s->data_listen);
		s->data_listen = -1;
	}
	memset(&s->port_addr, 0, sizeof(s->port_addr));
	s->port_addr.sin_family = AF_INET;
	s->port_addr.sin_port = htons(port);
	if (inet_pton(AF_INET, addr, &s->port_addr.sin_addr) != 1) {
		ftp_send(s->ctrl, "501 Bad address");
		return;
	}
	s->data_is_port = true;
	ftp_send(s->ctrl, "200 EPRT command successful");
}

/* 建立数据连接: PASV (accept) 或 PORT (connect) */
static int open_data(struct ftp_session *s)
{
	if (s->data_is_port) {
		s->data_is_port = false;
		int d = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

		if (d < 0) {
			return -1;
		}
		if (connect(d, (struct sockaddr *)&s->port_addr, sizeof(s->port_addr)) < 0) {
			close(d);
			return -1;
		}
		set_data_sock_timeout(d);
		return d;
	}

	if (s->data_listen < 0) {
		return -1;
	}
	/* select + accept: Zephyr 的 SO_RCVTIMEO 对 accept() 不生效,
	 * 必须用 select 等数据连接 (带超时), 否则客户端 PASV 后不连
	 * 数据端口会永久阻塞单线程 FTP */
	fd_set rfds;
	struct timeval tv = {
		.tv_sec = FTP_DATA_TIMEOUT_MS / 1000,
		.tv_usec = (FTP_DATA_TIMEOUT_MS % 1000) * 1000,
	};

	FD_ZERO(&rfds);
	FD_SET(s->data_listen, &rfds);
	if (select(s->data_listen + 1, &rfds, NULL, NULL, &tv) <= 0 ||
	    !FD_ISSET(s->data_listen, &rfds)) {
		close(s->data_listen);
		s->data_listen = -1;
		return -1;
	}

	int d = accept(s->data_listen, NULL, NULL);

	close(s->data_listen);
	s->data_listen = -1;
	if (d >= 0) {
		set_data_sock_timeout(d);
	}
	return d;
}

/* LIST (long) / NLST (short) */
static void cmd_list(struct ftp_session *s, const char *path, bool long_fmt)
{
	char fspath[FTP_BUF_SIZE];

	fs_path(fspath, sizeof(fspath), s->cwd, path);

	struct fs_dirent de;

	if (fs_stat(fspath, &de) != 0) {
		ftp_send(s->ctrl, "550 No such file or directory");
		if (s->data_listen >= 0) {
			close(s->data_listen);
			s->data_listen = -1;
		}
		return;
	}

	int data = open_data(s);

	if (data < 0) {
		ftp_send(s->ctrl, "425 No data connection");
		return;
	}

	if (de.type == FS_DIR_ENTRY_FILE) {
		const char *base = strrchr(path, '/');

		base = base ? base + 1 : path;
		ftp_send(s->ctrl, "150 Here comes the file listing");
		char tbuf[24];

		format_ls_time(tbuf, sizeof(tbuf), base);
		int len = long_fmt ? snprintf(s->buf, sizeof(s->buf),
					      "-rw-r--r-- 1 owner group %10u %s %s\r\n",
					      (unsigned)de.size, tbuf, base)
				   : snprintf(s->buf, sizeof(s->buf), "%s\r\n", base);

		if (len > 0) {
			send_all(data, s->buf, len);
		}
		close(data);
		ftp_send(s->ctrl, "226 Transfer complete");
		return;
	}

	ftp_send(s->ctrl, "150 Here comes the directory listing");

	struct fs_dir_t dir;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, fspath) == 0) {
		while (fs_readdir(&dir, &de) == 0 && de.name[0] != '\0') {
			char tbuf[24];

			format_ls_time(tbuf, sizeof(tbuf), de.name);
			int len = long_fmt ? snprintf(s->buf, sizeof(s->buf),
						      "%s 1 owner group %10u %s %s\r\n",
						      de.type == FS_DIR_ENTRY_DIR ? "drwxr-xr-x"
										  : "-rw-r--r--",
						      (unsigned)de.size, tbuf, de.name)
					   : snprintf(s->buf, sizeof(s->buf), "%s\r\n", de.name);

			if (len > 0) {
				send_all(data, s->buf, len);
			}
		}
		fs_closedir(&dir);
	}
	close(data);
	ftp_send(s->ctrl, "226 Directory send OK");
}

static void cmd_retr(struct ftp_session *s, const char *path)
{
	if (!s->authed) {
		ftp_send(s->ctrl, "530 Not logged in");
		if (s->data_listen >= 0) {
			close(s->data_listen);
			s->data_listen = -1;
		}
		s->data_is_port = false;
		return;
	}

	char fspath[FTP_BUF_SIZE];

	fs_path(fspath, sizeof(fspath), s->cwd, path);

	struct fs_file_t fp;

	fs_file_t_init(&fp);
	if (fs_open(&fp, fspath, FS_O_READ) != 0) {
		ftp_send(s->ctrl, "550 Failed to open file");
		if (s->data_listen >= 0) {
			close(s->data_listen);
			s->data_listen = -1;
		}
		s->data_is_port = false;
		return;
	}

	if (s->rest > 0) {
		if (fs_seek(&fp, s->rest, FS_SEEK_SET) != 0) {
			fs_close(&fp);
			ftp_send(s->ctrl, "550 Seek failed");
			s->rest = 0;
			if (s->data_listen >= 0) {
				close(s->data_listen);
				s->data_listen = -1;
			}
			s->data_is_port = false;
			return;
		}
	}

	ftp_send(s->ctrl, "150 Opening data connection");

	int data = open_data(s);

	if (data < 0) {
		fs_close(&fp);
		ftp_send(s->ctrl, "425 No data connection");
		s->rest = 0;
		return;
	}

	ssize_t n;
	static char in[FTP_BUF_SIZE / 2]; /* 单线程 select: 静态复用, 避免大数组压栈 */

	while ((n = fs_read(&fp, in, sizeof(in))) > 0) {
		size_t send_len = n;

		if (s->type_ascii) {
			send_len = ascii_crlf(s->buf, in, n);
			(void)send_all(data, s->buf, send_len);
		} else {
			(void)send_all(data, in, n);
		}
	}
	fs_close(&fp);
	close(data);
	s->rest = 0;
	ftp_send(s->ctrl, "226 Transfer complete");
}

static void cmd_stor(struct ftp_session *s, const char *path, bool is_appe)
{
	if (!s->authed || s->anon) {
		ftp_send(s->ctrl, "530 Permission denied");
		if (s->data_listen >= 0) {
			close(s->data_listen);
			s->data_listen = -1;
		}
		s->data_is_port = false;
		return;
	}

	char fspath[FTP_BUF_SIZE];
	uint32_t rest = s->rest;

	fs_path(fspath, sizeof(fspath), s->cwd, path);

	struct fs_file_t fp;

	fs_file_t_init(&fp);
	/* STOR (无 REST): 截断旧文件全新写入; REST 续传: 定位到偏移;
	 * APPE: 不截断, 追加到文件末尾 (FS_O_APPEND 保证每次写都定位到 EOF)。 */
	int flags = FS_O_CREATE | FS_O_WRITE;

	if (is_appe) {
		flags |= FS_O_APPEND;
	} else if (rest == 0) {
		flags |= FS_O_TRUNC;
	}
	if (fs_open(&fp, fspath, flags) != 0) {
		ftp_send(s->ctrl, "550 Failed to open file");
		if (s->data_listen >= 0) {
			close(s->data_listen);
			s->data_listen = -1;
		}
		s->data_is_port = false;
		return;
	}

	if (is_appe) {
		/* APPE: 定位到文件末尾, 后续数据追加写入 */
		fs_seek(&fp, 0, FS_SEEK_END);
	} else if (rest > 0) {
		fs_seek(&fp, rest, FS_SEEK_SET);
	}

	ftp_send(s->ctrl, "150 Ok to send data");

	int data = open_data(s);

	if (data < 0) {
		fs_close(&fp);
		ftp_send(s->ctrl, "425 No data connection");
		s->rest = 0;
		return;
	}

	ssize_t n;

	s->pending_cr = false;
	while ((n = recv(data, s->buf, sizeof(s->buf), 0)) > 0) {
		size_t wlen = s->type_ascii ? ascii_strip_cr(s->buf, n, &s->pending_cr) : (size_t)n;

		if (wlen > 0 && fs_write(&fp, s->buf, wlen) != wlen) {
			break;
		}
	}
	fs_close(&fp);
	close(data);
	s->rest = 0;
	ftp_send(s->ctrl, "226 Transfer complete");
}

/* 处理一条命令 (数据命令在此阻塞传输, 独占) */
static void handle_command(struct ftp_session *s, char *line)
{
	char *sp = strchr(line, ' ');
	char cmd[8] = {0};
	char *arg = sp ? sp + 1 : (char *)"";

	if (sp) {
		size_t clen = sp - line;

		if (clen >= sizeof(cmd)) {
			clen = sizeof(cmd) - 1;
		}
		memcpy(cmd, line, clen);
		cmd[clen] = '\0';
	} else {
		strncpy(cmd, line, sizeof(cmd) - 1);
	}
	for (char *p = cmd; *p; p++) {
		if (*p >= 'a' && *p <= 'z') {
			*p -= 'a' - 'A';
		}
	}

	s->last_activity = k_uptime_get();
	LOG_DBG("FTP cmd: %s %s", cmd, arg);

	if (!strcmp(cmd, "USER")) {
		s->anon = !strcmp(arg, "anonymous") || !strcmp(arg, "ftp");
		s->rename_pending = false;
		ftp_send(s->ctrl, "331 Please specify the password");
	} else if (!strcmp(cmd, "PASS")) {
		if (s->anon || !strcmp(arg, FTP_PASS)) {
			s->authed = true;
			ftp_send(s->ctrl, "230 Login successful");
		} else {
			s->authed = false;
			ftp_send(s->ctrl, "530 Login incorrect");
		}
	} else if (!strcmp(cmd, "SYST")) {
		ftp_send(s->ctrl, "215 UNIX Type: L8");
	} else if (!strcmp(cmd, "FEAT")) {
		ftp_send(s->ctrl,
			 "211-Features:\r\n SIZE\r\n PASV\r\n EPSV\r\n PORT\r\n EPRT"
			 "\r\n REST STREAM\r\n TYPE A;I\r\n NLST\r\n MKD\r\n RMD\r\n211 END");
	} else if (!strcmp(cmd, "TYPE")) {
		s->type_ascii = (arg[0] == 'A' || arg[0] == 'a');
		ftp_send(s->ctrl, "200 Type set");
	} else if (!strcmp(cmd, "PWD")) {
		ftp_sendf(s->ctrl, "257 \"%s\" is the current directory", s->cwd);
	} else if (!strcmp(cmd, "CWD")) {
		norm_path(s->cwd, sizeof(s->cwd), s->cwd, arg);
		ftp_send(s->ctrl, "250 Directory successfully changed");
	} else if (!strcmp(cmd, "CDUP")) {
		norm_path(s->cwd, sizeof(s->cwd), s->cwd, "..");
		ftp_send(s->ctrl, "250 Directory successfully changed");
	} else if (!strcmp(cmd, "PASV")) {
		cmd_pasv(s);
	} else if (!strcmp(cmd, "EPSV")) {
		cmd_epsv(s);
	} else if (!strcmp(cmd, "PORT")) {
		cmd_port(s, arg);
	} else if (!strcmp(cmd, "EPRT")) {
		cmd_eprt(s, arg);
	} else if (!strcmp(cmd, "LIST") || !strcmp(cmd, "NLST")) {
		history_sync();
		cmd_list(s, arg, !strcmp(cmd, "LIST"));
	} else if (!strcmp(cmd, "RETR")) {
		history_sync();
		cmd_retr(s, arg);
	} else if (!strcmp(cmd, "STOR") || !strcmp(cmd, "APPE")) {
		cmd_stor(s, arg, !strcmp(cmd, "APPE"));
	} else if (!strcmp(cmd, "DELE")) {
		char fspath[FTP_BUF_SIZE];

		fs_path(fspath, sizeof(fspath), s->cwd, arg);
		int rc = (!s->authed || s->anon) ? -1 : fs_unlink(fspath);

		ftp_send(s->ctrl, rc == 0 ? "250 Delete OK" : "550 Delete failed");
	} else if (!strcmp(cmd, "MKD") || !strcmp(cmd, "XMKD")) {
		char fspath[FTP_BUF_SIZE];

		fs_path(fspath, sizeof(fspath), s->cwd, arg);
		int rc = (!s->authed || s->anon) ? -1 : fs_mkdir(fspath);

		if (rc == 0) {
			ftp_sendf(s->ctrl, "257 \"%s\" created", arg);
		} else {
			ftp_send(s->ctrl, "550 Cannot create directory");
		}
	} else if (!strcmp(cmd, "RMD") || !strcmp(cmd, "XRMD")) {
		char fspath[FTP_BUF_SIZE];

		fs_path(fspath, sizeof(fspath), s->cwd, arg);
		int rc = (!s->authed || s->anon) ? -1 : fs_unlink(fspath);

		ftp_send(s->ctrl, rc == 0 ? "250 Remove OK" : "550 Cannot remove directory");
	} else if (!strcmp(cmd, "SIZE")) {
		char fspath[FTP_BUF_SIZE];
		struct fs_dirent ent;

		fs_path(fspath, sizeof(fspath), s->cwd, arg);
		if (fs_stat(fspath, &ent) == 0 && ent.type == FS_DIR_ENTRY_FILE) {
			ftp_sendf(s->ctrl, "213 %u", (unsigned)ent.size);
		} else {
			ftp_send(s->ctrl, "550 Not found");
		}
	} else if (!strcmp(cmd, "REST")) {
		unsigned long off = 0;

		sscanf(arg, "%lu", &off);
		s->rest = (uint32_t)off;
		ftp_sendf(s->ctrl, "350 Restart position accepted (%lu)", off);
	} else if (!strcmp(cmd, "RNFR")) {
		char fspath[FTP_BUF_SIZE];
		struct fs_dirent ent;

		fs_path(fspath, sizeof(fspath), s->cwd, arg);
		if (!s->authed || s->anon || fs_stat(fspath, &ent) != 0) {
			ftp_send(s->ctrl, "550 No such file");
		} else {
			strncpy(s->rename_from, fspath, sizeof(s->rename_from) - 1);
			s->rename_pending = true;
			ftp_send(s->ctrl, "350 Ready for RNTO");
		}
	} else if (!strcmp(cmd, "RNTO")) {
		char fspath[FTP_BUF_SIZE];

		fs_path(fspath, sizeof(fspath), s->cwd, arg);
		int rc = (!s->rename_pending || !s->authed || s->anon)
				 ? -1
				 : fs_rename(s->rename_from, fspath);

		s->rename_pending = false;
		ftp_send(s->ctrl, rc == 0 ? "250 Rename successful" : "550 Rename failed");
	} else if (!strcmp(cmd, "QUIT")) {
		ftp_send(s->ctrl, "221 Goodbye");
		close(s->ctrl);
		s->ctrl = -1;
	} else if (!strcmp(cmd, "NOOP") || !strcmp(cmd, "ALLO")) {
		ftp_send(s->ctrl, "200 OK");
	} else {
		ftp_send(s->ctrl, "502 Command not implemented");
	}
}

/* 读取一行命令 (非阻塞友好: select 已确认可读)。
 * 返回: >0 行长度, 0 对端关闭, <0 超时或错误 (errno=EAGAIN 视为超时)。 */
static int recv_line(int s, char *buf, int maxlen)
{
	int total = 0;

	while (total < maxlen - 1) {
		int n = recv(s, buf + total, 1, 0);

		if (n <= 0) {
			/* SO_RCVTIMEO 触发的超时不应关闭会话, 由空闲超时检查统一处理 */
			return (n < 0 && errno == EAGAIN) ? -1 : 0;
		}
		if (buf[total] == '\n') {
			break;
		}
		total++;
	}
	buf[total] = '\0';
	while (total > 0 && (buf[total - 1] == '\r' || buf[total - 1] == '\n')) {
		buf[--total] = '\0';
	}
	return total;
}

static void close_session(struct ftp_session *s)
{
	if (s->ctrl >= 0) {
		close(s->ctrl);
	}
	if (s->data_listen >= 0) {
		close(s->data_listen);
	}
	memset(s, 0, sizeof(*s));
	s->ctrl = -1;
	s->data_listen = -1;
	strcpy(s->cwd, "/");
}

static void ftp_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	(void)io_lfs_wait_ready(K_SECONDS(5));

	for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
		sessions[i].ctrl = -1;
		sessions[i].data_listen = -1;
		strcpy(sessions[i].cwd, "/");
	}

	int serv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (serv < 0) {
		LOG_ERR("FTP socket failed: %d", errno);
		return;
	}

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(FTP_CTRL_PORT),
	};
	int opt = 1;

	(void)setsockopt(serv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	if (bind(serv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(serv, FTP_MAX_CLIENTS) < 0) {
		LOG_ERR("FTP bind/listen failed: %d", errno);
		return;
	}

	LOG_INF("FTP server on port %d (root %s, max %d clients, single-thread)", FTP_CTRL_PORT,
		FTP_ROOT, FTP_MAX_CLIENTS);

	while (1) {
		fd_set rfds;
		int maxfd = serv;

		FD_ZERO(&rfds);
		FD_SET(serv, &rfds);
		for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
			if (sessions[i].ctrl >= 0) {
				FD_SET(sessions[i].ctrl, &rfds);
				if (sessions[i].ctrl > maxfd) {
					maxfd = sessions[i].ctrl;
				}
			}
		}

		struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
		int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);

		if (n > 0) {
			if (FD_ISSET(serv, &rfds)) {
				int c = accept(serv, NULL, NULL);

				if (c >= 0) {
					int slot = -1;

					for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
						if (sessions[i].ctrl < 0) {
							slot = i;
							break;
						}
					}
					if (slot < 0) {
						static const char busy[] = "421 Too many users\r\n";

						(void)send(c, busy, sizeof(busy) - 1, 0);
						close(c);
					} else {
						memset(&sessions[slot], 0, sizeof(sessions[slot]));
						sessions[slot].ctrl = c;
						sessions[slot].data_listen = -1;
						sessions[slot].last_activity = k_uptime_get();
						strcpy(sessions[slot].cwd, "/");
						/* 控制连接 recv 超时: 防止客户端
						 * 连接后慢速/不发命令冻结整个 FTP 线程 */
						set_sock_timeout(c, FTP_CTRL_TIMEOUT_MS);
						ftp_send(c, "220 io-edge-hub FTP service ready");
					}
				}
			}
			for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
				if (sessions[i].ctrl >= 0 && FD_ISSET(sessions[i].ctrl, &rfds)) {
					char line[FTP_BUF_SIZE];
					int rl = recv_line(sessions[i].ctrl, line, sizeof(line));

					if (rl > 0) {
						handle_command(&sessions[i], line);
					} else if (rl == 0) {
						LOG_INF("FTP client %d gone", i);
						close_session(&sessions[i]);
					}
					/* rl < 0: SO_RCVTIMEO 超时, 保留会话等下次 select */
				}
			}
		}

		/* 空闲超时检查 */
		int64_t now = k_uptime_get();

		for (int i = 0; i < FTP_MAX_CLIENTS; i++) {
			if (sessions[i].ctrl >= 0 &&
			    (now - sessions[i].last_activity) >
				    (FTP_SESSION_TIMEOUT_SEC * MSEC_PER_SEC)) {
				LOG_INF("FTP client %d timeout", i);
				close_session(&sessions[i]);
			}
		}
	}
}

K_THREAD_DEFINE(ftp, CONFIG_IO_FTP_STACK_SIZE, ftp_thread, NULL, NULL, NULL, CONFIG_IO_FTP_PRIORITY,
		0, 0);
