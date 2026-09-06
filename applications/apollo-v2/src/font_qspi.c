// SPDX-License-Identifier: Apache-2.0
/* QSPI 中文字库: GB2312 一级 3755 字 + ASCII, 16px 4bpp (AF16 格式)。
 *
 * 布局 (计划 §10.4):
 *   0x00: magic "AF16"           (LE32)
 *   0x04: count                  (LE32, 码表项数)
 *   0x08: index_off              (LE32, 恒为 16)
 *   0x0C: bitmap_off             (LE32)
 *   码表: {unicode LE32, bitmap_off LE32} x count (unicode 升序)
 *   位图: 每字 16x16 4bpp = 128B, LVGL A4 行打包, 基线 y=13
 *
 * 读取路径: 全按需读 (flash_area_read) —— 码表项每 glyph 二分读 8B,
 * 位图每 glyph 读 128B 到静态缓冲。QSPI 随机读微秒级, UI 负载无感。
 * 历史: memmap 直读在本 HAL 上挂死 (V1 教训 #8, 两次复现);
 * 523KB 一次性读入 SDRAM 同样挂死 (2026-09-06), 故按需读为最终方案。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <lvgl.h>
#include <errno.h>
#include <string.h>

#include "font_qspi.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(apollo_font, LOG_LEVEL_INF);

#define FONT_AREA_ID	PARTITION_ID(font_partition)
#define FONT_OFFSET	PARTITION_OFFSET(font_partition)
#define FONT_SIZE	PARTITION_SIZE(font_partition)

#define AF16_MAGIC	0x36314641	/* "AF16" LE */
#define GLYPH_BMP_SIZE	128		/* 16x16 4bpp */
#define FONT_LINE_H	16
#define FONT_BASELINE	3

static bool font_ok;
static const struct flash_area *font_fa;
static uint32_t idx_count;
static uint32_t bitmap_off;
static uint8_t glyph_bitmap_buf[GLYPH_BMP_SIZE];	/* 单字形位图缓冲 */

static bool font_load(void)
{
	uint8_t hdr[16];
	uint32_t magic, index_off;

	if (font_ok) {
		return true;
	}

	if (flash_area_open(FONT_AREA_ID, &font_fa) != 0) {
		return false;
	}
	if (flash_area_read(font_fa, 0, hdr, sizeof(hdr)) != 0) {
		flash_area_close(font_fa);
		font_fa = NULL;
		return false;
	}

	magic = sys_get_le32(hdr);
	idx_count = sys_get_le32(hdr + 4);
	index_off = sys_get_le32(hdr + 8);
	bitmap_off = sys_get_le32(hdr + 12);

	if (magic != AF16_MAGIC || idx_count == 0 || idx_count > 6000 ||
	    index_off != 16 || bitmap_off != 16 + idx_count * 8) {
		flash_area_close(font_fa);
		font_fa = NULL;
		LOG_WRN("QSPI 字库未安装或格式非法 (magic=%08x count=%u)",
			magic, idx_count);
		return false;
	}

	font_ok = true;
	LOG_INF("QSPI 字库就绪: %u 字 (按需读取)", idx_count);
	return true;
}

bool apollo_font_qspi_ready(void)
{
	/* 懒加载: 首次调用触发校验, 之后直接返回状态 */
	return font_load();
}

/* ---------------- LVGL 字体回调 ---------------- */
static bool qspi_font_get_glyph_dsc(const lv_font_t *font,
				    lv_font_glyph_dsc_t *dsc,
				    uint32_t letter, uint32_t letter_next)
{
	ARG_UNUSED(font);
	ARG_UNUSED(letter_next);

	if (!font_load()) {
		return false;
	}

	/* 码表 unicode 升序, 二分; 每项 8B 按需读 (16 + mid*8) */
	uint32_t lo = 0, hi = idx_count;
	uint8_t item[8];

	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;

		if (flash_area_read(font_fa, 16 + mid * 8, item, 8) != 0) {
			return false;
		}
		uint32_t key = sys_get_le32(item);

		if (key < letter) {
			lo = mid + 1;
		} else if (key > letter) {
			hi = mid;
		} else {
			uint32_t bmp_rel = sys_get_le32(item + 4);

			dsc->adv_w = 16;
			dsc->box_w = 16;
			dsc->box_h = 16;
			dsc->ofs_x = 0;
			dsc->ofs_y = -(FONT_LINE_H - FONT_BASELINE);	/* 基线 y=13 */
			dsc->stride = 8;	/* 16px * 4bpp / 8 */
			dsc->format = LV_FONT_GLYPH_FORMAT_A4;
			dsc->is_placeholder = 0;
			/* 位图读入静态缓冲, 渲染同步完成, 下次查询前有效 */
			if (flash_area_read(font_fa, bitmap_off + bmp_rel,
					    glyph_bitmap_buf,
					    GLYPH_BMP_SIZE) != 0) {
				return false;
			}
			dsc->gid.src = glyph_bitmap_buf;
			return true;
		}
	}
	return false;
}

static const void *qspi_font_get_glyph_bitmap(lv_font_glyph_dsc_t *dsc,
					      lv_draw_buf_t *draw_buf)
{
	ARG_UNUSED(draw_buf);
	return dsc->gid.src;
}

const lv_font_t apollo_font_qspi = {
	.get_glyph_dsc = qspi_font_get_glyph_dsc,
	.get_glyph_bitmap = qspi_font_get_glyph_bitmap,
	.line_height = FONT_LINE_H,
	.base_line = FONT_BASELINE,
	.underline_position = -2,
	.underline_thickness = 1,
};

const lv_font_t *apollo_font_ui(void)
{
	return font_ok ? &apollo_font_qspi : NULL;
}

/* ---------------- 安装: /QSPI:/font.bin (或 /SD:/font.bin) -> font 分区 ---------------- */
void apollo_font_dump_header(char *out, size_t outsz)
{
	const struct flash_area *fa;
	uint8_t buf[16];

	if (flash_area_open(FONT_AREA_ID, &fa) != 0) {
		snprintf(out, outsz, "flash_area_open failed");
		return;
	}
	int rc = flash_area_read(fa, 0, buf, sizeof(buf));

	flash_area_close(fa);

	if (rc != 0) {
		snprintf(out, outsz, "read failed rc=%d", rc);
		return;
	}
	snprintf(out, outsz, "%02x %02x %02x %02x | %02x %02x %02x %02x | "
		 "%02x %02x %02x %02x | %02x %02x %02x %02x",
		 buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
		 buf[6], buf[7], buf[8], buf[9], buf[10], buf[11],
		 buf[12], buf[13], buf[14], buf[15]);
}

int apollo_font_install(void)
{
	static uint8_t buf[4096];
	struct fs_file_t f;
	uint32_t magic, count, index_off, bitmap_off, total;
	ssize_t n;
	uint32_t off = 0;
	const struct flash_area *fa;
	int rc;

	/* 优先 /QSPI: (SD 写在网络负载下不稳定, 教训 #1 场景) */
	fs_file_t_init(&f);
	rc = fs_open(&f, "/QSPI:/font.bin", FS_O_READ);
	if (rc != 0) {
		rc = fs_open(&f, "/SD:/font.bin", FS_O_READ);
	}
	if (rc != 0) {
		LOG_ERR("打开 font.bin 失败 (%d): 先用 smp_udp.py fswrite 上传到 "
			"/QSPI:/font.bin", rc);
		return rc;
	}
	n = fs_read(&f, buf, 16);
	if (n != 16) {
		fs_close(&f);
		LOG_ERR("读取字库头失败");
		return -EIO;
	}
	magic = sys_get_le32(buf);
	count = sys_get_le32(buf + 4);
	index_off = sys_get_le32(buf + 8);
	bitmap_off = sys_get_le32(buf + 12);
	if (magic != AF16_MAGIC || count == 0 || count > 6000 ||
	    index_off != 16 || bitmap_off != 16 + count * 8) {
		fs_close(&f);
		LOG_ERR("font.bin 头非法 (magic=%08x count=%u)", magic, count);
		return -EINVAL;
	}
	total = bitmap_off + count * GLYPH_BMP_SIZE;
	if (total > FONT_SIZE) {
		fs_close(&f);
		LOG_ERR("字库过大: %u > 分区 %u", total, FONT_SIZE);
		return -EINVAL;
	}

	printk("fontinstall: %u 字, %u 字节, 擦除+写入 QSPI ...\n",
	       count, total);

	rc = flash_area_open(FONT_AREA_ID, &fa);
	if (rc != 0) {
		fs_close(&f);
		return rc;
	}
	/* 分片擦除 + 间隔: 摊薄 QSPI 擦写电流突发 */
	uint32_t erase_len = ROUND_UP(total, 4096);

	for (uint32_t eo = 0; eo < erase_len; eo += 4096) {
		if (flash_area_erase(fa, eo, 4096) != 0) {
			flash_area_close(fa);
			fs_close(&f);
			LOG_ERR("擦除失败 @%u", eo);
			return -EIO;
		}
		k_msleep(25);
		printk("\r  erase %u/%u", eo + 4096, erase_len);
	}
	printk("\n");

	while (off < total) {
		size_t want = MIN(sizeof(buf), total - off);

		n = fs_read(&f, buf, want);
		if (n <= 0) {
			break;
		}
		if (flash_area_write(fa, off, buf, n) != 0) {
			flash_area_close(fa);
			fs_close(&f);
			LOG_ERR("写入失败 @%u", off);
			return -EIO;
		}
		off += n;
		k_msleep(1);
	}
	flash_area_close(fa);
	fs_close(&f);

	if (off != total) {
		LOG_ERR("文件不完整: %u/%u", off, total);
		return -EIO;
	}

	printk("fontinstall: 完成, 重启后生效 (apollo reboot)\n");
	return 0;
}
