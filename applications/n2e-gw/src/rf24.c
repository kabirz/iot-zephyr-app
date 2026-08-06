/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * nRF24L01P 接收模块 - 从 angle-handler 接收数据
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <nrf24l01p.h>
#include <n2e_gw.h>
#ifdef CONFIG_NRF24L01P_CRYPT
#include <nrf24_crypt.h>
#endif

LOG_MODULE_REGISTER(gw_rf24, LOG_LEVEL_INF);

#define RF24_PAYLOAD_MAX 32
#define RF24_ID_SIZE     2
#define RF24_TX_TIMEOUT  K_MSEC(100)
#define RF24_TX_RETRIES    5    /* 半双工冲突退避重试次数 (指数退避) */

static const struct device *rf24_dev = DEVICE_DT_GET(DT_NODELABEL(nrf24));
static K_MUTEX_DEFINE(rf24_tx_mutex);

/* RX 帧 msgq */
K_MSGQ_DEFINE(rf24_rx_msgq, sizeof(struct nrf24_frame), 8, 4);

/* ================================================================
 * RF24 配置
 * ================================================================ */
void gw_rf24_set_config(const uint8_t *addr)
{
	memcpy(gw_params.rf24_addr, addr, RF24_ADDR_LEN);

#ifdef CONFIG_NRF24L01P_CRYPT
	/* 地址变更时更新加密密钥 (5B addr 派生) */
	nrf24_crypt_set_key(addr);
#endif

	if (!device_is_ready(rf24_dev)) {
		return;
	}

	struct nrf24_cfg cfg = {
		.channel = RF24_FIXED_CH,
		.address_width = RF24_ADDR_LEN,
		.tx_addr = gw_params.rf24_addr,
	};

	nrf24_configure(rf24_dev, &cfg);
	nrf24_start_rx(rf24_dev);

	LOG_INF("RF24 config: ch=%d addr=%02x%02x%02x%02x%02x", RF24_FIXED_CH, addr[0], addr[1],
		addr[2], addr[3], addr[4]);
}

/* ================================================================
 * RF24 初始化
 * ================================================================ */
void gw_rf24_init(void)
{
	if (!device_is_ready(rf24_dev)) {
		LOG_ERR("nRF24 device not ready");
		gw_led_error_on();   /* 关键硬件初始化失败, 点亮错误灯 */
		return;
	}

	/* 应用配置 */
	gw_rf24_set_config(gw_params.rf24_addr);

	/* 注册 msgq */
	nrf24_add_rx_msgq(rf24_dev, &rf24_rx_msgq);

	/* 进入 PRX 接收模式 */
	int ret = nrf24_start_rx(rf24_dev);

	if (ret != 0) {
		LOG_ERR("nRF24 start RX failed: %d", ret);
		gw_led_error_on();
		return;
	}
	LOG_INF("nRF24 ready (PRX, ch=%d)", RF24_FIXED_CH);
}

/* ================================================================
 * RF24 发送 (向 angle-handler 发送数据)
 * ================================================================ */
bool gw_rf24_send(uint16_t can_id, const uint8_t *data, size_t len)
{
	if (!device_is_ready(rf24_dev)) {
		return false;
	}
#ifdef CONFIG_NRF24L01P_CRYPT
	/* 加密: [ctr 1B][CAN_ID 2B][payload] ≤ 32 → payload ≤ 29 */
	if (len > RF24_PAYLOAD_MAX - RF24_ID_SIZE - 1) {
		return false;
	}
#else
	if (len > RF24_PAYLOAD_MAX - RF24_ID_SIZE) {
		return false;
	}
#endif

	uint8_t buf[RF24_PAYLOAD_MAX];

	sys_put_be16(can_id, buf);
	if (len > 0) {
		memcpy(buf + RF24_ID_SIZE, data, len);
	}

	uint8_t frame_len = (uint8_t)(len + RF24_ID_SIZE);

#ifdef CONFIG_NRF24L01P_CRYPT
	/* 原地加密: buf 从 [CAN_ID][data] 变为 [ctr][ciphertext], 长度 +1 */
	frame_len = nrf24_crypt_seal(buf, frame_len);
	if (frame_len == 0) {
		return false;
	}
#endif

	struct nrf24_tx_result result;

	/* 半双工冲突退避重试: 双向自发通信时两边可能同时切 PTX, 互相收不到
	 * ACK → MAX_RT 双失败. 失败后指数退避再重试, 打破同步避免再次撞车.
	 * 退避窗口随重试次数翻倍 (3/7/15/31/63ms 上限), 高冲突时逐步拉开
	 * 间距, 比固定窗口更易躲开对端连续 PTX. 退避在 mutex 外, 让出空隙. */
	int ret;

	for (int attempt = 0; ; attempt++) {
		k_mutex_lock(&rf24_tx_mutex, K_FOREVER);
		ret = nrf24_send(rf24_dev, buf, frame_len, RF24_TX_TIMEOUT, &result);
		k_mutex_unlock(&rf24_tx_mutex);

		if (ret == 0 || attempt >= RF24_TX_RETRIES) {
			break;
		}
		/* 指数退避: attempt 0→上限3ms, 1→7, 2→15, 3→31, 4→63; 加 1ms 下限 */
		uint32_t backoff = 1 + (k_uptime_get_32() % ((1U << (attempt + 1)) + 1));

		k_msleep(backoff);
	}

	if (ret != 0) {
		LOG_WRN("nRF24 send failed (id=0x%03x ret=%d tries=%d)", can_id, ret,
			RF24_TX_RETRIES + 1);
		return false;
	}

	if (gw_params.log) {
		LOG_INF("TX id=0x%03x len=%zu acked=%d retrans=%d", can_id, len,
			result.acked, result.retransmits);
	}

	gw_led_rf24_activity();   /* 标记 2.4G 发送活动 (零阻塞) */
	return true;
}

/* ================================================================
 * RF24 接收线程
 * ================================================================ */
static void rf24_rx_thread(void)
{
	if (!device_is_ready(rf24_dev)) {
		LOG_ERR("nRF24 device not ready, RX thread exit");
		return;
	}

	struct nrf24_frame frame;

	while (1) {
		if (k_msgq_get(&rf24_rx_msgq, &frame, K_MSEC(200)) != 0) {
			continue;
		}
		if (frame.len < RF24_ID_SIZE) {
			continue;
		}

#ifdef CONFIG_NRF24L01P_CRYPT
		/* 原地解密: [ctr][ciphertext] → [CAN_ID][plaintext], 长度 -1.
		 * 窗口外/重放帧返回 0, 静默丢弃. */
		uint8_t pt_len = nrf24_crypt_open(frame.data, frame.len);
		if (pt_len == 0) {
			continue;
		}
		frame.len = pt_len;
#endif

		gw_led_rf24_activity();   /* 标记 2.4G 接收活动 (零阻塞) */

		uint16_t can_id = sys_get_be16(frame.data);
		uint8_t data_len = frame.len - RF24_ID_SIZE;
		const uint8_t *data = frame.data + RF24_ID_SIZE;

		if (gw_params.log) {
			LOG_INF("RX id=0x%03x len=%d", can_id, data_len);
		}

		/* 测试帧 (TEST_FRAME): 交给 rf24_shell 处理 (ping/echo/data) */
		if (can_id == TEST_FRAME) {
			rf24_test_handle_rx(data, data_len);
			continue;
		}

		/* 只转发 HANDLER_STATE 和心跳帧 */
		if (can_id != HANDLER_STATE && can_id != COBID_HEATBEAT) {
			continue;
		}

		/* nRF24 数据通过 UDP 转发给上位机 */
		gw_udp_send(frame.data, frame.len);
	}
}

K_THREAD_DEFINE(thread_rf24_rx, CONFIG_N2E_GW_RF24_RX_STACK, rf24_rx_thread, NULL, NULL, NULL,
		CONFIG_N2E_GW_RF24_RX_PRIORITY, 0, 0);
