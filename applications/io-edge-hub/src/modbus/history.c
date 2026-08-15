/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * IO 历史记录: msgq 累积 + 专用工作队列 (k_work) 批量写
 *
 *   - DI/AI 采样线程 send_history_data() 入 msgq + 触发 work (合并去重)
 *   - 专用工作队列 (hist_work_q) handler 批量取 msgq, 写当前文件
 *     (保持打开, fs_sync 批量 flush), 多条记录一次同步, 减少 Flash 写次数
 *   - 单文件 1MB 上限: 超过则关闭 + 新建 data_MMDD_HHMMSS.raw
 *   - 保留至多 10 个文件, 超限删最旧 (按名序 = 时间序)
 *   - DI 10B / AI 16B, 与 RT-Thread / PC 解析工具兼容
 *
 * 无独立应用线程: 复用专用工作队列 (独立于系统工作队列, 避免 LittleFS
 * 写盘阻塞 Modbus server 的异步处理)。
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <init.h>
#include "fs_littlefs.h"

LOG_MODULE_REGISTER(io_history, LOG_LEVEL_INF);

#define HIST_DIR		"/lfs1"
#define HIST_MAX_FILES		10
#define HIST_FILE_MAX		(1024 * 1024)

K_MSGQ_DEFINE(his_msgq, sizeof(struct his_data), 16, 4);

/* 专用工作队列: 历史落盘与 Modbus server (系统工作队列) 隔离.
 * 4096: cleanup_old_files 的文件名数组 + LittleFS 目录遍历/写入调用链较深,
 * 2048 会栈溢出 (深度溢出 → double fault → LOCKUP 静默复位) */
K_KERNEL_STACK_DEFINE(hist_q_stack, 4096);
static struct k_work_q hist_work_q;

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
	int mon, mday, hour, min, sec;

	/* RTC 未同步时 gmtime 可能返回 NULL, 用全零填充文件名兜底 */
	if (lt == NULL) {
		snprintf(buf, len, "data_0101_000000.raw");
		return;
	}
	/* 钳位到合法范围: 消除 -Wformat-truncation (字段值域本应有界,
	 * 钳位既保证 %02d 定宽 2 位, 也避免 RTC 异常数据生成非法文件名) */
	mon  = CLAMP(lt->tm_mon + 1, 1, 12);
	mday = CLAMP(lt->tm_mday, 1, 31);
	hour = CLAMP(lt->tm_hour, 0, 23);
	min  = CLAMP(lt->tm_min, 0, 59);
	sec  = CLAMP(lt->tm_sec, 0, 59);
	snprintf(buf, len, "data_%02d%02d_%02d%02d%02d.raw",
		 mon, mday, hour, min, sec);
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

	char name[24];
	char path[48];

	make_hist_name(name, sizeof(name));
	/* fs_open 需要绝对路径 (以 '/' 开头的挂载点前缀 + 文件名) */
	snprintf(path, sizeof(path), "%s/%s", HIST_DIR, name);
	fs_file_t_init(&his_fp);
	if (fs_open(&his_fp, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND) != 0) {
		return -EIO;
	}

	fs_seek(&his_fp, 0, FS_SEEK_END);
	off_t tell = fs_tell(&his_fp);

	if (tell < 0) {
		LOG_ERR("history file tell failed: %lld", (long long)tell);
		fs_close(&his_fp);
		his_fp_open = false;
		return -EIO;
	}
	his_cur_size = (uint32_t)tell;
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
		ssize_t wr = fs_write(&his_fp, &d, len);

		if (wr == (ssize_t)len) {
			his_cur_size += len;
			wrote = true;
		} else {
			/* 部分写入: 文件可能超限, 强制下次轮转新文件 */
			LOG_WRN("history write short (%zd/%zu)", wr, len);
			his_cur_size = HIST_FILE_MAX;
			break;
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

/* 专用工作队列初始化 (priority 5: 早于 settings 11, 供历史开关同步使用) */
static int hist_work_q_init(void)
{
	k_work_queue_start(&hist_work_q, hist_q_stack,
			   K_KERNEL_STACK_SIZEOF(hist_q_stack), 10, NULL);
	return 0;
}
SYS_INIT(hist_work_q_init, APPLICATION, 5);

void history_enable_write(bool en)
{
	history_enabled = en;
	LOG_INF("history %s", en ? "enabled" : "disabled");
	if (!en) {
		/* 异步 flush + 关闭, 不在调用线程 (Modbus 写回调) 做 Flash IO */
		k_work_submit_to_queue(&hist_work_q, &his_work);
	}
}

void send_history_data(const struct his_data *data)
{
	static atomic_t drop_cnt;

	if (!history_enabled || !io_lfs_is_ready()) {
		return;
	}
	if (k_msgq_put(&his_msgq, data, K_NO_WAIT) == 0) {
		/* 提交到专用工作队列 (k_work 合并: 执行前重复提交只跑一次)。
		 * handler 批量取 msgq + 保持文件打开 + fs_sync, 减少 Flash 写 */
		k_work_submit_to_queue(&hist_work_q, &his_work);
	} else if ((atomic_inc(&drop_cnt) % 100) == 0) {
		/* 后台落盘慢时 msgq 会满, 节流告警 (已丢弃 %u 条) */
		LOG_WRN("history msgq full, %u samples dropped", (uint32_t)drop_cnt);
	}
}
