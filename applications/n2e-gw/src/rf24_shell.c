/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 2.4G 无线 (nRF24L01+) 互通测试 shell 命令
 *
 * 与 angle-handler 之间的链路测试:
 *   rf24 info                       查看 addr/device 状态 (信道固定为 1)
 *   rf24 addr <b0 b1 b2 b3 b4>      设置 5 字节地址
 *   rf24 send <text...>             发送 DATA 帧
 *   rf24 ping [count=5] [iv_ms=200] ping/echo 往返测试, 统计 RTT
 *   rf24 listen [on|off]            切换监听 (打印 DATA + 自动回 ECHO)
 *
 * 测试帧: 帧 ID = TEST_FRAME (0x777)
 * 载荷首字节 sub_cmd:
 *   0x01 PING:  [seq]
 *   0x02 ECHO:  [seq][rtt_stamp 2B BE]   (回方原样填充 seq)
 *   0x03 DATA:  [seq][raw bytes...]
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <nrf24l01p.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <n2e_gw.h>

LOG_MODULE_REGISTER(gw_rf24_shell, LOG_LEVEL_INF);

/* 测试帧 sub_cmd */
enum {
	RF24_TEST_PING = 0x01,
	RF24_TEST_ECHO = 0x02,
	RF24_TEST_DATA = 0x03,
};

/* 载荷上限 = NRF24_MAX_PAYLOAD - 2B 帧 ID */
#define RF24_TEST_PAYLOAD_MAX (NRF24_MAX_PAYLOAD - 2)
/* ping 单次等待 echo 超时 */
#define RF24_PING_TIMEOUT_MS  300

static const struct device *const rf24_dev = DEVICE_DT_GET(DT_NODELABEL(nrf24));

/* ping/echo 同步: RX 线程投递 ECHO seq, ping 等待线程消费 */
K_MSGQ_DEFINE(rf24_ping_msgq, sizeof(uint8_t), 2, 1);
static uint8_t ping_wait_seq;       /* 当前等待的 echo seq, 0xFF=无 */
static volatile bool listen_mode;   /* listen on/off */

/* 当前发送序号 */
static uint8_t tx_seq;

/* ================================================================
 * 由 rf24.c RX 线程调用的测试帧分发
 * ================================================================ */
void rf24_test_handle_rx(const uint8_t *data, uint8_t len)
{
	if (len < 1) {
		return;
	}
	uint8_t sub = data[0];

	switch (sub) {
	case RF24_TEST_PING: {
		/* 自动回 ECHO, seq 原样回填. 延时 2ms 让 IRQ 线程排空 RX FIFO 并清
		 * RX_DR (IRQ 引脚回升), 避免紧接的 TX_DS 下降沿与 RX 下降沿重叠被
		 * STM32F1 EXTI 丢掉. */
		uint8_t echo[3] = {RF24_TEST_ECHO, (len >= 2) ? data[1] : 0xFF, 0};

		k_msleep(2);
		gw_rf24_send(TEST_FRAME, echo, sizeof(echo));
		LOG_DBG("PING -> ECHO seq=%02x", echo[1]);
		break;
	}
	case RF24_TEST_ECHO: {
		if (len >= 2) {
			uint8_t seq = data[1];
			/* 仅投递给当前等待方 (无等待方时丢弃) */
			if (ping_wait_seq == seq) {
				k_msgq_put(&rf24_ping_msgq, &seq, K_NO_WAIT);
			}
		}
		break;
	}
	case RF24_TEST_DATA: {
		if (listen_mode) {
			/* [sub][seq][raw...] */
			char text[RF24_TEST_PAYLOAD_MAX + 1];
			uint8_t txt_len = (len >= 2) ? (len - 2) : 0;

			if (txt_len > sizeof(text) - 1) {
				txt_len = sizeof(text) - 1;
			}
			memcpy(text, data + 2, txt_len);
			text[txt_len] = '\0';
			LOG_INF("RF24 RX: %s", text);
		}
		break;
	}
	default:
		LOG_DBG("Unknown test sub_cmd=0x%02x", sub);
		break;
	}
}

/* ================================================================
 * Shell handlers
 * ================================================================ */
static int cmd_rf24_info(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	char addr_str[RF24_ADDR_LEN * 3 + 1] = {0};

	for (int i = 0; i < RF24_ADDR_LEN; i++) {
		snprintf(addr_str + i * 3, 4, "%02x%s", gw_params.rf24_addr[i],
			 (i == RF24_ADDR_LEN - 1) ? "" : " ");
	}
	shell_print(ctx, "device:  %s",
		    device_is_ready(rf24_dev) ? "ready" : "NOT ready");
	shell_print(ctx, "channel: %d (fixed)", RF24_FIXED_CH);
	shell_print(ctx, "addr:    %s", addr_str);
	shell_print(ctx, "listen:  %s", listen_mode ? "on" : "off");
	return 0;
}

static int cmd_rf24_addr(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 6) {
		shell_error(ctx, "usage: rf24 addr <b0> <b1> <b2> <b3> <b4>  (5 hex bytes)");
		return -EINVAL;
	}
	if (!device_is_ready(rf24_dev)) {
		shell_error(ctx, "nRF24 device not ready");
		return -EIO;
	}
	uint8_t addr[RF24_ADDR_LEN];

	for (int i = 0; i < RF24_ADDR_LEN; i++) {
		long v = strtol(argv[i + 1], NULL, 16);

		if (v < 0 || v > 0xFF) {
			shell_error(ctx, "invalid byte: %s", argv[i + 1]);
			return -EINVAL;
		}
		addr[i] = (uint8_t)v;
	}
	gw_rf24_set_config(addr);
	persist_save_rf24_config();

	char addr_str[RF24_ADDR_LEN * 3 + 1] = {0};

	for (int i = 0; i < RF24_ADDR_LEN; i++) {
		snprintf(addr_str + i * 3, 4, "%02x%s", gw_params.rf24_addr[i],
			 (i == RF24_ADDR_LEN - 1) ? "" : " ");
	}
	shell_print(ctx, "addr set to %s", addr_str);
	return 0;
}

static int cmd_rf24_send(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(ctx, "usage: rf24 send <text...>");
		return -EINVAL;
	}
	if (!device_is_ready(rf24_dev)) {
		shell_error(ctx, "nRF24 device not ready");
		return -EIO;
	}

	/* 拼接所有 argv 为一段文本, 留 2B 头给 [sub][seq] */
	uint8_t payload[RF24_TEST_PAYLOAD_MAX];
	size_t off = 2;
	payload[0] = RF24_TEST_DATA;
	payload[1] = tx_seq++;

	for (int i = 1; i < argc && off < sizeof(payload); i++) {
		const char *s = argv[i];

		if (off > 2 && off < sizeof(payload)) {
			payload[off++] = ' ';
		}
		while (*s && off < sizeof(payload)) {
			payload[off++] = (uint8_t)*s++;
		}
	}

	bool ok = gw_rf24_send(TEST_FRAME, payload, off);

	shell_print(ctx, "TX %s seq=%02x len=%zu", ok ? "ok" : "FAIL", payload[1], off);
	return ok ? 0 : -EIO;
}

static int cmd_rf24_ping(const struct shell *ctx, size_t argc, char **argv)
{
	int count = 5;
	int interval_ms = 200;

	if (argc >= 2) {
		count = (int)strtol(argv[1], NULL, 10);
		if (count < 1) {
			count = 1;
		} else if (count > 100) {
			count = 100;
		}
	}
	if (argc >= 3) {
		interval_ms = (int)strtol(argv[2], NULL, 10);
		if (interval_ms < 10) {
			interval_ms = 10;
		} else if (interval_ms > 5000) {
			interval_ms = 5000;
		}
	}
	if (!device_is_ready(rf24_dev)) {
		shell_error(ctx, "nRF24 device not ready");
		return -EIO;
	}

	shell_print(ctx, "ping %d x %dms ...", count, interval_ms);

	int ok = 0;
	uint32_t rtt_sum = 0, rtt_max = 0, rtt_min = UINT32_MAX;

	/* 清空残留 echo */
	k_msgq_cleanup(&rf24_ping_msgq);

	for (int i = 0; i < count; i++) {
		uint8_t seq = tx_seq++;
		uint8_t ping[2] = {RF24_TEST_PING, seq};

		ping_wait_seq = seq;

		uint32_t t0 = k_uptime_get_32();
		bool tx_ok = gw_rf24_send(TEST_FRAME, ping, sizeof(ping));

		if (!tx_ok) {
			shell_print(ctx, "  [%d] TX FAIL seq=%02x", i + 1, seq);
			ping_wait_seq = 0xFF;
			k_msleep(interval_ms);
			continue;
		}

		uint8_t got = 0xFF;
		int ret = k_msgq_get(&rf24_ping_msgq, &got, K_MSEC(RF24_PING_TIMEOUT_MS));

		ping_wait_seq = 0xFF;
		if (ret == 0 && got == seq) {
			uint32_t rtt = k_uptime_get_32() - t0;

			ok++;
			rtt_sum += rtt;
			if (rtt > rtt_max) {
				rtt_max = rtt;
			}
			if (rtt < rtt_min) {
				rtt_min = rtt;
			}
			shell_print(ctx, "  [%d] seq=%02x rtt=%ums", i + 1, seq, rtt);
		} else {
			shell_print(ctx, "  [%d] seq=%02x TIMEOUT", i + 1, seq);
		}
		if (i < count - 1) {
			k_msleep(interval_ms);
		}
	}

	if (ok > 0) {
		shell_print(ctx, "result: %d/%d ok, rtt avg=%ums max=%ums min=%ums", ok, count,
			    rtt_sum / ok, rtt_max, rtt_min);
	} else {
		shell_print(ctx, "result: 0/%d ok (all timeout)", count);
	}
	return 0;
}

static int cmd_rf24_listen(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(ctx, "listen: %s", listen_mode ? "on" : "off");
		return 0;
	}
	if (strcmp(argv[1], "on") == 0) {
		listen_mode = true;
	} else if (strcmp(argv[1], "off") == 0) {
		listen_mode = false;
	} else {
		shell_error(ctx, "usage: rf24 listen [on|off]");
		return -EINVAL;
	}
	shell_print(ctx, "listen: %s", listen_mode ? "on" : "off");
	return 0;
}

/* rf24 diag: 通过 LOG_INF 输出一份完整硬件寄存器快照, 用于定位 SPI/射频问题 */
static int cmd_rf24_diag(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(ctx, "dumping nRF24 registers (see LOG_INF output)...");
	nrf24_dump_regs(rf24_dev);
	return 0;
}

/* rf24 log [0/1]: 开关 nRF24 收发详细日志 (TX/RX id + len + acked/retrans) */
static int cmd_rf24_log(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(ctx, "rf24 log: %s", gw_params.log ? "on" : "off");
		return 0;
	}
	gw_params.log = (atoi(argv[1]) != 0);
	shell_print(ctx, "rf24 log: %s", gw_params.log ? "on" : "off");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_rf24_cmds,
	SHELL_CMD_ARG(info, NULL, "Show RF24 addr/state", cmd_rf24_info, 1, 0),
	SHELL_CMD_ARG(addr, NULL, "Set 5-byte addr <b0 b1 b2 b3 b4> (hex)", cmd_rf24_addr, 6, 0),
	SHELL_CMD_ARG(send, NULL, "Send DATA frame <text...>", cmd_rf24_send, 2, 15),
	SHELL_CMD_ARG(ping, NULL, "Ping test [count=5] [interval_ms=200]", cmd_rf24_ping, 1, 2),
	SHELL_CMD_ARG(listen, NULL, "Listen mode [on|off] (auto echo + print DATA)", cmd_rf24_listen,
		      1, 1),
		SHELL_CMD_ARG(diag, NULL, "Dump nRF24 hardware registers", cmd_rf24_diag, 1, 0),
		SHELL_CMD_ARG(log, NULL, "Enable/disable RF24 tx/rx debug log [0/1]", cmd_rf24_log, 1, 1),
		SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(rf24, &sub_rf24_cmds, "RF24 (nRF24L01+) link test", NULL);
