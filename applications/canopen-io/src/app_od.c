/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * OD 扩展回调: 计测器对象的写副作用与触发语义
 * (参数源统一为 holding_reg[]/settings, 0x2004 成为寄存器的 OD 桥接视图)
 *   - 0x2000/0x2001: passthrough 扩展, 仅为启用 flagsPDO (采样线程
 *     OD_requestTPDO 触发事件型 TPDO 需要 entry->extension 存在)
 *   - 0x2002: 写后联动 DO GPIO/LED 并同步 holding_reg (SDO 写与 RPDO1
 *     收包共路; Modbus/Web 写则反向经 mb_set_do 镜像回 0x2002)
 *   - 0x2004: 应用参数桥接到 holding_reg (settings/FCB 持久化):
 *     :1/:2 使能位图 → 0x01/0x02; :3/:4 采样间隔(钳位) → 0x03/0x04;
 *     :5 写 1 → holding_reg_save(); :6 写 1 → 延迟冷重启.
 *     触发子索引写后回读恒为 0。持久化不再走 CANopenNode storage 的
 *     0x1010:2 (main 中仅保留通信参数条目), 改由 modbus/ settings 命名空间
 *     统一存储。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "CANopen.h"
#include "OD.h"
#include "io.h"
#include "app_od.h"
#include "fw_download.h"
#include "init.h"

LOG_MODULE_REGISTER(canopen_io_od, LOG_LEVEL_INF);

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

/* SDO 读前把寄存器当前值刷进 OD 缓冲 (触发子索引 :5/:6 恒 0) */
static ODR_t cfg_read(OD_stream_t *stream, void *buf, OD_size_t count, OD_size_t *countRead)
{
	OD_PERSIST_APP.x2004_configParams[0] = get_holding_reg(HOLDING_DI_ENABLE_IDX);
	OD_PERSIST_APP.x2004_configParams[1] = get_holding_reg(HOLDING_AI_ENABLE_IDX);
	OD_PERSIST_APP.x2004_configParams[2] =
		(uint16_t)get_holding_reg(HOLDING_DI_SAMPLE_MS_IDX);
	OD_PERSIST_APP.x2004_configParams[3] =
		(uint16_t)get_holding_reg(HOLDING_AI_SAMPLE_MS_IDX);
	OD_PERSIST_APP.x2004_configParams[4] = 0;
	OD_PERSIST_APP.x2004_configParams[5] = 0;
	return OD_readOriginal(stream, buf, count, countRead);
}

static ODR_t cfg_write(OD_stream_t *stream, const void *buf, OD_size_t count,
		       OD_size_t *countWritten)
{
	uint16_t val;
	uint16_t sub = stream->subIndex;

	if (count != sizeof(val)) {
		return ODR_DATA_SHORT;
	}
	memcpy(&val, buf, sizeof(val));

	switch (sub) {
	case 1: /* DI 使能 → holding_reg[0x01] */
		update_holding_reg(HOLDING_DI_ENABLE_IDX, val);
		break;
	case 2: /* AI 使能 → holding_reg[0x02] */
		update_holding_reg(HOLDING_AI_ENABLE_IDX, val);
		break;
	case 3: /* DI 采样间隔 → holding_reg[0x03] (钳位) */
	case 4: /* AI 采样间隔 → holding_reg[0x04] (钳位) */
		if (val < CONFIG_CANOPEN_IO_SAMPLE_MIN_MS) {
			val = CONFIG_CANOPEN_IO_SAMPLE_MIN_MS;
		} else if (val > CONFIG_CANOPEN_IO_SAMPLE_MAX_MS) {
			val = CONFIG_CANOPEN_IO_SAMPLE_MAX_MS;
		}
		update_holding_reg(
			sub == 3 ? HOLDING_DI_SAMPLE_MS_IDX : HOLDING_AI_SAMPLE_MS_IDX, val);
		break;
	case 5: /* 保存触发: 全量持久化 modbus/ 参数 */
		if (fw_download_active()) {
			return ODR_DATA_DEV_STATE; /* 下载进行中, 拒绝存储 */
		}
		if (val == 1) {
			holding_reg_save();
			LOG_INF("params saved via OD 0x2004:5");
		}
		val = 0; /* 回读恒为 0 */
		break;
	case 6: /* 重启触发: 延迟冷重启 (housekeeping 排空日志后执行) */
		if (val == 1) {
			set_reboot_status(true);
		}
		val = 0; /* 回读恒为 0 */
		break;
	default:
		/* 子索引 0 (highest sub-index) 等: 原样透传 */
		return OD_writeOriginal(stream, buf, count, countWritten);
	}

	return OD_writeOriginal(stream, &val, sizeof(val), countWritten);
}

static OD_extension_t ext_2000 = {
	.object = NULL, .read = OD_readOriginal, .write = OD_writeOriginal};
static OD_extension_t ext_2001 = {
	.object = NULL, .read = OD_readOriginal, .write = OD_writeOriginal};
static OD_extension_t ext_2002 = {
	.object = NULL, .read = OD_readOriginal, .write = do_write};
static OD_extension_t ext_2004 = {
	.object = NULL, .read = cfg_read, .write = cfg_write};

void app_od_init(void)
{
	OD_extension_init(OD_ENTRY_H2000, &ext_2000);
	OD_extension_init(OD_ENTRY_H2001, &ext_2001);
	OD_extension_init(OD_ENTRY_H2002, &ext_2002);
	OD_extension_init(OD_ENTRY_H2004, &ext_2004);
}
