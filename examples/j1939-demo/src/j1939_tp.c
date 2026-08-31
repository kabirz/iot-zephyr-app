/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * J1939/21 传输层: BAM (Broadcast Announce Message) 多包发送
 *
 * 用 TP.CM (PGN 60416) 广播控制帧, 再用 TP.DT (PGN 60160) 以
 * 55ms 间隔 (规范要求 >= 50ms) 发出全部数据帧, 接收方不确认。
 * 报文最长 1785 字节 (255 包 x 7 字节)。
 *
 * 未实现: 接收端重组、RTS/CTS 点对点流控 (TP.CM_RTS/CTS/EOM)。
 */
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "j1939.h"

LOG_MODULE_REGISTER(j1939_tp, CONFIG_LOG_DEFAULT_LEVEL);

#define TP_CM_BAM       0x20u  /* BAM 控制字节 */
#define TP_FRAME_GAP_MS 55u    /* 规范: 数据帧间隔 >= 50ms */
#define TP_PRIO         7u     /* 传输层报文用最低优先级 */

int j1939_send_bam(uint32_t pgn, const uint8_t *data, size_t len)
{
	uint8_t cm[8];
	uint8_t dt[8];
	size_t off = 0;
	uint8_t seq = 1;
	uint8_t sa;
	int ret;

	if (!j1939_address_valid()) {
		return -EADDRNOTAVAIL;
	}
	if (len == 0 || len > J1939_MAX_DATA) {
		return -EINVAL;
	}
	if (len <= 8) {
		/* 单帧足够, 退化为普通发送 */
		return j1939_send(TP_PRIO, pgn, J1939_SA_GLOBAL, data, len);
	}

	sa = j1939_source_address();

	/* TP.CM BAM: [控制字节][总长 LE16][总包数][保留][目标 PGN LE24] */
	cm[0] = TP_CM_BAM;
	cm[1] = (uint8_t)(len & 0xff);
	cm[2] = (uint8_t)((len >> 8) & 0xff);
	cm[3] = (uint8_t)((len + 6) / 7);
	cm[4] = 0xff;
	cm[5] = (uint8_t)(pgn & 0xff);
	cm[6] = (uint8_t)((pgn >> 8) & 0xff);
	cm[7] = (uint8_t)((pgn >> 16) & 0xff);

	ret = j1939_send_raw(TP_PRIO, J1939_PGN_TP_CM, sa,
			     J1939_SA_GLOBAL, cm, sizeof(cm));
	if (ret != 0) {
		return ret;
	}

	/* TP.DT: [序号][7 字节数据], 尾帧用 0xFF 填充 */
	while (off < len) {
		size_t n = MIN(7, len - off);

		dt[0] = seq++;
		memcpy(&dt[1], &data[off], n);
		memset(&dt[1 + n], 0xff, 7 - n);

		ret = j1939_send_raw(TP_PRIO, J1939_PGN_TP_DT, sa,
				     J1939_SA_GLOBAL, dt, sizeof(dt));
		if (ret != 0) {
			LOG_ERR("TP.DT seq %u failed: %d", seq - 1, ret);
			return ret;
		}

		off += n;
		if (off < len) {
			k_sleep(K_MSEC(TP_FRAME_GAP_MS));
		}
	}

	return 0;
}
