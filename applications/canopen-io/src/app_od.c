/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * OD 扩展回调: 计测器对象的写副作用与触发语义
 *   - 0x2000/0x2001: passthrough 扩展, 仅为启用 flagsPDO (采样线程
 *     OD_requestTPDO 触发事件型 TPDO 需要 entry->extension 存在)
 *   - 0x2002: 写后联动 DO GPIO/LED (SDO 写与 RPDO1 收包共路)
 *   - 0x2004:3/4 采样间隔钳位; :5 写 1 => 0x1010:1/2 "save" 整组持久化;
 *     :6 写 1 => 延迟冷重启. 触发子索引写后回读恒为 0
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include "CANopen.h"
#include "OD.h"
#include "io.h"
#include "app_od.h"

LOG_MODULE_REGISTER(canopen_io_od, LOG_LEVEL_INF);

#define OD_SAVE_MAGIC 0x73617665U /* "save" (0x1010/0x1011 魔数, 小端) */

static ODR_t do_write(OD_stream_t *stream, const void *buf, OD_size_t count,
		      OD_size_t *countWritten)
{
	ODR_t ret = OD_writeOriginal(stream, buf, count, countWritten);

	if (ret == ODR_OK && count == sizeof(uint16_t)) {
		uint16_t val;

		memcpy(&val, buf, sizeof(val));
		co_io_set_do(val);
	}
	return ret;
}

static void app_reboot_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("reboot requested (OD 0x2004:6)");
	sys_reboot(SYS_REBOOT_COLD);
}

static struct k_work_delayable app_reboot_work;

static ODR_t cfg_write(OD_stream_t *stream, const void *buf, OD_size_t count,
		       OD_size_t *countWritten)
{
	uint16_t val;

	if (count != sizeof(val)) {
		return ODR_DATA_SHORT;
	}
	memcpy(&val, buf, sizeof(val));

	switch (stream->subIndex) {
	case 3: /* DI 采样间隔 */
	case 4: /* AI 采样间隔 */
		if (val < CONFIG_CANOPEN_IO_SAMPLE_MIN_MS) {
			val = CONFIG_CANOPEN_IO_SAMPLE_MIN_MS;
		} else if (val > CONFIG_CANOPEN_IO_SAMPLE_MAX_MS) {
			val = CONFIG_CANOPEN_IO_SAMPLE_MAX_MS;
		}
		return OD_writeOriginal(stream, &val, sizeof(val), countWritten);
	case 5: /* 保存触发 */
		if (val == 1) {
			(void)OD_set_u32(OD_ENTRY_H1010, 1, OD_SAVE_MAGIC, false);
			(void)OD_set_u32(OD_ENTRY_H1010, 2, OD_SAVE_MAGIC, false);
			LOG_INF("OD saved via 0x2004:5");
		}
		val = 0;
		return OD_writeOriginal(stream, &val, sizeof(val), countWritten);
	case 6: /* 重启触发 */
		if (val == 1) {
			k_work_schedule(&app_reboot_work,
					K_MSEC(CONFIG_CANOPEN_IO_FW_REBOOT_DELAY_MS));
		}
		val = 0;
		return OD_writeOriginal(stream, &val, sizeof(val), countWritten);
	default:
		return OD_writeOriginal(stream, buf, count, countWritten);
	}
}

static OD_extension_t ext_2000 = {
	.object = NULL, .read = OD_readOriginal, .write = OD_writeOriginal};
static OD_extension_t ext_2001 = {
	.object = NULL, .read = OD_readOriginal, .write = OD_writeOriginal};
static OD_extension_t ext_2002 = {
	.object = NULL, .read = OD_readOriginal, .write = do_write};
static OD_extension_t ext_2004 = {
	.object = NULL, .read = OD_readOriginal, .write = cfg_write};

void app_od_init(void)
{
	k_work_init_delayable(&app_reboot_work, app_reboot_handler);

	OD_extension_init(OD_ENTRY_H2000, &ext_2000);
	OD_extension_init(OD_ENTRY_H2001, &ext_2001);
	OD_extension_init(OD_ENTRY_H2002, &ext_2002);
	OD_extension_init(OD_ENTRY_H2004, &ext_2004);
}
