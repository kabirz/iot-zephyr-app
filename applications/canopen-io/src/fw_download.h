/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CiA 302-2 固件下载: OD 0x1F50 (固件数据) / 0x1F51 (固件控制)
 */
#ifndef CANOPEN_IO_FW_DOWNLOAD_H
#define CANOPEN_IO_FW_DOWNLOAD_H

#include <stdbool.h>

/* OD 0x1F51 读出的状态值 */
enum fw_dl_state {
	FW_DL_IDLE = 0,
	FW_DL_READY = 1,
	FW_DL_STREAMING = 2,
	FW_DL_CONFIRMED = 3,
	FW_DL_ERROR = 4,
};

void fw_download_init(void);
bool fw_download_active(void);

#endif /* CANOPEN_IO_FW_DOWNLOAD_H */
