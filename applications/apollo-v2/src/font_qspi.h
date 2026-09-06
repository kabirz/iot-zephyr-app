/* SPDX-License-Identifier: Apache-2.0 */
#ifndef APOLLO_FONT_QSPI_H
#define APOLLO_FONT_QSPI_H

#include <lvgl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* QSPI 字库 (GB2312 一级 + ASCII, 16px 4bpp, AF16 格式)。
 * 数据经 QSPI memory-mapped 区直读, LVGL 回调内二分查码表。 */

/* 字库就绪 (AF16 magic 校验通过); 未安装/校验失败返回 false */
bool apollo_font_qspi_ready(void);

/* QSPI 版全量中文字体 (未就绪时不可用, 用 apollo_font_ui() 兜底) */
extern const lv_font_t apollo_font_qspi;

/* UI 优先字体: QSPI 全量字库, 未安装则回退编译内置的 UI 用词字库 */
const lv_font_t *apollo_font_ui(void);

/* 诊断: 读分区头 16 字节 (hex 字符串输出到 out) */
void apollo_font_dump_header(char *out, size_t outsz);

/* 从 /SD:/font.bin 安装字库到 QSPI font 分区 (校验 AF16 头)。
 * 成功返回 0, 写完后需重启 (清 D-Cache 并让 UI 重新加载)。 */
int apollo_font_install(void);

#endif /* APOLLO_FONT_QSPI_H */
