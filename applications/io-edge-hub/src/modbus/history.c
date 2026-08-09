/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * IO 历史记录: k_fifo 异步缓冲 + 独立 writer 线程写 LittleFS
 *
 *   - DI/AI 采样线程调 send_history_data() 入队 (非阻塞, 满则丢弃)
 *   - writer 线程从 fifo 取记录, 按 type 追加写当前文件
 *   - 文件按 data_MMDD_HHMM.raw 命名 (同分钟聚合), 维持至多 10 个, 超限删最旧
 *   - 单文件软上限 1MB (超过则切换到下一分钟文件)
 *   - 数据格式与 RT-Thread / PC 解析工具兼容 (DI 10B, AI 16B)
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <init.h>
#include "fs_littlefs.h"

LOG_MODULE_REGISTER(io_history, LOG_LEVEL_INF);

#define HIST_DIR	"/lfs1"
#define HIST_MAX_FILES	10
#define HIST_FILE_MAX	(1024 * 1024)

/* 历史记录消息队列 (替代 k_fifo + k_malloc: 定长池, 无堆碎片) */
K_MSGQ_DEFINE(his_msgq, sizeof(struct his_data), 16, 4);
static volatile bool history_enabled;
static char cur_name[64];	/* 当前文件名 data_MMDD_HHMM.raw */

void history_enable_write(bool en)
{
	history_enabled = en;
	LOG_INF("history %s", en ? "enabled" : "disabled");
}

void send_history_data(const struct his_data *data)
{
	if (!history_enabled) {
		return;
	}

	/* 非阻塞入队: msgq 满则丢弃最新记录, 不阻塞采样线程 */
	(void)k_msgq_put(&his_msgq, data, K_NO_WAIT);
}

/* 删除最旧历史文件, 维持 <= HIST_MAX_FILES 个 */
static void cleanup_old_files(void)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	char names[HIST_MAX_FILES + 2][sizeof(cur_name)];
	int n = 0;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, HIST_DIR) != 0) {
		return;
	}

	while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
		if (ent.type != FS_DIR_ENTRY_FILE) {
			continue;
		}
		if (strncmp(ent.name, "data_", 5) != 0) {
			continue;
		}
		if (n < HIST_MAX_FILES + 2) {
			strncpy(names[n], ent.name, sizeof(names[0]) - 1);
			names[n][sizeof(names[0]) - 1] = '\0';
			n++;
		}
	}
	fs_closedir(&dir);

	/* 文件名形如 data_MMDD_HHMM.raw, 字典序 = 时间序 */
	while (n > HIST_MAX_FILES) {
		int min_i = 0;

		for (int i = 1; i < n; i++) {
			if (strcmp(names[i], names[min_i]) < 0) {
				min_i = i;
			}
		}
		char path[40];

		snprintf(path, sizeof(path), "%s/%s", HIST_DIR, names[min_i]);
		fs_unlink(path);
		LOG_INF("rotated out %s", names[min_i]);
		/* 用最后一个覆盖被删项, 缩减计数 */
		names[min_i][0] = '\0';
		if (min_i != n - 1) {
			strcpy(names[min_i], names[n - 1]);
		}
		n--;
	}
}

static void write_record(const struct his_data *d)
{
	time_t t = time(NULL);
	struct tm *lt = gmtime(&t);
	char name[sizeof(cur_name)];

	snprintf(name, sizeof(name), "data_%02d%02d_%02d%02d.raw",
		 lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min);
	if (strcmp(name, cur_name) != 0) {
		/* 新文件: 触发轮转清理 */
		strcpy(cur_name, name);
		cleanup_old_files();
	}

	char path[80];
	struct fs_dirent ent;

	snprintf(path, sizeof(path), "%s/%s", HIST_DIR, cur_name);

	/* 单文件超 1MB: 强制切到下一文件名 (用序号后缀避免覆盖当前分钟) */
	if (fs_stat(path, &ent) == 0 && ent.type == FS_DIR_ENTRY_FILE &&
	    ent.size >= HIST_FILE_MAX) {
		snprintf(path, sizeof(path), "%s/%s.full", HIST_DIR, cur_name);
	}

	struct fs_file_t fp;

	fs_file_t_init(&fp);
	if (fs_open(&fp, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND) != 0) {
		return;
	}

	size_t rec_len = (d->type == DI_TYPE) ? 10U : 16U;

	fs_write(&fp, d, rec_len);
	fs_close(&fp);
}

static void his_writer_loop(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!io_lfs_wait_ready(K_SECONDS(5))) {
		LOG_ERR("LittleFS not ready, history disabled");
		return;
	}

	LOG_INF("history writer ready");

	while (1) {
		struct his_data d;

		k_msgq_get(&his_msgq, &d, K_FOREVER);
		write_record(&d);
	}
}

K_THREAD_DEFINE(history_writer, CONFIG_IO_HISTORY_WRITER_STACK_SIZE,
		his_writer_loop, NULL, NULL, NULL, 6, 0, 0);
