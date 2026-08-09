/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * LittleFS 文件系统挂载 (flash-area 模式, 外部 W25Q128 littlefs 分区)
 *
 * 挂载点 /lfs1, 存放 IO 历史记录文件 (history.c 写, FTP 下载)。
 * 首次挂载失败 (分区未格式化) 时自动 mkfs 后重挂。
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(io_lfs, LOG_LEVEL_INF);

static struct fs_littlefs lfs_data;
static struct fs_mount_t lfs_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &lfs_data,
	.storage_dev = (void *)PARTITION_ID(littlefs_partition),
	.mnt_point = "/lfs1",
};

static K_SEM_DEFINE(lfs_ready, 0, 1);

struct fs_mount_t *io_lfs_mount(void)
{
	return &lfs_mnt;
}

bool io_lfs_wait_ready(k_timeout_t timeout)
{
	return k_sem_take(&lfs_ready, timeout) == 0;
}

static int littlefs_init(void)
{
	int rc = fs_mount(&lfs_mnt);

	if (rc == -ENODEV || rc == -EINVAL) {
		LOG_INF("LittleFS unformatted, running mkfs");
		rc = fs_mkfs(FS_LITTLEFS, (uintptr_t)lfs_mnt.storage_dev,
			     &lfs_data, 0);
		if (rc == 0) {
			rc = fs_mount(&lfs_mnt);
		}
	}

	if (rc != 0) {
		LOG_ERR("LittleFS mount failed: %d", rc);
		return rc;
	}

	k_sem_give(&lfs_ready);

	struct fs_statvfs stat;

	if (fs_statvfs("/lfs1", &stat) == 0) {
		LOG_INF("LittleFS /lfs1: %llu bytes free",
			(unsigned long long)stat.f_bfree * stat.f_bsize);
	} else {
		LOG_INF("LittleFS /lfs1 mounted");
	}
	return 0;
}

SYS_INIT(littlefs_init, APPLICATION, 99);
