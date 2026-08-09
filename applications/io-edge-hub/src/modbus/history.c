/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * IO 历史记录: msgq 累积 + 系统工作队列 (k_work) 批量写
 *
 *   - DI/AI 采样线程 send_history_data() 入 msgq + 触发延迟 work (1s)
 *   - work handler 批量取 msgq, 写当前文件 (保持打开, fs_sync 批量 flush),
 *     多条记录一次同步, 大幅减少 Flash 写次数
 *   - 单文件 1MB 上限: 超过则关闭 + 新建 data_MMDD_HHMMSS.raw
 *   - 保留至多 10 个文件, 超限删最旧 (按名序 = 时间序)
 *   - DI 10B / AI 16B, 与 RT-Thread / PC 解析工具兼容
 *
 * 无独立线程: 复用系统工作队列 (CONFIG_SYSTEM_WORKQUEUE)。
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

#define HIST_DIR		"/lfs1"
#define HIST_MAX_FILES		10
#define HIST_FILE_MAX		(1024 * 1024)

K_MSGQ_DEFINE(his_msgq, sizeof(struct his_data), 16, 4);

static volatile bool history_enabled;
static struct fs_file_t his_fp;
static bool his_fp_open;
static uint32_t his_cur_size;

/* 删除最旧历史文件, 维持 <= HIST_MAX_FILES 个 */
static void cleanup_old_files(void)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	char names[HIST_MAX_FILES + 2][24];
	int n = 0;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, HIST_DIR) != 0) {
		return;
	}
	while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
		if (ent.type != FS_DIR_ENTRY_FILE ||
		    strncmp(ent.name, "data_", 5) != 0) {
			continue;
		}
		if (n < HIST_MAX_FILES + 2) {
			strncpy(names[n], ent.name, sizeof(names[0]) - 1);
			names[n][sizeof(names[0]) - 1] = '\0';
			n++;
		}
	}
	fs_closedir(&dir);

	while (n > HIST_MAX_FILES) {
		int min_i = 0;

		for (int i = 1; i < n; i++) {
			if (strcmp(names[i], names[min_i]) < 0) {
				min_i = i;
			}
		}
		char path[48];

		snprintf(path, sizeof(path), "%s/%s", HIST_DIR, names[min_i]);
		fs_unlink(path);
		LOG_INF("rotated out %s", names[min_i]);
		if (min_i != n - 1) {
			strcpy(names[min_i], names[n - 1]);
		}
		n--;
	}
}

static void make_hist_name(char *buf, size_t len)
{
	time_t t = time(NULL);
	struct tm *lt = gmtime(&t);

	snprintf(buf, len, "data_%02d%02d_%02d%02d%02d.raw",
		 lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
}

/* 确保当前文件可写: 未打开或超 1MB 时新建 (data_MMDD_HHMMSS.raw) */
static int ensure_file(void)
{
	if (his_fp_open && his_cur_size < HIST_FILE_MAX) {
		return 0;
	}
	if (his_fp_open) {
		fs_close(&his_fp);
		his_fp_open = false;
	}

	char path[48];

	make_hist_name(path, sizeof(path));
	fs_file_t_init(&his_fp);
	if (fs_open(&his_fp, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND) != 0) {
		return -EIO;
	}

	fs_seek(&his_fp, 0, FS_SEEK_END);
	his_cur_size = (uint32_t)fs_tell(&his_fp);
	if (his_cur_size == 0) {
		cleanup_old_files();
	}
	his_fp_open = true;
	LOG_INF("history file: %s", path);
	return 0;
}

/* 批量取 msgq 写当前文件; close_after=true 时写完关闭 */
static void his_flush(bool close_after)
{
	if (!io_lfs_is_ready()) {
		return;
	}

	struct his_data d;
	bool wrote = false;

	while (k_msgq_get(&his_msgq, &d, K_NO_WAIT) == 0) {
		if (ensure_file() != 0) {
			break;
		}
		size_t len = (d.type == DI_TYPE) ? 10U : 16U;

		if (fs_write(&his_fp, &d, len) == (ssize_t)len) {
			his_cur_size += len;
			wrote = true;
		}
	}

	if (wrote && his_fp_open) {
		fs_sync(&his_fp);
	}
	if (close_after && his_fp_open) {
		fs_close(&his_fp);
		his_fp_open = false;
	}
}

static void his_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	his_flush(!history_enabled);
}

K_WORK_DEFINE(his_work, his_work_handler);

void history_enable_write(bool en)
{
	history_enabled = en;
	LOG_INF("history %s", en ? "enabled" : "disabled");
	if (!en) {
		his_flush(true);
	}
}

void send_history_data(const struct his_data *data)
{
	if (!history_enabled || !io_lfs_is_ready()) {
		return;
	}
	if (k_msgq_put(&his_msgq, data, K_NO_WAIT) == 0) {
		/* 立即提交系统工作队列 (k_work 合并: 执行前重复提交只跑一次)。
		 * handler 批量取 msgq + 保持文件打开 + fs_sync, 减少 Flash 写 */
		k_work_submit(&his_work);
	}
}
