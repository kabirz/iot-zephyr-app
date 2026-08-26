/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CiA 302-2 固件下载: OD 0x1F50 (固件数据, DOMAIN 流式) / 0x1F51 (固件控制)
 *
 * 状态机: IDLE --0x1F51=0--> READY --首笔 0x1F50--> STREAMING --0x1F51=1-->
 *         CONFIRMED --> 延迟重启 (MCUboot SWAP_SCRATCH 换固件并验签)
 *
 * 写入 slot1 用 flash_img 懒擦除 (stream_flash 按需擦扇区), 无整块预擦停顿;
 * 镜像为 MCUboot 签名 .bin, 设备端不验签, MCUboot 启动时验证, 失败自动回滚.
 * 传输中断 (SDO abort/超时) 后状态停留, 重写 0x1F51=0 复位重来, 不残留可
 * 启动的半包状态.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>

#include "CANopen.h"
#include "OD.h"
#include "fw_download.h"

LOG_MODULE_REGISTER(canopen_io_fw, LOG_LEVEL_INF);

static struct flash_img_context fw_img;
static enum fw_dl_state fw_state = FW_DL_IDLE;
static size_t fw_written;
static struct k_work_delayable fw_reboot_work;

static void fw_reboot_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("firmware confirmed, rebooting for MCUboot swap");
	sys_reboot(SYS_REBOOT_COLD);
}

/* 0x1F50: SDO 流写 -> slot1 (flash_img 懒擦除) */
static ODR_t fw_data_write(OD_stream_t *stream, const void *buf, OD_size_t count,
			   OD_size_t *countWritten)
{
	if (fw_state != FW_DL_READY && fw_state != FW_DL_STREAMING) {
		return ODR_DEV_INCOMPAT; /* 未先写 0x1F51=0 进入下载模式 */
	}
	if (count == 0) {
		return ODR_OK;
	}
	if (flash_img_buffered_write(&fw_img, buf, count, false) != 0) {
		fw_state = FW_DL_ERROR;
		return ODR_DATA_TRANSF; /* SDO abort 0x08000020 */
	}
	fw_written += count;
	stream->dataOffset += count;
	fw_state = FW_DL_STREAMING;
	*countWritten = count;

	/* 末笔: SDO server 在传输结束时把 dataLength 置为总字节数 */
	if (stream->dataLength != 0 && stream->dataOffset >= stream->dataLength) {
		return ODR_OK;
	}
	return ODR_PARTIAL;
}

static ODR_t fw_ctrl_read(OD_stream_t *stream, void *buf, OD_size_t count,
			  OD_size_t *countRead)
{
	uint32_t val = (uint32_t)fw_state;

	if (count < sizeof(val)) {
		return ODR_DATA_SHORT;
	}
	memcpy(buf, &val, sizeof(val));
	*countRead = sizeof(val);
	return ODR_OK;
}

static ODR_t fw_ctrl_write(OD_stream_t *stream, const void *buf, OD_size_t count,
			   OD_size_t *countWritten)
{
	uint32_t val;

	if (count != sizeof(val)) {
		return ODR_DATA_SHORT;
	}
	memcpy(&val, buf, sizeof(val));

	if (val == 0) { /* 复位/进入下载模式 (中止在途传输也走这里) */
		if (flash_img_init_id(&fw_img, PARTITION_ID(slot1_partition)) != 0) {
			fw_state = FW_DL_ERROR;
			return ODR_HW;
		}
		fw_written = 0;
		fw_state = FW_DL_READY;
		return OD_writeOriginal(stream, buf, count, countWritten);
	}
	if (val == 1) { /* 确认: flush + 请求升级 + 延迟重启 */
		if (fw_state != FW_DL_STREAMING || fw_written == 0) {
			fw_state = FW_DL_ERROR;
			return ODR_DEV_INCOMPAT;
		}
		if (flash_img_buffered_write(&fw_img, NULL, 0, true) != 0) {
			fw_state = FW_DL_ERROR;
			return ODR_DATA_TRANSF;
		}
		if (boot_request_upgrade(BOOT_UPGRADE_PERMANENT) != 0) {
			fw_state = FW_DL_ERROR;
			return ODR_HW;
		}
		fw_state = FW_DL_CONFIRMED;
		(void)OD_writeOriginal(stream, buf, count, countWritten);
		k_work_schedule(&fw_reboot_work,
				K_MSEC(CONFIG_CANOPEN_IO_FW_REBOOT_DELAY_MS));
		return ODR_OK;
	}
	return ODR_INVALID_VALUE;
}

static OD_extension_t ext_1F50 = {
	.object = NULL, .read = NULL, .write = fw_data_write};
static OD_extension_t ext_1F51 = {
	.object = NULL, .read = fw_ctrl_read, .write = fw_ctrl_write};

bool fw_download_active(void)
{
	return fw_state == FW_DL_READY || fw_state == FW_DL_STREAMING ||
	       fw_state == FW_DL_CONFIRMED;
}

void fw_download_init(void)
{
	k_work_init_delayable(&fw_reboot_work, fw_reboot_handler);

	OD_extension_init(OD_ENTRY_H1F50, &ext_1F50);
	OD_extension_init(OD_ENTRY_H1F51, &ext_1F51);
	LOG_INF("CiA 302 firmware download ready (0x1F50/0x1F51 -> slot1)");
}
