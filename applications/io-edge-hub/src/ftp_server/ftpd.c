/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * FTP 服务器 (LittleFS /lfs1, PASV 模式, 静态 buffer 无 malloc)
 *
 *   - 控制连接端口 21, 串行会话 (一次一客户端)
 *   - 认证 admin/admin (anonymous 只读)
 *   - PASV 被动模式数据连接; 命令: USER/PASS/SYST/TYPE/PWD/PASV/
 *     LIST/RETR/STOR/DELE/SIZE/QUIT
 *   - 用于 IO 历史记录文件下载/管理
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/posix/netinet/in.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/logging/log.h>
#include "fs_littlefs.h"
#include "ftp.h"

LOG_MODULE_REGISTER(io_ftp, LOG_LEVEL_INF);

static char ftp_buf[FTP_BUF_SIZE];

/* 发送一行 FTP 响应 (自动加 \r\n) */
static void ftp_send(int s, const char *msg)
{
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
	return send(s, buf, len, 0);
}

/* 读取一行命令 (去 \r\n) */
static int recv_line(int s, char *buf, int maxlen)
{
	int total = 0;

	while (total < maxlen - 1) {
		int n = recv(s, buf + total, 1, 0);

		if (n <= 0) {
			return n;
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

/* 取本机 IPv4 (PASV 回复用) */
static void get_local_ip(uint8_t *ip)
{
	struct net_if *iface = net_if_get_default();

	memset(ip, 0, 4);
	if (iface) {
		struct in_addr *a = (struct in_addr *)net_if_ipv4_get_global_addr(
			iface, NET_ADDR_PREFERRED);

		if (a) {
			memcpy(ip, &a->s_addr, 4);
		}
	}
}

/* 客户端路径 -> 文件系统路径 (/lfs1 + path) */
static void make_path(char *out, size_t outlen, const char *client_path)
{
	if (client_path[0] == '/') {
		snprintf(out, outlen, "%s%s", FTP_ROOT, client_path);
	} else {
		snprintf(out, outlen, "%s/%s", FTP_ROOT, client_path);
	}
}

/* 会话状态 */
struct ftp_session {
	int ctrl;
	bool authed;
	bool anon;
	int data_listen;	/* PASV 监听 socket */
	uint8_t ip[4];
};

/* PASV: 创建被动数据监听 socket */
static void cmd_pasv(struct ftp_session *sess)
{
	if (sess->data_listen >= 0) {
		close(sess->data_listen);
	}
	sess->data_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sess->data_listen < 0) {
		ftp_send(sess->ctrl, "425 Cannot open passive connection");
		return;
	}

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = 0,
	};

	if (bind(sess->data_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(sess->data_listen, 1) < 0) {
		close(sess->data_listen);
		sess->data_listen = -1;
		ftp_send(sess->ctrl, "425 Passive bind failed");
		return;
	}

	socklen_t alen = sizeof(addr);

	getsockname(sess->data_listen, (struct sockaddr *)&addr, &alen);
	get_local_ip(sess->ip);

	uint16_t port = ntohs(addr.sin_port);

	ftp_sendf(sess->ctrl,
		  "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
		  sess->ip[0], sess->ip[1], sess->ip[2], sess->ip[3],
		  (port >> 8) & 0xFF, port & 0xFF);
}

/* 等待并接受数据连接 */
static int data_accept(struct ftp_session *sess)
{
	if (sess->data_listen < 0) {
		return -1;
	}
	/* Zephyr 无 SO_REUSEADDR 等, accept 直接阻塞等待客户端连数据端口 */
	int d = accept(sess->data_listen, NULL, NULL);

	close(sess->data_listen);
	sess->data_listen = -1;
	return d;
}

static void cmd_list(struct ftp_session *sess, const char *path)
{
	int data = data_accept(sess);

	if (data < 0) {
		ftp_send(sess->ctrl, "425 No data connection");
		return;
	}
	ftp_send(sess->ctrl, "150 Here comes the directory listing");

	struct fs_dir_t dir;
	struct fs_dirent ent;

	fs_dir_t_init(&dir);
	char fspath[FTP_BUF_SIZE];

	make_path(fspath, sizeof(fspath), path);
	if (fs_opendir(&dir, fspath) == 0) {
		while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
			int len = snprintf(ftp_buf, sizeof(ftp_buf),
					   "%s %u %s\r\n",
					   ent.type == FS_DIR_ENTRY_DIR ? "D" : "F",
					   (unsigned)ent.size, ent.name);

			if (len > 0) {
				send(data, ftp_buf, len, 0);
			}
		}
		fs_closedir(&dir);
	}
	close(data);
	ftp_send(sess->ctrl, "226 Directory send OK");
}

static void cmd_retr(struct ftp_session *sess, const char *path)
{
	if (!sess->authed) {
		ftp_send(sess->ctrl, "530 Not logged in");
		if (sess->data_listen >= 0) {
			close(sess->data_listen);
			sess->data_listen = -1;
		}
		return;
	}
	int data = data_accept(sess);

	if (data < 0) {
		ftp_send(sess->ctrl, "425 No data connection");
		return;
	}

	char fspath[FTP_BUF_SIZE];

	make_path(fspath, sizeof(fspath), path);

	struct fs_file_t fp;

	fs_file_t_init(&fp);
	if (fs_open(&fp, fspath, FS_O_READ) != 0) {
		close(data);
		ftp_send(sess->ctrl, "550 Failed to open file");
		return;
	}

	ftp_send(sess->ctrl, "150 Opening data connection");

	ssize_t n;

	while ((n = fs_read(&fp, ftp_buf, sizeof(ftp_buf))) > 0) {
		if (send(data, ftp_buf, n, 0) < 0) {
			break;
		}
	}
	fs_close(&fp);
	close(data);
	ftp_send(sess->ctrl, "226 Transfer complete");
}

static void cmd_stor(struct ftp_session *sess, const char *path)
{
	if (!sess->authed || sess->anon) {
		ftp_send(sess->ctrl, "530 Permission denied");
		if (sess->data_listen >= 0) {
			close(sess->data_listen);
			sess->data_listen = -1;
		}
		return;
	}
	int data = data_accept(sess);

	if (data < 0) {
		ftp_send(sess->ctrl, "425 No data connection");
		return;
	}

	char fspath[FTP_BUF_SIZE];

	make_path(fspath, sizeof(fspath), path);

	struct fs_file_t fp;

	fs_file_t_init(&fp);
	if (fs_open(&fp, fspath, FS_O_CREATE | FS_O_WRITE) != 0) {
		close(data);
		ftp_send(sess->ctrl, "550 Failed to open file");
		return;
	}

	ftp_send(sess->ctrl, "150 Ok to send data");

	ssize_t n;

	while ((n = recv(data, ftp_buf, sizeof(ftp_buf), 0)) > 0) {
		if (fs_write(&fp, ftp_buf, n) != n) {
			break;
		}
	}
	fs_close(&fp);
	close(data);
	ftp_send(sess->ctrl, "226 Transfer complete");
}

static void handle_session(int ctrl)
{
	struct ftp_session sess = {
		.ctrl = ctrl,
		.authed = false,
		.anon = false,
		.data_listen = -1,
	};

	ftp_send(ctrl, "220 io-edge-hub FTP service ready");

	char line[FTP_BUF_SIZE];

	while (recv_line(ctrl, line, sizeof(line)) > 0) {
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

		/* 大写化命令 */
		for (char *p = cmd; *p; p++) {
			if (*p >= 'a' && *p <= 'z') {
				*p -= 'a' - 'A';
			}
		}

		LOG_DBG("FTP cmd: %s %s", cmd, arg);

		if (!strcmp(cmd, "USER")) {
			sess.anon = !strcmp(arg, "anonymous") || !strcmp(arg, "ftp");
			ftp_send(ctrl, "331 Please specify the password");
		} else if (!strcmp(cmd, "PASS")) {
			if (sess.anon ||
			    (!strcmp(arg, FTP_PASS))) {
				sess.authed = true;
				ftp_send(ctrl, "230 Login successful");
			} else {
				sess.authed = false;
				ftp_send(ctrl, "530 Login incorrect");
			}
		} else if (!strcmp(cmd, "SYST")) {
			ftp_send(ctrl, "215 UNIX Type: L8");
		} else if (!strcmp(cmd, "TYPE")) {
			ftp_send(ctrl, "200 Type set");
		} else if (!strcmp(cmd, "PWD")) {
			ftp_send(ctrl, "257 \"/\" is current directory");
		} else if (!strcmp(cmd, "CWD")) {
			ftp_send(ctrl, "250 OK");
		} else if (!strcmp(cmd, "PASV")) {
			cmd_pasv(&sess);
		} else if (!strcmp(cmd, "LIST")) {
			cmd_list(&sess, arg);
		} else if (!strcmp(cmd, "RETR")) {
			cmd_retr(&sess, arg);
		} else if (!strcmp(cmd, "STOR")) {
			cmd_stor(&sess, arg);
		} else if (!strcmp(cmd, "DELE")) {
			char fspath[FTP_BUF_SIZE];

			make_path(fspath, sizeof(fspath), arg);
			int rc = (!sess.authed || sess.anon) ? -1 : fs_unlink(fspath);

			ftp_send(ctrl, rc == 0 ? "250 Delete OK" : "550 Delete failed");
		} else if (!strcmp(cmd, "SIZE")) {
			char fspath[FTP_BUF_SIZE];
			struct fs_dirent ent;

			make_path(fspath, sizeof(fspath), arg);
			if (fs_stat(fspath, &ent) == 0 && ent.type == FS_DIR_ENTRY_FILE) {
				ftp_sendf(ctrl, "213 %u", (unsigned)ent.size);
			} else {
				ftp_send(ctrl, "550 Not found");
			}
		} else if (!strcmp(cmd, "QUIT")) {
			ftp_send(ctrl, "221 Goodbye");
			break;
		} else if (!strcmp(cmd, "NOOP")) {
			ftp_send(ctrl, "200 OK");
		} else {
			ftp_send(ctrl, "502 Command not implemented");
		}
	}

	if (sess.data_listen >= 0) {
		close(sess.data_listen);
	}
}

static void ftp_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* 等待 LittleFS 就绪 (历史文件存储) */
	(void)io_lfs_wait_ready(K_SECONDS(5));

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
	    listen(serv, 2) < 0) {
		LOG_ERR("FTP bind/listen failed: %d", errno);
		return;
	}

	LOG_INF("FTP server on port %d (root %s)", FTP_CTRL_PORT, FTP_ROOT);

	while (1) {
		int c = accept(serv, NULL, NULL);

		if (c < 0) {
			continue;
		}
		LOG_INF("FTP client connected");
		handle_session(c);
		close(c);
		LOG_INF("FTP client disconnected");
	}
}

K_THREAD_DEFINE(ftp, CONFIG_IO_FTP_STACK_SIZE, ftp_thread,
		NULL, NULL, NULL, CONFIG_IO_FTP_PRIORITY, 0, 0);
