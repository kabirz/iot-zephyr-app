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
 *
 * 与其他升级通道 (UDP/WebSocket) 共用 fw_upgrade_state 互斥锁:
 * 进入下载模式时申请 FW_UPGRADE_CHANNEL_SDO 锁, 被占用则拒绝;
 * 确认升级成功后保持占用直到重启, 失败/复位后归还。
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
#include "fw_upgrade_state.h"
#include "init.h"

LOG_MODULE_REGISTER(canopen_io_fw, LOG_LEVEL_INF);

static struct flash_img_context fw_img;
static enum fw_dl_state fw_state = FW_DL_IDLE;
static size_t fw_written;
/* 本模块是否已持有升级锁 (重复写 0x1F51=0 复位不重复申请) */
static bool sdo_lock_held;
static struct k_work_delayable fw_reboot_work;

static void fw_reboot_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("firmware confirmed, rebooting for MCUboot swap");
	history_sync();
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
		/* 预擦除 slot1 尾部 trailer 区 (对齐 libs/can_fw_upgrade 的经验):
		 * trailer 扇区可能残留旧数据 (曾用其他固件升级过), 镜像体的懒擦除
		 * 覆盖不到, 残留会污染 boot_request_upgrade 写入的 trailer 导致
		 * MCUboot 判定 no-swap。只擦尾部 8KB (约百毫秒), 不阻塞 SDO 应答。
		 * 其他通道升级进行中则拒绝进入。 */
		const struct flash_area *fa;

		if (!sdo_lock_held) {
			if (!fw_upgrade_try_lock(FW_UPGRADE_CHANNEL_SDO)) {
				LOG_WRN("fw download: upgrade busy on other channel");
				return ODR_DEV_INCOMPAT;
			}
			sdo_lock_held = true;
		}
		if (flash_area_open(PARTITION_ID(slot1_partition), &fa) != 0) {
			goto error;
		}
		size_t trailer_sz = MIN(8192, fa->fa_size);
		int rc = flash_area_erase(fa, fa->fa_size - trailer_sz, trailer_sz);

		flash_area_close(fa);
		if (rc != 0) {
			goto error;
		}
		if (flash_img_init_id(&fw_img, PARTITION_ID(slot1_partition)) != 0) {
			goto error;
		}
		fw_written = 0;
		fw_state = FW_DL_READY;
		return OD_writeOriginal(stream, buf, count, countWritten);
	error:
		fw_state = FW_DL_ERROR;
		fw_upgrade_unlock(FW_UPGRADE_CHANNEL_SDO);
		sdo_lock_held = false;
		return ODR_HW;
	}
	if (val == 1) { /* 确认: flush + 请求升级 + 延迟重启 */
		if (fw_state != FW_DL_STREAMING || fw_written == 0) {
			fw_state = FW_DL_ERROR;
			if (sdo_lock_held) {
				fw_upgrade_unlock(FW_UPGRADE_CHANNEL_SDO);
				sdo_lock_held = false;
			}
			return ODR_DEV_INCOMPAT;
		}
		if (flash_img_buffered_write(&fw_img, NULL, 0, true) != 0) {
			fw_state = FW_DL_ERROR;
			if (sdo_lock_held) {
				fw_upgrade_unlock(FW_UPGRADE_CHANNEL_SDO);
				sdo_lock_held = false;
			}
			return ODR_DATA_TRANSF;
		}
		if (boot_request_upgrade(BOOT_UPGRADE_PERMANENT) != 0) {
			fw_state = FW_DL_ERROR;
			if (sdo_lock_held) {
				fw_upgrade_unlock(FW_UPGRADE_CHANNEL_SDO);
				sdo_lock_held = false;
			}
			return ODR_HW;
		}
		fw_state = FW_DL_CONFIRMED;
		/* 升级锁保持占用直到重启, 阻塞 UDP/Web 通道并发升级 */
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
