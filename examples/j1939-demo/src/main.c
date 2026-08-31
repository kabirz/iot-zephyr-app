/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * j1939-demo: 把 io_edge_f407vet6 当作一个 J1939 "IO 计测节点"
 *
 * 演示内容:
 *   1. 上电地址声明 (J1939/81, SA 默认 128, 可 Kconfig 改)
 *   2. 周期广播 8 字节 IO 快照 PGN 65280 (0xFF00, 厂商自定义区)
 *   3. 每 N 帧用 TP BAM 发送 16 字节扩展快照 PGN 65281 (0xFF01)
 *   4. 应答 Request (PGN 59904) 对上述 PGN 的请求
 *   5. 打印总线上其他节点的 TP.CM (收包重组未实现, 仅提示)
 *
 * IO 数据为模拟值; 实际项目将 io_sample() 替换为真实 GPIO/ADC 读取。
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "j1939.h"

LOG_MODULE_REGISTER(j1939_demo, CONFIG_LOG_DEFAULT_LEVEL);

/* Proprietary B PGN (0xFF00~): 厂商自定义广播区, 不与标准 PGN 冲突 */
#define PGN_IO_STATUS     65280u  /* 0xFF00: 单帧 8B 快照 */
#define PGN_IO_STATUS_EXT 65281u  /* 0xFF01: 16B, 经 TP BAM 发送 */
#define IO_STATUS_PRIO    6u

static uint8_t tx_seq;

/* 模拟 16 路 DI + 4 路 AI (mV) */
static void io_sample(uint16_t di[2], uint16_t ai[4])
{
	di[0] = (uint16_t)(tx_seq * 0x1359u);
	di[1] = (uint16_t)(tx_seq * 0x2468u);
	for (int i = 0; i < 4; i++) {
		ai[i] = (uint16_t)(1500 + (tx_seq * (i + 3)) % 2000);
	}
	tx_seq++;
}

static int send_io_status(void)
{
	uint16_t di[2];
	uint16_t ai[4];
	uint8_t seq = tx_seq;

	io_sample(di, ai);

	/* 单帧: [seq][DI LE16][AI0 LE16][AI1 LE16][保留] */
	uint8_t buf[8] = {
		seq,
		(uint8_t)(di[0] & 0xff), (uint8_t)(di[0] >> 8),
		(uint8_t)(ai[0] & 0xff), (uint8_t)(ai[0] >> 8),
		(uint8_t)(ai[1] & 0xff), (uint8_t)(ai[1] >> 8),
		0xff,
	};

	return j1939_send(IO_STATUS_PRIO, PGN_IO_STATUS,
			  J1939_SA_GLOBAL, buf, sizeof(buf));
}

static int send_io_status_ext(void)
{
	uint16_t di[2];
	uint16_t ai[4];
	uint8_t seq = tx_seq;
	uint8_t buf[16];

	io_sample(di, ai);

	/* 扩展帧: [seq][DI LE16][AI0..3 LE16 x4][uptime 秒 LE32][保留] */
	buf[0] = seq;
	buf[1] = (uint8_t)(di[0] & 0xff);
	buf[2] = (uint8_t)(di[0] >> 8);
	sys_put_le16(ai[0], &buf[3]);
	sys_put_le16(ai[1], &buf[5]);
	sys_put_le16(ai[2], &buf[7]);
	sys_put_le16(ai[3], &buf[9]);
	sys_put_le32((uint32_t)(k_uptime_get() / 1000), &buf[11]);
	buf[15] = 0xff;

	return j1939_send_bam(PGN_IO_STATUS_EXT, buf, sizeof(buf));
}

/* Request (PGN 59904) 应答 */
static bool on_request(uint32_t pgn, uint8_t requester)
{
	switch (pgn) {
	case PGN_IO_STATUS:
		LOG_INF("PGN 65280 requested by SA %u", requester);
		return send_io_status() == 0;
	case PGN_IO_STATUS_EXT:
		LOG_INF("PGN 65281 requested by SA %u", requester);
		return send_io_status_ext() == 0;
	default:
		LOG_INF("PGN %u not supported (from SA %u)", pgn, requester);
		return false;  /* 演示从简: 不回 ACK/NACK (PGN 59392) */
	}
}

/* 演示 PGN 处理器注册: 提示总线上其他节点的多包传输 */
static void on_tp_cm(uint32_t pgn, uint8_t prio, uint8_t sa, uint8_t da,
		     const uint8_t *data, size_t len)
{
	ARG_UNUSED(pgn);
	ARG_UNUSED(prio);
	ARG_UNUSED(da);

	uint8_t ctrl = (len > 0) ? data[0] : 0;

	LOG_INF("TP.CM ctrl 0x%02x from SA %u (RX 重组未实现, 忽略)",
		ctrl, sa);
}

int main(void)
{
	const struct device *can = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
	int ret;

	if (!device_is_ready(can)) {
		LOG_ERR("CAN device %s not ready", can->name);
		return 0;
	}

	/* NAME: 演示用固定值。正式产品应按 J1939/81 选择 Function/
	 * 制造商代码, Identity Number 建议由 MCU 96bit UID 派生以保证唯一 */
	const struct j1939_name name = {
		.function = 40,              /* 演示值 */
		.manufacturer_code = 0x4d2,  /* 演示值 */
		.identity_number = 0x0af12b, /* 演示值 */
	};

	ret = j1939_init(can, j1939_name_pack(&name));
	if (ret != 0) {
		return 0;
	}

	j1939_set_request_handler(on_request);
	j1939_register_handler(J1939_PGN_TP_CM, on_tp_cm);

	/* 地址声明: 失败 (地址耗尽) 则 5s 后重试 */
	while ((ret = j1939_claim_address(CONFIG_J1939_DEMO_PREFERRED_ADDRESS)) != 0) {
		LOG_WRN("address claim failed (%d), retry in 5 s", ret);
		k_sleep(K_SECONDS(5));
	}

	LOG_INF("node up: SA=%u, IO PGN %u/%u, period %d ms",
		j1939_source_address(), PGN_IO_STATUS, PGN_IO_STATUS_EXT,
		CONFIG_J1939_DEMO_TX_PERIOD_MS);

	uint8_t n = 0;

	for (;;) {
		k_sleep(K_MSEC(CONFIG_J1939_DEMO_TX_PERIOD_MS));

		/* 地址被更小 NAME 的节点抢占 -> 重新声明 */
		if (!j1939_address_valid()) {
			LOG_WRN("address lost, re-claiming");
			j1939_claim_address(CONFIG_J1939_DEMO_PREFERRED_ADDRESS);
			continue;
		}

		ret = send_io_status();
		if (ret != 0) {
			LOG_ERR("send failed: %d", ret);
			continue;
		}

		n++;
		if (n % CONFIG_J1939_DEMO_BAM_EVERY_N == 0) {
			send_io_status_ext();
		}
	}

	return 0;
}
