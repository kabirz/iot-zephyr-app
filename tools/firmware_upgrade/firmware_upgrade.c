/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * io-edge-hub 固件升级 CLI (Linux C 实现)
 *
 * 与 Python 版 (firmware_upgrade.py) 功能对齐, 单文件无外部依赖.
 * 适合嵌入运维脚本/CI, 启动快零运行时开销.
 *
 * 协议:
 *   UDP: 端口 8600, FW_START(0x01)/FW_DATA(0x02)/FW_END(0x03), 跨子网可用
 *   CAN: Linux SocketCAN, 帧 0x101-0x105 (keyhash/START/DATA/CONFIRM)
 *        0x106/0x107 = MCUboot 启动探测/响应 (bootloader 升级模式 -b)
 *
 * 用法:
 *   firmware_upgrade upgrade -i 192.168.12.101 -f app.signed.bin
 *   firmware_upgrade upgrade -c can0 -f app.signed.bin
 *   firmware_upgrade upgrade -c can0 -b -f app.signed.bin  (MCUboot 内升级)
 *   firmware_upgrade version -i 192.168.12.101
 *   firmware_upgrade version -c can0
 *
 * 退出码: 0=成功 1=镜像错误 2=通信失败 3=设备拒绝
 *
 * 构建: make  (或 gcc -O2 -Wall -o firmware_upgrade firmware_upgrade.c)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
/* Linux SocketCAN */
#include <linux/can.h>
#include <linux/can/raw.h>

/* ==================== 常量 (与固件 libs/ 对齐) ==================== */

#define UDP_FW_PORT_DEFAULT        8600
#define UDP_TIMEOUT_MS             2000
#define UDP_FW_START_TIMEOUT_MS    5000
#define UDP_FW_END_TIMEOUT_MS      10000
#define UDP_CHUNK_SIZE             511
#define UDP_CHUNK_SIZE_V2_MAX      1400  /* DATA_V2 上位机上限 (实际取设备协商值) */
#define UDP_FW_WINDOW              8     /* DATA_V2 go-back-N 窗口帧数 */
#define UDP_FW_V2_ACK_TIMEOUT_MS   1000  /* 窗口级 ACK 超时 (覆盖渐进擦除的扇区擦停顿 ~400ms) */
#define UDP_FW_V2_MAX_RETRIES      8     /* 单窗口停滞重试上限 */

/* UDP 固件命令码 (与 libs/udp_fw_upgrade 对齐) */
#define UDP_FW_CMD_START           0x01
#define UDP_FW_CMD_DATA            0x02
#define UDP_FW_CMD_END             0x03
#define UDP_FW_CMD_GET_VERSION     0x04
#define UDP_FW_CMD_REBOOT          0x05
#define UDP_FW_CMD_DATA_V2         0x06

#define CAN_ID_FW_CMD              0x101
#define CAN_ID_FW_REPLY            0x102
#define CAN_ID_FW_DATA             0x103
#define CAN_ID_FW_KEYHASH          0x104
#define CAN_ID_FW_VERSION          0x105
#define CAN_ID_FW_BOOT_PROBE       0x106
#define CAN_ID_FW_BOOT_ACK         0x107

#define CAN_FW_CMD_START_UPDATE    0
#define CAN_FW_CMD_CONFIRM         1
#define CAN_FW_CMD_VERSION         2
#define CAN_FW_CMD_REBOOT          3

#define CAN_FW_CODE_OFFSET         0
#define CAN_FW_CODE_UPDATE_SUCCESS 1
#define CAN_FW_CODE_VERSION        2
#define CAN_FW_CODE_CONFIRM        3
#define CAN_FW_CODE_FLASH_ERROR    4
#define CAN_FW_CODE_TRANSFER_ERROR 5
#define CAN_FW_CODE_KEYHASH_ERROR  6

#define CAN_FW_CONFIRM_MAGIC       0x55AA55AAU
#define CAN_FRAME_TIMEOUT_MS       3000
#define CAN_KEYHASH_FRAMES         5
#define CAN_DATA_FRAME_PAYLOAD     8
#define CAN_OFFSET_REPLY_INTERVAL  8

/* MCUboot 启动探测帧 (与 libs/can_fw_upgrade/can_fw_upgrade_internal.h 对齐) */
#define CAN_FW_BOOT_PROBE_MAGIC    0x42544F31U  /* 'B''T''O''1' */
#define CAN_BOOT_PROBE_WAIT_MS     5000
#define CAN_BOOT_ACK_BYTE          0x5A

/* MCUboot 镜像 */
#define IMG_MAGIC                  0x96F3B83DU
#define IMG_TLV_INFO_MAGIC         0x6907
#define IMG_TLV_KEYHASH            0x01
#define IMG_KEYHASH_LEN            32

/* 退出码 */
#define EXIT_OK             0
#define EXIT_IMAGE_ERR      1
#define EXIT_COMM_ERR       2
#define EXIT_DEVICE_REJECT  3

/* ==================== 类型 ==================== */

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

struct fw_image {
	u8 *data;
	size_t size;
	u8 keyhash[IMG_KEYHASH_LEN];
	int has_keyhash;
};

struct progress {
	size_t total;
	const char *label;
	int last_pct;
};

/* ==================== 错误处理 ==================== */

static void die(int code, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(code);
}

/* ==================== 字节序辅助 (LE) ==================== */

static u16 rd_le16(const u8 *p)
{
	return (u16)(p[0] | (p[1] << 8));
}

static u32 rd_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void wr_le16(u8 *p, u16 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}

static void wr_le32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

/* ==================== CRC16-CCITT ==================== */

/* 与 Zephyr crc16_ccitt (subsys/crc/crc16_sw.c) 一致:
 * poly 0x1021, init 0x0000, bit-reflected 变体. */
static u16 crc16_ccitt(const u8 *data, size_t len)
{
	u16 seed = 0x0000;

	while (len-- > 0) {
		u8 e = (u8)seed ^ *data++;
		u8 f = (u8)(e ^ (e << 4));

		seed = (u16)((seed >> 8) ^ ((u16)f << 8) ^ ((u16)f << 3) ^ (f >> 4));
	}
	return seed;
}

/* ==================== 进度条 ==================== */

static void progress_init(struct progress *p, size_t total, const char *label)
{
	p->total = total ? total : 1;
	p->label = label ? label : "";
	p->last_pct = -1;
}

static void progress_update(struct progress *p, size_t current)
{
	int pct = (int)(current * 100 / p->total);
	int width = 40;
	int filled;
	int i;

	if (pct == p->last_pct) {
		return;
	}
	if (pct > 100) {
		pct = 100;
	}
	p->last_pct = pct;
	filled = width * pct / 100;
	fprintf(stderr, "\r%s [", p->label);
	for (i = 0; i < filled; i++) {
		fputc('=', stderr);
	}
	for (i = filled; i < width; i++) {
		fputc(' ', stderr);
	}
	fprintf(stderr, "] %3d%%", pct);
	fflush(stderr);
	if (pct >= 100) {
		fputc('\n', stderr);
	}
}

static void progress_message(struct progress *p, const char *msg)
{
	/* 未完成的进度条先换行, 避免消息粘连 */
	if (p->last_pct >= 0 && p->last_pct < 100) {
		fputc('\n', stderr);
	}
	p->last_pct = -1;
	printf("%s\n", msg);
	fflush(stdout);
}

/* ==================== 镜像解析 ==================== */

static int parse_image(const char *path, struct fw_image *img, int require_keyhash)
{
	FILE *f = fopen(path, "rb");
	long sz;
	u8 *data;
	u16 hdr_size;
	u32 img_size;
	size_t tlv_off;
	size_t off;

	if (!f) {
		fprintf(stderr, "文件不存在或不可读: %s: %s\n", path, strerror(errno));
		return EXIT_IMAGE_ERR;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		fprintf(stderr, "fseek 失败\n");
		return EXIT_IMAGE_ERR;
	}
	sz = ftell(f);
	if (sz < 32) {
		fclose(f);
		fprintf(stderr, "文件过短 (%ldB), 不像 MCUboot 镜像\n", sz);
		return EXIT_IMAGE_ERR;
	}
	rewind(f);
	data = malloc((size_t)sz);
	if (!data) {
		fclose(f);
		die(EXIT_IMAGE_ERR, "内存不足");
	}
	if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
		fclose(f);
		free(data);
		fprintf(stderr, "读取失败: %s\n", path);
		return EXIT_IMAGE_ERR;
	}
	fclose(f);

	if (rd_le32(data) != IMG_MAGIC) {
		free(data);
		fprintf(stderr, "magic 不匹配: 期望 0x%08X (非 MCUboot 镜像)\n", IMG_MAGIC);
		return EXIT_IMAGE_ERR;
	}
	hdr_size = rd_le16(data + 8);
	img_size = rd_le32(data + 12);
	if (hdr_size < 32 || (hdr_size & 0x3) || img_size > (u32)sz) {
		free(data);
		fprintf(stderr, "hdr_size(%u)/img_size(%u) 异常\n", hdr_size, img_size);
		return EXIT_IMAGE_ERR;
	}
	tlv_off = (size_t)hdr_size + (size_t)img_size;
	if (tlv_off + 4 > (size_t)sz) {
		free(data);
		fprintf(stderr, "TLV 区起始越界, 镜像可能被截断\n");
		return EXIT_IMAGE_ERR;
	}
	if (rd_le16(data + tlv_off) != IMG_TLV_INFO_MAGIC) {
		free(data);
		fprintf(stderr, "TLV info magic 不匹配: 期望 0x%04X\n", IMG_TLV_INFO_MAGIC);
		return EXIT_IMAGE_ERR;
	}

	/* 找 KEYHASH TLV */
	img->has_keyhash = 0;
	off = tlv_off + 4;
	while (off + 4 <= (size_t)sz) {
		u16 tp = rd_le16(data + off);
		u16 tlv_len = rd_le16(data + off + 2);

		if (tp == 0 || tlv_len == 0 || off + 4 + tlv_len > (size_t)sz) {
			break;
		}
		if (tp == IMG_TLV_KEYHASH && tlv_len == IMG_KEYHASH_LEN) {
			memcpy(img->keyhash, data + off + 4, IMG_KEYHASH_LEN);
			img->has_keyhash = 1;
			break;
		}
		off += 4 + tlv_len;
	}

	if (require_keyhash && !img->has_keyhash) {
		free(data);
		fprintf(stderr, "镜像无 KEYHASH TLV (用 --no-keyhash 跳过校验)\n");
		return EXIT_IMAGE_ERR;
	}

	img->data = data;
	img->size = (size_t)sz;
	return EXIT_OK;
}

static void free_image(struct fw_image *img)
{
	if (img->data) {
		free(img->data);
		img->data = NULL;
	}
}

/* ==================== UDP 升级通道 ==================== */

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

struct udp_ctx {
	int sock;
	struct sockaddr_in dst;
};

static int udp_open(struct udp_ctx *u, const char *ip, int port)
{
	u->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (u->sock < 0) {
		return -errno;
	}
	memset(&u->dst, 0, sizeof(u->dst));
	u->dst.sin_family = AF_INET;
	u->dst.sin_port = htons((u16)port);
	if (inet_pton(AF_INET, ip, &u->dst.sin_addr) != 1) {
		close(u->sock);
		u->sock = -1;
		return -EINVAL;
	}
	return 0;
}

static void udp_close(struct udp_ctx *u)
{
	if (u->sock >= 0) {
		close(u->sock);
		u->sock = -1;
	}
}

/* 发 [cmd][payload], 等回复. 返回回复总长度 (含 cmd echo 字节), <0 = 错误.
 * reply[0] 已校验 == cmd. */
static int udp_send_recv(struct udp_ctx *u, u8 cmd, const u8 *payload, int payload_len,
			 u8 *reply, int max_reply_len, int timeout_ms)
{
	u8 req[1 + 512];
	struct timeval tv;
	socklen_t fl;
	int n;

	if (payload_len < 0 || payload_len > 512) {
		return -EINVAL;
	}
	req[0] = cmd;
	if (payload_len > 0) {
		memcpy(req + 1, payload, payload_len);
	}

	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	if (setsockopt(u->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		return -errno;
	}

	if (sendto(u->sock, req, 1 + payload_len, 0,
		   (struct sockaddr *)&u->dst, sizeof(u->dst)) < 0) {
		return -errno;
	}

	fl = sizeof(u->dst);
	n = recvfrom(u->sock, reply, max_reply_len, 0,
		     (struct sockaddr *)&u->dst, &fl);
	if (n < 0) {
		return -errno;
	}
	if (n < 1 || reply[0] != cmd) {
		return -EIO;
	}
	return n;
}

/* FW_DATA_V2 窗口 go-back-N 流式发送: 连发 UDP_FW_WINDOW 帧不等回复,
 * 按回复中的期望 offset 推进; 超时/丢帧从最后确认处重传 (设备按 offset 去重). */
static int udp_fw_data_v2_stream(struct udp_ctx *u, const u8 *data, size_t total,
				 int chunk, struct progress *prog)
{
	u8 frame[1 + 4 + UDP_CHUNK_SIZE_V2_MAX];
	u8 reply[64];
	struct timeval tv;
	size_t off = 0;
	int retries = 0;

	while (off < total) {
		size_t win_end = off + (size_t)UDP_FW_WINDOW * chunk;
		size_t w, confirmed;
		long deadline;

		if (win_end > total) {
			win_end = total;
		}

		/* 发送一个窗口 [off, win_end) */
		w = off;
		while (w < win_end) {
			size_t n = (win_end - w > (size_t)chunk) ? (size_t)chunk : win_end - w;

			frame[0] = UDP_FW_CMD_DATA_V2;
			wr_le32(frame + 1, (u32)w);
			memcpy(frame + 5, data + w, n);
			if (sendto(u->sock, frame, 5 + n, 0,
				   (struct sockaddr *)&u->dst, sizeof(u->dst)) < 0) {
				return -errno;
			}
			w += n;
		}

		/* 收窗口内 ACK, 追踪最大确认 offset (回复始终是设备期望 offset) */
		deadline = now_ms() + UDP_FW_V2_ACK_TIMEOUT_MS;
		confirmed = off;
		while (confirmed < win_end) {
			long remain = deadline - now_ms();
			socklen_t fl = sizeof(u->dst);
			u32 roff;
			int n;

			if (remain <= 0) {
				break;
			}
			tv.tv_sec = remain / 1000;
			tv.tv_usec = (remain % 1000) * 1000;
			if (setsockopt(u->sock, SOL_SOCKET, SO_RCVTIMEO,
				       &tv, sizeof(tv)) < 0) {
				return -errno;
			}
			n = recvfrom(u->sock, reply, sizeof(reply), 0,
				     (struct sockaddr *)&u->dst, &fl);
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK ||
				    errno == EINTR) {
					break;
				}
				return -errno;
			}
			if (n >= 5 && reply[0] == UDP_FW_CMD_DATA_V2) {
				roff = (u32)reply[1] | ((u32)reply[2] << 8) |
				       ((u32)reply[3] << 16) | ((u32)reply[4] << 24);
				if (roff > confirmed) {
					confirmed = roff > total ? total : roff;
					retries = 0;  /* 有推进即重置停滞计数 */
				}
			}
		}
		progress_update(prog, confirmed);

		if (confirmed >= win_end) {
			off = confirmed;
			continue;
		}
		/* 窗口未完全确认: 从确认处 go-back-N 重传 (重复帧设备自动丢弃) */
		retries++;
		if (retries > UDP_FW_V2_MAX_RETRIES) {
			fprintf(stderr, "\nFW_DATA_V2 窗口重试超限 (offset=%zu, "
				"设备停滞或链路中断)\n", confirmed);
			return -ETIMEDOUT;
		}
		off = confirmed;
	}
	return 0;
}

static int udp_get_version(const char *ip, int port, char *out, int out_cap)
{
	struct udp_ctx u;
	int rc;
	u8 reply[128];
	int n;
	int len;

	rc = udp_open(&u, ip, port);
	if (rc < 0) {
		fprintf(stderr, "UDP 打开失败: %s\n", strerror(-rc));
		return EXIT_COMM_ERR;
	}
	n = udp_send_recv(&u, UDP_FW_CMD_GET_VERSION, NULL, 0, reply, sizeof(reply), UDP_TIMEOUT_MS);
	udp_close(&u);
	if (n < 0) {
		fprintf(stderr, "GET_VERSION 失败: %s\n", strerror(-n));
		return EXIT_COMM_ERR;
	}
	if (n < 2) {
		fprintf(stderr, "GET_VERSION 回复过短\n");
		return EXIT_COMM_ERR;
	}
	len = n - 1;
	if (len > out_cap - 1) {
		len = out_cap - 1;
	}
	memcpy(out, reply + 1, len);
	out[len] = 0;
	return EXIT_OK;
}

static int upgrade_udp(const char *ip, int port, const char *file,
		       int test_mode, int no_keyhash)
{
	struct fw_image img = { 0 };
	struct progress prog = { 0 };
	struct udp_ctx u;
	int rc;
	u8 reply[64];
	int n;
	u32 off;
	u32 v2_chunk = 0;
	u16 crc;

	rc = parse_image(file, &img, !no_keyhash);
	if (rc != EXIT_OK) {
		return rc;
	}
	progress_init(&prog, 1, "");
	progress_message(&prog, "");
	printf("镜像: %s (%zuB)  keyhash=%s\n", file, img.size,
	       no_keyhash ? "跳过" : (img.has_keyhash ? "有" : "无"));

	rc = udp_open(&u, ip, port);
	if (rc < 0) {
		fprintf(stderr, "UDP 打开失败: %s\n", strerror(-rc));
		rc = EXIT_COMM_ERR;
		goto out_free;
	}

	/* [1/4] FW_START */
	progress_message(&prog, "[1/4] FW_START (擦写 slot1, 请稍候 ~5s)...");
	{
		u8 pl[4 + IMG_KEYHASH_LEN];
		int plen = 4;

		wr_le32(pl, (u32)img.size);
		if (!no_keyhash && img.has_keyhash) {
			memcpy(pl + 4, img.keyhash, IMG_KEYHASH_LEN);
			plen += IMG_KEYHASH_LEN;
		}
		n = udp_send_recv(&u, UDP_FW_CMD_START, pl, plen, reply, sizeof(reply),
				  UDP_FW_START_TIMEOUT_MS);
	}
	if (n < 0) {
		fprintf(stderr, "\nFW_START 失败: %s\n", strerror(-n));
		rc = EXIT_COMM_ERR;
		goto out_close;
	}
	if (n < 2) {
		fprintf(stderr, "\nFW_START 回复过短\n");
		rc = EXIT_COMM_ERR;
		goto out_close;
	}
	if (reply[1] == 2) {
		fprintf(stderr, "\n设备拒绝: keyhash 不匹配\n");
		rc = EXIT_DEVICE_REJECT;
		goto out_close;
	}
	if (reply[1] != 1) {
		fprintf(stderr, "\n设备拒绝 FW_START (status=%u)\n", reply[1]);
		rc = EXIT_DEVICE_REJECT;
		goto out_close;
	}
	/* 新固件回复带 [v2_chunk 2B]: 协商 DATA_V2 单帧大小 (老固件 → 0, 停等模式) */
	v2_chunk = (n >= 4) ? ((u32)reply[2] | ((u32)reply[3] << 8)) : 0;
	if (v2_chunk > UDP_CHUNK_SIZE_V2_MAX) {
		v2_chunk = UDP_CHUNK_SIZE_V2_MAX;
	}
	progress_message(&prog, "[1/4] FW_START OK");

	/* [2/4] FW_DATA (设备支持 V2 走窗口流水线, 否则回退停等) */
	if (v2_chunk >= 512) {
		printf("[2/4] FW_DATA_V2 发送 %zuB (窗口 %d x %uB)...\n",
		       img.size, UDP_FW_WINDOW, v2_chunk);
		progress_init(&prog, img.size, "      数据");
		rc = udp_fw_data_v2_stream(&u, img.data, img.size, (int)v2_chunk, &prog);
		if (rc < 0) {
			fprintf(stderr, "\nFW_DATA_V2 失败: %s\n", strerror(-rc));
			rc = EXIT_COMM_ERR;
			goto out_close;
		}
		goto data_done;
	}
	printf("[2/4] FW_DATA 发送 %zuB (停等 511B)...\n", img.size);
	progress_init(&prog, img.size, "      数据");
	off = 0;
	while (off < img.size) {
		u32 chunk = (img.size - off > UDP_CHUNK_SIZE)
				    ? UDP_CHUNK_SIZE
				    : (u32)(img.size - off);
		u8 pl[1 + UDP_CHUNK_SIZE];

		pl[0] = 0x02;
		memcpy(pl + 1, img.data + off, chunk);
		n = udp_send_recv(&u, UDP_FW_CMD_DATA, pl + 1, chunk, reply, sizeof(reply),
				  UDP_TIMEOUT_MS);
		if (n < 0) {
			fprintf(stderr, "\nFW_DATA 失败 (offset=%u): %s\n",
				off, strerror(-n));
			rc = EXIT_COMM_ERR;
			goto out_close;
		}
		if (n < 5) {
			fprintf(stderr, "\nFW_DATA 回复过短\n");
			rc = EXIT_COMM_ERR;
			goto out_close;
		}
		off += chunk;
		progress_update(&prog, off);
	}
data_done:
	progress_message(&prog, "[2/4] FW_DATA 完成");

	/* [3/4] FW_END */
	crc = crc16_ccitt(img.data, img.size);
	printf("[3/4] FW_END (CRC=0x%04X, flush + 读回校验 ~10s)...\n", crc);
	{
		u8 pl[3];

		pl[0] = test_mode ? 1 : 0;
		wr_le16(pl + 1, crc);
		n = udp_send_recv(&u, UDP_FW_CMD_END, pl, 3, reply, sizeof(reply),
				  UDP_FW_END_TIMEOUT_MS);
	}
	if (n < 0) {
		fprintf(stderr, "\nFW_END 失败: %s\n", strerror(-n));
		rc = EXIT_COMM_ERR;
		goto out_close;
	}
	if (n < 2 || reply[1] != 1) {
		fprintf(stderr, "\nFW_END 失败 (result=%u, CRC 不匹配或写 flash 失败)\n",
			n >= 2 ? reply[1] : 0);
		rc = EXIT_DEVICE_REJECT;
		goto out_close;
	}
	progress_message(&prog, "[3/4] FW_END OK");

	/* [4/4] REBOOT: 设备回复后 100ms 冷重启, 无回复不视为失败 */
	printf("[4/4] REBOOT (触发 MCUboot 交换)...\n");
	n = udp_send_recv(&u, UDP_FW_CMD_REBOOT, NULL, 0, reply, sizeof(reply),
			  UDP_TIMEOUT_MS);
	if (n < 0) {
		printf("[4/4] REBOOT 已发送 (无回复, 设备可能已重启)\n");
	} else {
		printf("[4/4] REBOOT OK\n");
	}
	printf("升级完成 (%s), 等待设备重启...\n",
	       test_mode ? "测试模式" : "永久模式");
	rc = EXIT_OK;

out_close:
	udp_close(&u);
out_free:
	free_image(&img);
	return rc;
}

/* ==================== CAN 升级通道 (Linux SocketCAN) ==================== */

struct can_ctx {
	int sock;
};

static int can_open(struct can_ctx *c, const char *ifname)
{
	struct ifreq ifr;
	struct sockaddr_can addr;

	c->sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (c->sock < 0) {
		return -errno;
	}
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(c->sock, SIOCGIFINDEX, &ifr) < 0) {
		int e = errno;

		close(c->sock);
		c->sock = -1;
		return -e;
	}
	memset(&addr, 0, sizeof(addr));
	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;
	if (bind(c->sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int e = errno;

		close(c->sock);
		c->sock = -1;
		return -e;
	}
	return 0;
}

static void can_close(struct can_ctx *c)
{
	if (c->sock >= 0) {
		close(c->sock);
		c->sock = -1;
	}
}

static int can_send(struct can_ctx *c, u32 id, const u8 *data, int len)
{
	struct can_frame fr;
	int rc;

	if (len < 0 || len > 8) {
		return -EINVAL;
	}
	memset(&fr, 0, sizeof(fr));
	fr.can_id = id;
	fr.can_dlc = (u8)len;
	if (len > 0) {
		memcpy(fr.data, data, len);
	}
	/* EAGAIN = 内核 TX 队列满 (总线拥塞), 小睡重试 */
	do {
		rc = write(c->sock, &fr, sizeof(fr));
		if (rc == (ssize_t)sizeof(fr)) {
			return 0;
		}
		if (rc < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			return -errno;
		}
		usleep(1000);
	} while (1);
}

/* 等下一帧. expect_id=0 表示接受任意 ID. 返回 0=成功, <0=错误/超时. */
static int can_recv(struct can_ctx *c, u32 expect_id, struct can_frame *out,
		    int timeout_ms)
{
	struct timeval tv;

	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	if (setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		return -errno;
	}
	for (;;) {
		struct can_frame fr;
		ssize_t n = read(c->sock, &fr, sizeof(fr));

		if (n < 0) {
			return -errno;
		}
		if (n < (ssize_t)sizeof(fr)) {
			continue;
		}
		if (fr.can_id & (CAN_EFF_FLAG | CAN_ERR_FLAG | CAN_RTR_FLAG)) {
			continue;
		}
		if (expect_id != 0 && fr.can_id != expect_id) {
			continue;
		}
		*out = fr;
		return 0;
	}
}

static void can_flush_rx(struct can_ctx *c, int duration_ms)
{
	/* 简单策略: 每轮 50ms 超时, duration_ms 决定轮数; 任意一轮超时则停止 */
	struct timeval tv = { .tv_sec = 0, .tv_usec = 50 * 1000 };
	struct can_frame fr;
	int rounds = duration_ms / 50 + 1;

	setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	while (rounds-- > 0) {
		if (read(c->sock, &fr, sizeof(fr)) < 0) {
			break; /* 超时, 缓冲清空 */
		}
	}
}

static int can_send_fw_cmd(struct can_ctx *c, u32 cmd, u32 arg)
{
	u8 data[8];

	wr_le32(data, cmd);
	wr_le32(data + 4, arg);
	return can_send(c, CAN_ID_FW_CMD, data, 8);
}

static int can_wait_reply(struct can_ctx *c, u32 *out_code, u32 *out_arg,
			  int timeout_ms)
{
	struct can_frame fr;
	int rc = can_recv(c, CAN_ID_FW_REPLY, &fr, timeout_ms);

	if (rc < 0) {
		return rc;
	}
	if (fr.can_dlc < 8) {
		return -EIO;
	}
	*out_code = rd_le32(fr.data);
	*out_arg = rd_le32(fr.data + 4);
	return 0;
}

static int can_get_version(const char *ifname, char *out, int out_cap)
{
	struct can_ctx c;
	int rc;
	u32 code, arg;
	int total_len;
	int expected;
	struct can_frame fr;
	u8 ver[64];
	int ver_len = 0;
	int i;

	rc = can_open(&c, ifname);
	if (rc < 0) {
		fprintf(stderr, "CAN 打开失败 (%s): %s\n", ifname, strerror(-rc));
		return EXIT_COMM_ERR;
	}
	can_flush_rx(&c, 200);
	rc = can_send_fw_cmd(&c, CAN_FW_CMD_VERSION, 0);
	if (rc < 0) {
		fprintf(stderr, "VERSION 命令发送失败: %s\n", strerror(-rc));
		can_close(&c);
		return EXIT_COMM_ERR;
	}
	rc = can_wait_reply(&c, &code, &arg, CAN_FRAME_TIMEOUT_MS);
	if (rc < 0) {
		fprintf(stderr, "VERSION 等回复超时\n");
		can_close(&c);
		return EXIT_COMM_ERR;
	}
	if (code != CAN_FW_CODE_VERSION) {
		fprintf(stderr, "VERSION 回复 code 异常: %u\n", code);
		can_close(&c);
		return EXIT_COMM_ERR;
	}
	total_len = (int)arg;
	if (total_len <= 0 || total_len > 63) {
		fprintf(stderr, "VERSION 长度异常: %d\n", total_len);
		can_close(&c);
		return EXIT_COMM_ERR;
	}
	expected = (total_len + 6) / 7;
	for (i = 0; i < expected; i++) {
		rc = can_recv(&c, CAN_ID_FW_VERSION, &fr, CAN_FRAME_TIMEOUT_MS);
		if (rc < 0) {
			fprintf(stderr, "VERSION 分片 %d 超时\n", i);
			can_close(&c);
			return EXIT_COMM_ERR;
		}
		int chunk = fr.can_dlc > 1 ? fr.can_dlc - 1 : 0;

		if (chunk > 0 && ver_len + chunk < (int)sizeof(ver)) {
			memcpy(ver + ver_len, fr.data + 1, chunk);
			ver_len += chunk;
		}
	}
	can_close(&c);
	ver[ver_len] = 0;
	/* 截到 NUL */
	for (i = 0; i < ver_len; i++) {
		if (ver[i] == 0) {
			ver_len = i;
			break;
		}
	}
	if (ver_len > out_cap - 1) {
		ver_len = out_cap - 1;
	}
	memcpy(out, ver, ver_len);
	out[ver_len] = 0;
	return EXIT_OK;
}

/* bootloader 升级模式 (-b):
 * 1. 给运行中的设备 (应用同样实现本协议) 发 REBOOT, 令其重启进 MCUboot;
 *    设备已在 bootloader 等待中时该帧触发重新启动 → 重新探测。
 * 2. 等 MCUboot 的 0x106 探测帧 (设备重启后 500ms 窗口内多次发送)。
 * 3. 收到后立即回 0x107, 设备随即进入固件升级等待 (15s 空闲超时),
 *    后续 START/DATA/CONFIRM 全部由 bootloader 应答。 */
static int can_enter_boot(struct can_ctx *c)
{
	struct can_frame fr;
	long deadline;
	int rc;

	can_flush_rx(c, 200);
	printf("      REBOOT 已发送 (等待设备进 bootloader)...\n");
	can_send_fw_cmd(c, CAN_FW_CMD_REBOOT, 0);

	deadline = now_ms() + CAN_BOOT_PROBE_WAIT_MS;
	for (;;) {
		long remain = deadline - now_ms();

		if (remain <= 0) {
			fprintf(stderr, "未收到 bootloader 探测帧 (0x106), 设备未重启或未启用 CAN bootloader\n");
			return -ETIMEDOUT;
		}
		rc = can_recv(c, CAN_ID_FW_BOOT_PROBE, &fr, (int)remain);
		if (rc == 0) {
			break;
		}
	}

	if (fr.can_dlc < 8 || rd_le32(fr.data) != CAN_FW_BOOT_PROBE_MAGIC) {
		fprintf(stderr, "探测帧格式异常 (dlc=%u)\n", fr.can_dlc);
		return -EIO;
	}
	printf("      bootloader 就绪: v%u.%u.%u\n",
	       fr.data[4], fr.data[5], fr.data[6]);

	/* 立即应答 (设备探测窗口 500ms) */
	{
		u8 ack = CAN_BOOT_ACK_BYTE;

		return can_send(c, CAN_ID_FW_BOOT_ACK, &ack, 1);
	}
}

static int can_send_keyhash(struct can_ctx *c, const u8 *keyhash)
{
	int seq;

	for (seq = 0; seq < CAN_KEYHASH_FRAMES; seq++) {
		u8 data[8];
		int rc;

		data[0] = (u8)seq;
		memcpy(data + 1, keyhash + seq * 7, 7);
		rc = can_send(c, CAN_ID_FW_KEYHASH, data, 8);
		if (rc < 0) {
			return rc;
		}
		usleep(5000); /* 5ms, 给固件 RX 线程喘息 */
	}
	return 0;
}

static int can_start_update(struct can_ctx *c, u32 img_size)
{
	int rc;
	u32 code, arg;

	rc = can_send_fw_cmd(c, CAN_FW_CMD_START_UPDATE, img_size);
	if (rc < 0) {
		return rc;
	}
	/* 擦 slot1 耗时, 给长超时 */
	rc = can_wait_reply(c, &code, &arg, 10000);
	if (rc < 0) {
		return rc;
	}
	if (code == CAN_FW_CODE_KEYHASH_ERROR) {
		fprintf(stderr, "设备拒绝: keyhash 不匹配\n");
		return -EACCES;
	}
	if (code == CAN_FW_CODE_FLASH_ERROR) {
		fprintf(stderr, "设备拒绝: flash 擦除失败 (arg=%u)\n", arg);
		return -EIO;
	}
	if (code != CAN_FW_CODE_OFFSET) {
		fprintf(stderr, "START_UPDATE 意外回复: code=%u arg=%u\n", code, arg);
		return -EIO;
	}
	if (arg != 0) {
		fprintf(stderr, "START_UPDATE 初始 offset 非 0: %u\n", arg);
		return -EIO;
	}
	return 0;
}

static int can_send_data(struct can_ctx *c, const u8 *data, size_t len,
			 size_t total, struct progress *prog)
{
	u8 frame_data[CAN_DATA_FRAME_PAYLOAD];
	size_t off = 0;
	int seq_in_block = 0;

	/* 主循环: 每 8B 一帧, 末尾不足 8B 的留到 while 后单独处理 */

	while (off + CAN_DATA_FRAME_PAYLOAD <= len) {
		int rc;
		u32 code, arg;

		memcpy(frame_data, data + off, CAN_DATA_FRAME_PAYLOAD);
		rc = can_send(c, CAN_ID_FW_DATA, frame_data, CAN_DATA_FRAME_PAYLOAD);
		if (rc < 0) {
			return rc;
		}
		off += CAN_DATA_FRAME_PAYLOAD;
		seq_in_block++;
		progress_update(prog, off < total ? off : total);

		if (seq_in_block >= CAN_OFFSET_REPLY_INTERVAL || off >= len) {
			rc = can_wait_reply(c, &code, &arg, 5000);
			if (rc < 0) {
				return rc;
			}
			if (code == CAN_FW_CODE_FLASH_ERROR) {
				fprintf(stderr, "\nFW_DATA flash 写失败 (arg=%u)\n", arg);
				return -EIO;
			}
			if (code == CAN_FW_CODE_TRANSFER_ERROR) {
				fprintf(stderr, "\nFW_DATA 传输错误 (arg=%u)\n", arg);
				return -EIO;
			}
			if (code == CAN_FW_CODE_UPDATE_SUCCESS) {
				/* 全部写完, 设备确认 */
				return 0;
			}
			if (code != CAN_FW_CODE_OFFSET) {
				fprintf(stderr, "\nFW_DATA 意外回复: code=%u arg=%u\n",
					code, arg);
				return -EIO;
			}
			seq_in_block = 0;
		}
	}

	/* 不足 8B 的末尾用实际 DLC 发送: 补齐会使设备 fw_written 超过总数,
	 * 不落流控边界导致设备不再回复 */
	if (off < len) {
		int rc;
		u32 code, arg;
		size_t remain = len - off;

		rc = can_send(c, CAN_ID_FW_DATA, data + off, (int)remain);
		if (rc < 0) {
			return rc;
		}
		progress_update(prog, total);
		rc = can_wait_reply(c, &code, &arg, 5000);
		if (rc < 0) {
			return rc;
		}
		if (code != CAN_FW_CODE_UPDATE_SUCCESS && code != CAN_FW_CODE_OFFSET) {
			fprintf(stderr, "\nFW_DATA 末帧意外回复: code=%u\n", code);
			return -EIO;
		}
	}

	return 0;
}

static int can_confirm(struct can_ctx *c, int permanent)
{
	int rc;
	u32 code, arg;

	rc = can_send_fw_cmd(c, CAN_FW_CMD_CONFIRM, permanent ? 1 : 0);
	if (rc < 0) {
		return rc;
	}
	rc = can_wait_reply(c, &code, &arg, 5000);
	if (rc < 0) {
		return rc;
	}
	if (code == CAN_FW_CODE_TRANSFER_ERROR) {
		fprintf(stderr, "CONFIRM 传输错误 (arg=%u)\n", arg);
		return -EIO;
	}
	if (code != CAN_FW_CODE_CONFIRM) {
		fprintf(stderr, "CONFIRM 意外回复: code=%u arg=%u\n", code, arg);
		return -EIO;
	}
	if (arg != CAN_FW_CONFIRM_MAGIC) {
		fprintf(stderr, "CONFIRM magic 不匹配: 0x%08X\n", arg);
		return -EIO;
	}
	return 0;
}

static int upgrade_can(const char *ifname, const char *file,
		       int test_mode, int no_keyhash, int boot_mode)
{
	struct fw_image img = { 0 };
	struct progress prog = { 0 };
	struct can_ctx c;
	int rc;

	rc = parse_image(file, &img, !no_keyhash);
	if (rc != EXIT_OK) {
		return rc;
	}
	printf("镜像: %s (%zuB)  keyhash=%s\n", file, img.size,
	       no_keyhash ? "跳过" : (img.has_keyhash ? "有" : "无"));

	rc = can_open(&c, ifname);
	if (rc < 0) {
		fprintf(stderr, "CAN 打开失败 (%s): %s\n", ifname, strerror(-rc));
		rc = EXIT_COMM_ERR;
		goto out_free;
	}

	/* [0/4] bootloader 模式: 命令设备重启 → 应答 MCUboot 探测,
	 * 之后整个升级在 bootloader 内完成 (设备掉底牌也能升级) */
	if (boot_mode) {
		printf("[0/4] bootloader 模式: 等待 MCUboot 探测 (0x106)...\n");
		rc = can_enter_boot(&c);
		if (rc < 0) {
			fprintf(stderr, "进入 bootloader 失败: %s\n", strerror(-rc));
			rc = EXIT_COMM_ERR;
			goto out_close;
		}
		printf("[0/4] bootloader 已应答, 升级将在 bootloader 内进行\n");
	}

	/* [1/4] keyhash */
	if (img.has_keyhash && !no_keyhash) {
		printf("[1/4] 发送 keyhash (5 帧 0x104)...\n");
		rc = can_send_keyhash(&c, img.keyhash);
		if (rc < 0) {
			fprintf(stderr, "keyhash 发送失败: %s\n", strerror(-rc));
			rc = EXIT_COMM_ERR;
			goto out_close;
		}
		printf("      keyhash 5 帧已发送 (32B)\n");
		printf("[1/4] keyhash 已发送\n");
	} else {
		printf("[1/4] 跳过 keyhash (--no-keyhash)\n");
	}

	/* [2/4] START_UPDATE */
	printf("[2/4] START_UPDATE (size=%zuB, 擦 slot1 ~5s)...\n", img.size);
	rc = can_start_update(&c, (u32)img.size);
	if (rc < 0) {
		if (rc == -EACCES) {
			rc = EXIT_DEVICE_REJECT;
		} else {
			fprintf(stderr, "START_UPDATE 失败: %s\n", strerror(-rc));
			rc = EXIT_COMM_ERR;
		}
		goto out_close;
	}
	printf("[2/4] START_UPDATE OK\n");

	/* [3/4] 数据 */
	printf("[3/4] FW_DATA 发送 %zuB (8B/帧, 每 64B 流控)...\n", img.size);
	progress_init(&prog, img.size, "      数据");
	rc = can_send_data(&c, img.data, img.size, img.size, &prog);
	if (rc < 0) {
		fprintf(stderr, "\nFW_DATA 失败: %s\n", strerror(-rc));
		rc = EXIT_COMM_ERR;
		goto out_close;
	}
	printf("[3/4] FW_DATA 完成\n");

	/* [4/4] CONFIRM */
	printf("[4/4] CONFIRM (%s模式)...\n", test_mode ? "测试" : "永久");
	rc = can_confirm(&c, !test_mode);
	if (rc < 0) {
		fprintf(stderr, "CONFIRM 失败: %s\n", strerror(-rc));
		rc = EXIT_COMM_ERR;
		goto out_close;
	}
	printf("[4/4] CONFIRM OK (%s)\n", test_mode ? "测试模式" : "永久模式");

	/* CONFIRM 后设备在 bootloader 本会话内直接完成交换 (无重启,
	 * 约 30-40s); 普通模式需发 REBOOT 让设备重启触发交换 */
	if (boot_mode) {
		printf("      CONFIRM 完成, 设备在本会话内交换 (~30-40s), 等待新固件...\n");
	} else {
		printf("      REBOOT (触发设备重启)...\n");
		can_send_fw_cmd(&c, CAN_FW_CMD_REBOOT, 0);
	}
	usleep(100000);
	printf("升级完成 (%s), 等待设备重启...\n",
	       test_mode ? "测试模式" : "永久模式");
	rc = EXIT_OK;

out_close:
	can_close(&c);
out_free:
	free_image(&img);
	return rc;
}

/* ==================== CLI ==================== */

static void usage(const char *prog)
{
	fprintf(stderr,
		"io-edge-hub 固件升级 CLI (Linux)\n\n"
		"用法:\n"
		"  %s upgrade  -f <file> [-i <ip>|-c <can>] [选项]\n"
		"  %s version  [-i <ip>|-c <can>]\n\n"
		"通用选项:\n"
		"  -i <ip>       UDP 目标 IP (用 UDP 通道)\n"
		"  -p <port>     UDP 端口 (默认 %d)\n"
		"  -c <can>      SocketCAN 通道 (如 can0, 用 CAN 通道)\n"
		"  -f <file>     固件镜像路径 (upgrade 必填)\n"
		"  -b            bootloader 升级模式 (仅 CAN): 命令设备重启进\n"
		"                MCUboot 并应答其探测帧, 升级全程在 bootloader 内\n"
		"  --test        测试模式 (重启后回滚, 不永久)\n"
		"  --no-keyhash  跳过 keyhash 校验 (不推荐)\n"
		"  -h            显示帮助\n\n"
		"退出码: 0=成功 1=镜像错误 2=通信失败 3=设备拒绝\n",
		prog, prog, UDP_FW_PORT_DEFAULT);
}

/* 解析通用选项 (-i/-p/-c/-f/-b/--test/--no-keyhash).
 * 返回 0=成功, -1=错误 (已打印用法), 1=-h (要打印用法). */
static int parse_common_opts(int argc, char **argv,
			     const char **ip, int *port,
			     const char **can, const char **file,
			     int *test_mode, int *no_keyhash, int *boot_mode)
{
	/* 手动解析 (子命令后的参数), 不用 getopt 避免子命令 getopt 重置问题 */
	optind = 1;
	while (optind < argc) {
		const char *a = argv[optind];

		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			return 1;
		} else if (strcmp(a, "-i") == 0 && optind + 1 < argc) {
			*ip = argv[++optind];
		} else if (strcmp(a, "-p") == 0 && optind + 1 < argc) {
			*port = atoi(argv[++optind]);
		} else if (strcmp(a, "-c") == 0 && optind + 1 < argc) {
			*can = argv[++optind];
		} else if (strcmp(a, "-f") == 0 && optind + 1 < argc) {
			*file = argv[++optind];
		} else if (strcmp(a, "-b") == 0 || strcmp(a, "--boot") == 0) {
			*boot_mode = 1;
		} else if (strcmp(a, "--test") == 0) {
			*test_mode = 1;
		} else if (strcmp(a, "--no-keyhash") == 0) {
			*no_keyhash = 1;
		} else {
			fprintf(stderr, "未知参数: %s\n", a);
			return -1;
		}
		optind++;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *cmd;
	const char *ip = NULL;
	const char *can = NULL;
	const char *file = NULL;
	int port = UDP_FW_PORT_DEFAULT;
	int test_mode = 0;
	int no_keyhash = 0;
	int boot_mode = 0;
	int rc;

	if (argc < 2) {
		usage(argv[0]);
		return EXIT_IMAGE_ERR;
	}
	cmd = argv[1];

	if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
		usage(argv[0]);
		return EXIT_OK;
	}

	rc = parse_common_opts(argc - 1, argv + 1, &ip, &port, &can, &file,
			       &test_mode, &no_keyhash, &boot_mode);
	if (rc == 1) {
		usage(argv[0]);
		return EXIT_OK;
	}
	if (rc < 0) {
		usage(argv[0]);
		return EXIT_IMAGE_ERR;
	}
	if (boot_mode && !can) {
		fprintf(stderr, "-b bootloader 模式仅支持 CAN 通道 (需 -c <can>)\n");
		return EXIT_IMAGE_ERR;
	}

	if (strcmp(cmd, "version") == 0) {
		char ver[64];
		if (ip) {
			rc = udp_get_version(ip, port, ver, sizeof(ver));
			if (rc == EXIT_OK) {
				printf("UDP 版本: %s\n", ver);
			}
		} else if (can) {
			rc = can_get_version(can, ver, sizeof(ver));
			if (rc == EXIT_OK) {
				printf("CAN 版本: %s\n", ver);
			}
		} else {
			fprintf(stderr, "version 需要 -i <ip> 或 -c <can>\n");
			return EXIT_IMAGE_ERR;
		}
		return rc;
	}

	if (strcmp(cmd, "upgrade") == 0) {
		if (!file) {
			fprintf(stderr, "upgrade 必须指定 -f <file>\n");
			return EXIT_IMAGE_ERR;
		}
		if (ip) {
			return upgrade_udp(ip, port, file, test_mode, no_keyhash);
		} else if (can) {
			return upgrade_can(can, file, test_mode, no_keyhash, boot_mode);
		}
		fprintf(stderr, "upgrade 需要 -i <ip> (UDP) 或 -c <can> (CAN)\n");
		return EXIT_IMAGE_ERR;
	}

	fprintf(stderr, "未知子命令: %s (支持 upgrade / version)\n", cmd);
	usage(argv[0]);
	return EXIT_IMAGE_ERR;
}
