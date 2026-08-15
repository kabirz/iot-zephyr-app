/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 固件升级库 - 自包含实现
 * 库通过 SYS_INIT 自动初始化 CAN (bitrate/start + 全接收过滤器),
 * 使用静态 K_THREAD_DEFINE 的 RX 线程, 内部处理固件升级;
 * 非固件帧通过应用注册的回调分发。
 */

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include "can_fw_upgrade.h"
#include <fw_gitver.h>

#ifdef CONFIG_MCUBOOT_SIGNATURE_KEY_FILE
#include <fw_keyhash.h>
#endif

LOG_MODULE_REGISTER(can_fw_upgrade, LOG_LEVEL_INF);

/* CAN 帧 ID */
#define CAN_FW_PLATFORM_RX  0x101
#define CAN_FW_PLATFORM_TX  0x102
#define CAN_FW_FW_DATA_RX   0x103
#define CAN_FW_KEYHASH_RX   0x104   /* keyhash 帧: data[0]=seq, data[1..7]=7B chunk */
#define CAN_FW_VERSION_TX   0x105   /* 版本字符串帧: data[0]=seq, data[1..7]=7B 文本 */

/* keyhash 分帧: 每帧 1B seq + 7B keyhash (CAN DLC 上限 8B), 32B 需 5 帧 */
#define CAN_FW_KEYHASH_CHUNK_BYTES 7
#define CAN_FW_KEYHASH_CHUNKS ((FW_KEYHASH_KEY_LEN + CAN_FW_KEYHASH_CHUNK_BYTES - 1) / CAN_FW_KEYHASH_CHUNK_BYTES)
#define CAN_FW_KEYHASH_FULL_MASK ((1U << CAN_FW_KEYHASH_CHUNKS) - 1)

/* 命令码 */
enum fw_cmd {
	FW_CMD_START_UPDATE = 0,
	FW_CMD_CONFIRM,
	FW_CMD_VERSION,
	FW_CMD_REBOOT,
};

/* 响应码 */
enum fw_code {
	FW_CODE_OFFSET = 0,
	FW_CODE_UPDATE_SUCCESS,
	FW_CODE_VERSION,
	FW_CODE_CONFIRM,
	FW_CODE_FLASH_ERROR,
	FW_CODE_TRANFER_ERROR,
	FW_CODE_KEYHASH_ERROR,   /* keyhash 不一致, 已拒绝升级 */
};

#define SLOT1_PARTITION_ID PARTITION_ID(slot1_partition)

/* ================================================================
 * 全局状态
 * ================================================================ */
static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

/* 已注册的业务帧 handler 列表 (RX 线程按序遍历广播) */
struct can_fw_handler {
	can_fw_app_rx_cb_t cb;
	void *user_data;
};
static struct can_fw_handler handlers[CONFIG_CAN_FW_UPGRADE_MAX_HANDLERS];

static struct flash_img_context flash_img_ctx;
static bool fw_img_initialized;
static size_t fw_written;
static size_t fw_total_size;

#ifdef CONFIG_MCUBOOT_SIGNATURE_KEY_FILE
/* 上位机 keyhash 累积缓冲 (4 x 8B 分帧) + 到齐位图 */
static uint8_t rx_keybuf[FW_KEYHASH_KEY_LEN];
static uint8_t key_chunk_mask;
#endif

/* RX msgq (全接收过滤器投递目标) */
K_MSGQ_DEFINE(can_fw_rx_msgq, sizeof(struct can_frame), 8, 4);

/* ================================================================
 * 固件升级响应
 * ================================================================ */
static void fw_can_reply(uint32_t code, uint32_t offset)
{
	struct can_frame frame = {
		.id = CAN_FW_PLATFORM_TX,
		.data_32 = {code, offset},
		.dlc = can_bytes_to_dlc(8),
	};

	can_send(can_dev, &frame, K_MSEC(100), NULL, NULL);
}

/* 发送版本字符串分帧 (0x105). 每帧 data[0]=seq, data[1..7]=最多 7B 文本.
 * 末帧不足 7B 用 '\0' 填充, 上位机遇 '\0' 截断.
 * 版本字符串最长约 17B (v255.255.255_abcdef), 故最多 3 帧. */
static void fw_can_send_version_string(const char *ver, uint8_t len)
{
	for (uint8_t off = 0, seq = 0; off < len; off += 7, seq++) {
		struct can_frame frame = {
			.id = CAN_FW_VERSION_TX,
			.dlc = can_bytes_to_dlc(8),
		};
		uint8_t chunk = MIN(7, len - off);

		frame.data[0] = seq;
		memcpy(&frame.data[1], ver + off, chunk);
		if (chunk < 7) {
			memset(&frame.data[1 + chunk], 0, 7 - chunk);
		}
		can_send(can_dev, &frame, K_MSEC(100), NULL, NULL);
	}
}

/* ================================================================
 * 固件控制帧处理 (0x101)
 * ================================================================ */
static void handle_platform_rx(struct can_frame *frame)
{
	uint32_t cmd = frame->data_32[0];

	if (cmd == FW_CMD_START_UPDATE) {
		uint32_t size = can_dlc_to_bytes(frame->dlc);

		if (size != 8) {
			LOG_ERR("start update: invalid size %d", size);
			fw_can_reply(FW_CODE_FLASH_ERROR, 0);
			return;
		}

#ifdef CONFIG_MCUBOOT_SIGNATURE_KEY_FILE
		/* 升级前 keyhash 校验: 仅当上位机先前把 4 帧 keyhash (0x104) 送齐才校验;
		 * 不一致 → 拒绝且不触碰 slot1 flash. 老上位机不发 key 帧则放行 (兼容). */
		if ((key_chunk_mask & CAN_FW_KEYHASH_FULL_MASK) == CAN_FW_KEYHASH_FULL_MASK) {
			key_chunk_mask = 0;

			if (memcmp(rx_keybuf, fw_keyhash, FW_KEYHASH_KEY_LEN) != 0) {
				LOG_WRN("FW upgrade rejected: keyhash mismatch");
				fw_can_reply(FW_CODE_KEYHASH_ERROR, 0);
				return;
			}
		}
#endif

		/* 每次都重新擦除 + 初始化: 上次中途失败的传输若不重置,
		 * 旧偏移/缓冲状态会导致 flash 写错位 */
		{
			const struct flash_area *fa;

			if (flash_area_open(SLOT1_PARTITION_ID, &fa) != 0) {
				LOG_ERR("flash_area_open failed");
				fw_can_reply(FW_CODE_FLASH_ERROR, 0);
				return;
			}
			flash_area_erase(fa, 0, fa->fa_size);
			flash_area_close(fa);

			if (flash_img_init(&flash_img_ctx) != 0) {
				LOG_ERR("flash_img_init failed");
				fw_img_initialized = false;
				fw_can_reply(FW_CODE_FLASH_ERROR, 0);
				return;
			}
			fw_img_initialized = true;
			fw_written = 0;
		}

		/* 丢弃 msgq 中残留的旧 FW_DATA 帧 (擦除期间堆积的) */
		{
			struct can_frame stale;

			while (k_msgq_get(&can_fw_rx_msgq, &stale, K_NO_WAIT) == 0) {
			}
		}

		LOG_INF("FW upgrade start, size=%d", frame->data_32[1]);
		fw_total_size = frame->data_32[1];
		fw_can_reply(FW_CODE_OFFSET, 0);

	} else if (cmd == FW_CMD_CONFIRM) {
		/* val (data_32[1]): 0=临时升级 (重启后回滚), 1=永久升级.
		 * 与 gateway UDP 侧语义一致, 直接透传给 boot_request_upgrade.
		 * 未先成功 START 则拒绝 (不触碰 flash, 对齐 UDP 侧 fw_started 语义). */
		if (!fw_img_initialized) {
			LOG_WRN("FW confirm before start");
			fw_can_reply(FW_CODE_TRANFER_ERROR, 0);
			return;
		}

		uint32_t permanent = frame->data_32[1];

		flash_img_buffered_write(&flash_img_ctx, NULL, 0, true);
		fw_img_initialized = false;

		if (fw_written != fw_total_size) {
			LOG_ERR("FW upgrade failed: %zu != %zu", fw_written, fw_total_size);
			fw_can_reply(FW_CODE_TRANFER_ERROR, 0);
			return;
		}

		int ret = boot_request_upgrade(permanent);

		if (ret == 0) {
			LOG_INF("FW upgrade confirmed (permanent=%u), waiting for reboot",
				permanent);
			fw_can_reply(FW_CODE_CONFIRM, 0x55AA55AA);
			/* 不自动 reboot: 由上位机重启按钮 (FW_CMD_REBOOT) 触发 */
		} else {
			LOG_ERR("boot_request_upgrade failed: %d", ret);
			fw_can_reply(FW_CODE_TRANFER_ERROR, 0);
		}

	} else if (cmd == FW_CMD_VERSION) {
		/* 版本字符串 "v<M>.<m>.<p>_<6hex>" 分帧回复:
		 * 先发 FW_CODE_VERSION (offset=字符串总长度), 再发 N 帧 0x105 分片.
		 * 上位机据 offset 等待对应字节数, 或遇 '\0' 截断. */
		char ver[24];
		int vlen = snprintf(ver, sizeof(ver), "v%d.%d.%d_%s",
				    APP_VERSION_MAJOR, APP_VERSION_MINOR,
				    APP_PATCHLEVEL, FW_GIT_VERSION);
		fw_can_reply(FW_CODE_VERSION, (uint32_t)vlen);
		fw_can_send_version_string(ver, (uint8_t)vlen);

	} else if (cmd == FW_CMD_REBOOT) {
		/* 排空 deferred 日志缓冲再重启, 避免随 sys_reboot 丢失 */
		while (log_process()) {
		}
		k_msleep(50);
		sys_reboot(SYS_REBOOT_WARM);
	}
}

/* ================================================================
 * keyhash 帧处理 (0x104): data[0]=seq(0..3), data[1..8]=8B chunk
 * 累积到 rx_keybuf, 全部到齐置 full mask, 供 START_UPDATE 校验用。
 * ================================================================ */
#ifdef CONFIG_MCUBOOT_SIGNATURE_KEY_FILE
static void handle_keyhash_frame(struct can_frame *frame)
{
	uint8_t seq = frame->data[0];
	uint8_t rem = FW_KEYHASH_KEY_LEN - seq * CAN_FW_KEYHASH_CHUNK_BYTES;
	uint8_t chunk = MIN(rem, CAN_FW_KEYHASH_CHUNK_BYTES);
	uint8_t bytes = can_dlc_to_bytes(frame->dlc);

	if (seq >= CAN_FW_KEYHASH_CHUNKS || bytes < 1 + chunk) {
		LOG_WRN("keyhash frame invalid, seq=%u dlc=%u", seq, frame->dlc);
		return;
	}

	memcpy(&rx_keybuf[seq * CAN_FW_KEYHASH_CHUNK_BYTES], &frame->data[1], chunk);
	key_chunk_mask |= (1U << seq);

	LOG_INF("keyhash chunk %u/%d received", seq, CAN_FW_KEYHASH_CHUNKS);
}
#endif

/* ================================================================
 * 固件数据帧处理 (0x103)
 * ================================================================ */
static void handle_fw_data(struct can_frame *frame)
{
	if (!fw_img_initialized) {
		LOG_WRN("FW data before start");
		fw_can_reply(FW_CODE_TRANFER_ERROR, 0);
		return;
	}

	uint32_t size = can_dlc_to_bytes(frame->dlc);

	if (flash_img_buffered_write(&flash_img_ctx, frame->data, size, false) != 0) {
		LOG_ERR("flash write failed");
		fw_can_reply(FW_CODE_FLASH_ERROR, 0);
		return;
	}

	fw_written += size;

	if (fw_written == fw_total_size) {
		fw_can_reply(FW_CODE_UPDATE_SUCCESS, fw_total_size);
	} else if (fw_written % 64 == 0) {
		fw_can_reply(FW_CODE_OFFSET, fw_written);
	}
}

/* ================================================================
 * RX 线程 (静态): 固件帧内部处理, 其余帧分发应用回调
 * ================================================================ */
static void can_fw_rx_thread_fn(void *p1, void *p2, void *p3)
{
	struct can_frame frame;

	while (1) {
		if (k_msgq_get(&can_fw_rx_msgq, &frame, K_FOREVER) != 0) {
			continue;
		}

		if (frame.id == CAN_FW_PLATFORM_RX) {
			handle_platform_rx(&frame);
		} else if (frame.id == CAN_FW_FW_DATA_RX) {
			handle_fw_data(&frame);
#ifdef CONFIG_MCUBOOT_SIGNATURE_KEY_FILE
		} else if (frame.id == CAN_FW_KEYHASH_RX) {
			handle_keyhash_frame(&frame);
#endif
		} else {
			/* 广播给所有已注册的业务帧 handler; 若均未处理则告警 */
			bool handled = false;

			for (int i = 0; i < CONFIG_CAN_FW_UPGRADE_MAX_HANDLERS; i++) {
				if (handlers[i].cb && handlers[i].cb(&frame, handlers[i].user_data)) {
					handled = true;
				}
			}
			if (!handled) {
				uint8_t dlc = can_dlc_to_bytes(frame.dlc);

				LOG_WRN("unhandled CAN frame id=0x%03x dlc=%u", frame.id, dlc);
				LOG_HEXDUMP_WRN(frame.data, dlc, "data");
			}
		}
	}
}

K_THREAD_DEFINE(can_fw_rx_thread, CONFIG_CAN_FW_UPGRADE_RX_STACK_SIZE,
		can_fw_rx_thread_fn, NULL, NULL, NULL,
		CONFIG_CAN_FW_UPGRADE_RX_PRIORITY, 0, 0);

/* ================================================================
 * SYS_INIT: 初始化 CAN (bitrate/start + 全接收过滤器)
 * RX 线程由 K_THREAD_DEFINE 静态创建, 启动后阻塞在 msgq 等待帧。
 * ================================================================ */
static int can_fw_init(void)
{
	int err;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device not ready");
		return -ENODEV;
	}

	err = can_set_bitrate(can_dev, CONFIG_CAN_FW_UPGRADE_BITRATE);
	if (err) {
		LOG_ERR("CAN set bitrate failed: %d", err);
		return err;
	}
	err = can_start(can_dev);
	if (err) {
		LOG_ERR("CAN start failed: %d", err);
		return err;
	}

	/* 全接收过滤器 (mask=0): 所有 CAN 帧进入库 msgq */
	static const struct can_filter filter = {.mask = 0};

	can_add_rx_filter_msgq(can_dev, &can_fw_rx_msgq, &filter);

	LOG_INF("CAN FW upgrade initialized, version=v%d.%d.%d_%s",
		APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_PATCHLEVEL, FW_GIT_VERSION);
	return 0;
}
SYS_INIT(can_fw_init, APPLICATION, CONFIG_CAN_FW_UPGRADE_INIT_PRIORITY);

/* ================================================================
 * API: 注册业务帧回调
 * ================================================================ */
const struct device *can_fw_set_app_handler(can_fw_app_rx_cb_t cb, void *user_data)
{
	if (cb == NULL) {
		return can_dev;
	}
	for (int i = 0; i < CONFIG_CAN_FW_UPGRADE_MAX_HANDLERS; i++) {
		if (handlers[i].cb == NULL) {
			handlers[i].cb = cb;
			handlers[i].user_data = user_data;
			return can_dev;
		}
	}
	LOG_WRN("handler array full (%d)", CONFIG_CAN_FW_UPGRADE_MAX_HANDLERS);
	return NULL;
}

int can_fw_remove_handler(can_fw_app_rx_cb_t cb)
{
	for (int i = 0; i < CONFIG_CAN_FW_UPGRADE_MAX_HANDLERS; i++) {
		if (handlers[i].cb == cb) {
			handlers[i].cb = NULL;
			handlers[i].user_data = NULL;
			return 0;
		}
	}
	return -ENOENT;
}
