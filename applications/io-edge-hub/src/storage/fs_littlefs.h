/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __FS_LITTLEFS_H__
#define __FS_LITTLEFS_H__

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>

struct fs_mount_t *io_lfs_mount(void);

/* 阻塞等待 LittleFS 挂载完成 (history 线程启动时调用) */
bool io_lfs_wait_ready(k_timeout_t timeout);

#endif /* __FS_LITTLEFS_H__ */
