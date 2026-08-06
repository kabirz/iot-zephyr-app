/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 2.4G 无线通信 (nRF24L01+) — 中断驱动接收 + 请求-响应半双工
 *
 * 收发协调:
 *   nRF24 驱动内部 IRQ 线程排空 RX FIFO, 通过 rf24_rx_msgq 投递帧;
 *   本模块 RX 线程从 msgq 取帧并按 CAN ID 分发到扫描仪解析器.
 *   发送时 nrf24_send 内部切 PTX, 等待 ACK 后自动切回 PRX, 返回耗时与重传次数.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>
#include <nrf24l01p.h>
#include <rf24.h>
#include <mod-can.h>
#include <mod-gpio.h>
#include <display.h>
#ifdef CONFIG_NRF24L01P_CRYPT
#include <nrf24_crypt.h>
#endif

LOG_MODULE_REGISTER(rf24_radio, LOG_LEVEL_INF);

#define RF24_PAYLOAD_MAX  32
#define RF24_ID_SIZE      2
#define RF24_TX_TIMEOUT   K_MSEC(100)
#define RF24_TX_RETRIES   5     /* 半双工冲突退避重试次数 (指数退避) */

static const struct device *rf24_dev = DEVICE_DT_GET(DT_NODELABEL(nrf24));
static K_MUTEX_DEFINE(rf24_tx_mutex); /* 序列化多线程 TX (adc + gpio 按键) */

/* RX 帧 msgq: nRF24 驱动 IRQ 线程投递, 本模块 RX 线程消费 */
K_MSGQ_DEFINE(rf24_rx_msgq, sizeof(struct nrf24_frame), 8, 4);

/* ================================================================
 * 信号质量统计 (EMA, 无浮点)
 * nRF24L01+ 无 RSSI 寄存器, 用本地 TX 统计估算链路质量:
 *   retransmits (OBSERVE_TX.ARC_CNT, 0-15) + MAX_RT 失败
 * 映射 0-4 档, 对应 LoRa 时代 SignalQuality_t
 * (NONE/BAD/FAIR/GOOD/EXCELLENT) 与信号图标 signal_levels[5].
 * ================================================================ */
#define RF24_QUAL_ALPHA_NUM  1                     /* α = 1/8, 平滑去抖 (原 3/8 对单次冲突波动太敏感) */
#define RF24_QUAL_ALPHA_DEN  8
#define RF24_QUAL_FAIL_DROP  (2 * RF24_QUAL_ALPHA_DEN) /* 每次 MAX_RT 失败下拉 2 档 */
#define RF24_QUAL_INIT       (4 * RF24_QUAL_ALPHA_DEN) /* 初始满档 (×DEN) */

static int16_t rf24_quality = RF24_QUAL_INIT; /* 定点 EMA 累积值, level = /DEN */

/* 单次 TX -> 0-4 档评分 (MAX_RT 失败恒为 0; 成功按重传次数分档) */
static uint8_t rf24_tx_level(bool acked, uint8_t retrans)
{
	if (!acked) {
		return 0;       /* MAX_RT 重传耗尽: 最差 */
	}
	if (retrans <= 1) {
		return 4;       /* 0-1 次: 极好 */
	}
	if (retrans <= 4) {
		return 3;       /* 2-4 次: 良好 */
	}
	if (retrans <= 9) {
		return 2;       /* 5-9 次: 一般 */
	}
	return 1;               /* 10-15 次: 差但勉强通 */
}

/* 用本次 TX 结果更新 EMA 并在档位变化时刷新显示 */
static void rf24_update_rssi(bool acked, uint8_t retrans)
{
	uint8_t lv = rf24_tx_level(acked, retrans);

	rf24_quality = (lv * RF24_QUAL_ALPHA_DEN * RF24_QUAL_ALPHA_NUM
			+ rf24_quality * (RF24_QUAL_ALPHA_DEN - RF24_QUAL_ALPHA_NUM))
		       / RF24_QUAL_ALPHA_DEN;
	if (!acked) {
		/* 失败快速下拉, 避免 EMA 对掉线响应太慢 (≈3 次收敛到 0) */
		rf24_quality -= RF24_QUAL_FAIL_DROP;
		if (rf24_quality < 0) {
			rf24_quality = 0;
		}
	}

	/* 定点值 rf24_quality (满档 RF24_QUAL_INIT=32) → 0-4 档。
	 * 不用 q/8 整数除法 (满档只对应 q==32 单点, 偶发重传即跌落且难爬回);
	 * 改为分段映射, 满档窗口 q>=28 (约稳态的 87%), 通信良好即可稳定显示满档。 */
	uint8_t level;

	if (rf24_quality >= 28) {
		level = 4;
	} else if (rf24_quality >= 20) {
		level = 3;
	} else if (rf24_quality >= 12) {
		level = 2;
	} else if (rf24_quality >= 4) {
		level = 1;
	} else {
		level = 0;
	}
	if (global_params.rssi != level) {
		global_params.rssi = level;
		mod_display_rf24(level);
	}
}

void rf24_init(void)
{
	if (!device_is_ready(rf24_dev)) {
		LOG_ERR("nRF24 device not ready");
		return;
	}

	/* 上电 + 等 POR + 重新配置 + 进 PRX (电源由驱动按 power-gpios 管理) */
	nrf24_power_enable(rf24_dev, true);

	/* 应用持久化的地址配置 (信道固定为 RF24_FIXED_CH) */
	{
		struct nrf24_cfg cfg = {
			.channel = RF24_FIXED_CH,
			.address_width = RF24_ADDR_LEN,
			.tx_addr = global_params.rf24_addr,
		};

#ifdef CONFIG_NRF24L01P_CRYPT
		/* 用 rf24 地址派生加密密钥 (与 n2e-gw 共享同一地址 → 同一密钥) */
		nrf24_crypt_set_key(global_params.rf24_addr);
		nrf24_crypt_rx_reset();
#endif

		nrf24_configure(rf24_dev, &cfg);
		LOG_INF("nRF24 configured: ch=%d addr=%02x%02x%02x%02x%02x",
			RF24_FIXED_CH,
			global_params.rf24_addr[0], global_params.rf24_addr[1],
			global_params.rf24_addr[2], global_params.rf24_addr[3],
			global_params.rf24_addr[4]);
	}

	/* 注册 msgq: 驱动 IRQ 线程收到帧后投递到这里 */
	nrf24_add_rx_msgq(rf24_dev, &rf24_rx_msgq);
	/* 进入 PRX 接收模式并拉高 CE */
	int ret = nrf24_start_rx(rf24_dev);

	if (ret != 0) {
		LOG_ERR("nRF24 start RX failed: %d", ret);
		return;
	}
	LOG_INF("nRF24L01+ ready (PRX, irq-driven rx)");

	/* 复位信号质量 EMA 为满档, 避免上一会话残留低档影响新连接 */
	rf24_quality = RF24_QUAL_INIT;
}

void rf24_deinit(void)
{
	if (!device_is_ready(rf24_dev)) {
		return;
	}
	/* 软关机 (POWER_DOWN) + 断电, 保护 SPI 引脚 (驱动管理电源) */
	nrf24_power_enable(rf24_dev, false);
	LOG_INF("nRF24L01+ powered down");
}

bool rf24_data_send(uint16_t can_id, const uint8_t *data, size_t len)
{
	if (!device_is_ready(rf24_dev)) {
		return false;
	}
#ifdef CONFIG_NRF24L01P_CRYPT
	/* 加密: [ctr 1B][CAN_ID 2B][payload] ≤ 32 → payload ≤ 29 */
	if (len > RF24_PAYLOAD_MAX - RF24_ID_SIZE - 1) {
		LOG_ERR("Payload too large: %zu (max %d)", len,
			RF24_PAYLOAD_MAX - RF24_ID_SIZE - 1);
		return false;
	}
#else
	if (len > RF24_PAYLOAD_MAX - RF24_ID_SIZE) {
		LOG_ERR("Payload too large: %zu (max %d)", len, RF24_PAYLOAD_MAX - RF24_ID_SIZE);
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
	 * ACK → MAX_RT 双失败. 失败后随机退避再重试, 打破同步避免再次撞车.
	 * 退避放在 mutex 外, 释放 TX 锁让其他线程可发, 且让出空隙给对端. */
	int ret;

	for (int attempt = 0; ; attempt++) {
		k_mutex_lock(&rf24_tx_mutex, K_FOREVER);
		ret = nrf24_send(rf24_dev, buf, frame_len, RF24_TX_TIMEOUT, &result);
		k_mutex_unlock(&rf24_tx_mutex);

		if (ret == 0 || attempt >= RF24_TX_RETRIES) {
			break;
		}
		/* 指数退避: attempt 0→上限3ms, 1→7, 2→15, 3→31, 4→63; 加 1ms 下限.
		 * 退避中的失败是暂时性冲突, 不更新 RSSI —— 否则一次发送会多次拉低
		 * EMA, 冲突频繁时信号条剧烈抖动. 只用最终结果评估链路. */
		uint32_t backoff = 1 + (k_uptime_get_32() % ((1U << (attempt + 1)) + 1));

		k_msleep(backoff);
	}

	if (ret != 0) {
		LOG_WRN("nRF24 send failed (id=0x%03x ret=%d tries=%d)", can_id, ret,
			RF24_TX_RETRIES + 1);
		rf24_update_rssi(result.acked, result.retransmits);
		return false;
	}
	if (global_params.log) {
		LOG_INF("TX id=0x%03x acked=%d %ums retrans=%d", can_id, result.acked,
			result.elapsed_ms, result.retransmits);
	}
	rf24_update_rssi(result.acked, result.retransmits);
	return true;
}

bool rf24_get_link_status(void)
{
	return true;
}

/* ================================================================
 * 2.4G 接收线程 — 从 rf24_rx_msgq 取帧, 按 CAN ID 分发
 *
 * 帧: [CAN ID 2B BE][payload]
 * 支持 OVERBREAK_LASER / COORD_XY / COORD_Z, 复用 mod_can_parse_scanner().
 * ================================================================ */
static void rf24_rx_thread(void)
{
	if (!device_is_ready(rf24_dev)) {
		LOG_ERR("nRF24 device not ready, RX thread exit");
		return;
	}

	struct nrf24_frame frame;

	while (true) {
		if (atomic_get(&global_params.sleeping)) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}
		/* 仅 2.4G 模式下接收 */
		k_event_wait(&global_params.event, RF24_EVENT, false, K_FOREVER);

		/* 阻塞等待驱动 IRQ 线程投递的帧 */
		if (k_msgq_get(&rf24_rx_msgq, &frame, K_MSEC(200)) != 0) {
			continue;
		}
		if (frame.len < RF24_ID_SIZE) {
			LOG_WRN("nRF24 frame too short: %u", frame.len);
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

		uint16_t can_id = sys_get_be16(frame.data);
		uint8_t data_len = frame.len - RF24_ID_SIZE;

		if (global_params.log) {
			LOG_INF("RX id=0x%03x len=%d", can_id, data_len);
		}

		/* 测试帧 (TEST_FRAME): 交给 rf24_shell 处理 (ping/echo/data) */
		if (can_id == TEST_FRAME) {
			rf24_test_handle_rx(frame.data + RF24_ID_SIZE, data_len);
			continue;
		}

		/* 仅处理扫描仪数据帧, 其它 ID 忽略 */
		if (can_id != OVERBREAK_LASER && can_id != COORD_XY && can_id != COORD_Z) {
			LOG_DBG("Ignore 2.4G frame id=0x%03x", can_id);
			continue;
		}

		struct can_frame cf = {
			.id = can_id,
			.dlc = can_bytes_to_dlc(data_len),
		};
		memcpy(cf.data, frame.data + RF24_ID_SIZE, data_len);
		mod_can_parse_scanner(&cf);
	}
}
K_THREAD_DEFINE(thread_rf24_rx, 1024, rf24_rx_thread, NULL, NULL, NULL, 8, 0, 0);
