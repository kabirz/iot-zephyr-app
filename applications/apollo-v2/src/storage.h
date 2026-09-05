/* SPDX-License-Identifier: Apache-2.0 */
#ifndef APOLLO_STORAGE_H
#define APOLLO_STORAGE_H

#include <stdbool.h>

/* 存储子系统状态 (供状态行 / apollo info 查询) */
struct apollo_storage_info {
	bool qspi_ok;		/* W25Q256 就绪 */
	bool sdram_ok;		/* W9825G6KH 走样通过 */
	bool sd_mounted;	/* TF 卡 /SD: 挂载成功 */
};

const struct apollo_storage_info *storage_info(void);

/* 启动存储线程: QSPI/SDRAM 快速探测 + TF 卡带重试挂载 (后台) */
void storage_start(void);

#endif /* APOLLO_STORAGE_H */
