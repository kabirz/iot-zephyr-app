/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * J1939 核心: CAN 收发 / PGN 分发 / 地址声明 (简化版 J1939/81)
 *
 * 主线 Zephyr 无 J1939 协议栈, 本文件用原生 CAN API 自建:
 *   - 29 位全捕获过滤器 -> msgq -> RX 线程软件分发
 *   - 地址声明: Request 探询 + 0.5~1.5s 竞争窗口 + NAME 数值仲裁,
 *     冲突时让步到下一个更低地址; 未实现完整的 TR1~TR7 定时器组。
 */
#include <errno.h>
#include <string.h>

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include "j1939.h"

LOG_MODULE_REGISTER(j1939, CONFIG_LOG_DEFAULT_LEVEL);

#define RX_STACK_SIZE  1536
#define RX_MSGQ_DEPTH  16
#define RX_THREAD_PRIO 5

#define CLAIM_PRIO                6u
#define CLAIM_CONTENTION_MIN_MS   500u  /* 竞争窗口 0.5~1.5s */
#define CLAIM_CONTENTION_RAND_MS  1000u
#define CLAIM_SETTLE_MS           250u  /* 声明后的观察期 */

/* 应用可注册的 PGN 处理器上限 */
#define MAX_HANDLERS 8

struct j1939_handler {
	uint32_t pgn;
	j1939_rx_handler_t fn;
};

static const struct device *can_dev;
static uint64_t self_name;

/* 地址声明状态: peer_* 仅在 claim_lock 内修改 */
static struct k_mutex claim_lock;
static uint8_t self_sa = J1939_SA_NULL;
static atomic_t addr_valid = ATOMIC_INIT(false);
static uint8_t claim_candidate_sa;  /* 正在尝试的地址 */
static bool peer_seen;              /* 竞争窗口内出现的同地址声明 */
static uint64_t peer_name;

static struct j1939_handler handlers[MAX_HANDLERS];
static j1939_request_handler_t request_handler;

static void j1939_rx_thread(void *p1, void *p2, void *p3);

K_MSGQ_DEFINE(rx_msgq, sizeof(struct can_frame), RX_MSGQ_DEPTH, 4);
K_THREAD_DEFINE(rx_tid, RX_STACK_SIZE, j1939_rx_thread,
		NULL, NULL, NULL, RX_THREAD_PRIO, 0, 0);

/* ---------------- 发送 ---------------- */

int j1939_send_raw(uint8_t prio, uint32_t pgn, uint8_t sa, uint8_t da,
		   const uint8_t *data, size_t len)
{
	struct can_frame frame = {0};

	if (len > 8) {
		return -EMSGSIZE;
	}

	frame.flags = CAN_FRAME_IDE;
	frame.id = j1939_make_id(prio, pgn, sa, da);
	frame.dlc = (uint8_t)len;
	memcpy(frame.data, data, len);

	return can_send(can_dev, &frame, K_MSEC(100), NULL, NULL);
}

int j1939_send(uint8_t prio, uint32_t pgn, uint8_t da,
	       const uint8_t *data, size_t len)
{
	if (!atomic_get(&addr_valid)) {
		return -EADDRNOTAVAIL;
	}

	/* self_sa 为单字节, ARM 上读写原子; 竞态窗口极小, 演示可接受 */
	return j1939_send_raw(prio, pgn, self_sa, da, data, len);
}

/* ---------------- 地址声明 (J1939/81, 简化) ---------------- */

static void handle_address_claimed(uint8_t sa, const uint8_t *data, uint8_t dlc)
{
	if (dlc < 8) {
		return;
	}

	uint64_t name = sys_get_le64(data);

	k_mutex_lock(&claim_lock, K_FOREVER);

	if (sa == self_sa && atomic_get(&addr_valid) && name != self_name) {
		if (name < self_name) {
			/* 对方 NAME 数值更小 -> 仲裁失败, 我方让出地址 */
			atomic_set(&addr_valid, false);
			LOG_WRN("SA %u lost to NAME %08x:%08x, re-claim needed",
				sa, (unsigned int)(name >> 32), (unsigned int)name);
		}
		/* name > self_name: 我方持有地址, 忽略 (对方应让步) */
	} else if (sa == claim_candidate_sa && !atomic_get(&addr_valid)) {
		/* 竞争窗口内发现地址已被占 */
		peer_seen = true;
		peer_name = name;
	}

	k_mutex_unlock(&claim_lock);
}

int j1939_claim_address(uint8_t preferred_sa)
{
	uint8_t nb[8];
	uint8_t req[3] = {
		(uint8_t)(J1939_PGN_ADDRESS_CLAIMED & 0xff),
		(uint8_t)((J1939_PGN_ADDRESS_CLAIMED >> 8) & 0xff),
		(uint8_t)((J1939_PGN_ADDRESS_CLAIMED >> 16) & 0xff),
	};
	uint8_t sa = preferred_sa;
	int ret = -EADDRNOTAVAIL;

	if (preferred_sa > 253) {
		return -EINVAL;
	}

	sys_put_le64(self_name, nb);

	while (sa < 254) {
		k_mutex_lock(&claim_lock, K_FOREVER);
		claim_candidate_sa = sa;
		peer_seen = false;
		k_mutex_unlock(&claim_lock);

		/* 1) 用试探 SA 探询该地址是否已有主人 (Request DA=sa) */
		j1939_send_raw(CLAIM_PRIO, J1939_PGN_REQUEST,
			       sa, sa, req, sizeof(req));

		/* 2) 竞争窗口 0.5~1.5s (xorshift 抖动): 监听占用者的声明 */
		uint32_t r = k_uptime_get_32();

		r ^= r << 13;
		r ^= r >> 17;
		r ^= r << 5;
		k_sleep(K_MSEC(CLAIM_CONTENTION_MIN_MS + r % CLAIM_CONTENTION_RAND_MS));

		/* 3) 发出自己的地址声明 (全局广播) */
		j1939_send_raw(CLAIM_PRIO, J1939_PGN_ADDRESS_CLAIMED,
			       sa, J1939_SA_GLOBAL, nb, sizeof(nb));

		/* 4) 观察期: 更小 NAME 的反诉会置 peer_seen */
		k_sleep(K_MSEC(CLAIM_SETTLE_MS));

		bool lose;

		k_mutex_lock(&claim_lock, K_FOREVER);
		lose = peer_seen && peer_name < self_name;
		claim_candidate_sa = J1939_SA_NULL;
		if (!lose) {
			self_sa = sa;
			atomic_set(&addr_valid, true);
		}
		k_mutex_unlock(&claim_lock);

		if (lose) {
			LOG_INF("SA %u busy, yielding", sa);
			if (sa == 0) {
				break;
			}
			sa--;  /* 让步策略 (简化): 尝试下一个更低地址 */
			continue;
		}

		LOG_INF("address claimed: SA=%u NAME %08x:%08x", sa,
			(unsigned int)(self_name >> 32), (unsigned int)self_name);
		ret = 0;
		break;
	}

	if (ret != 0) {
		/* 地址耗尽: 广播 Address Not Claimed (SA=254) */
		j1939_send_raw(CLAIM_PRIO, J1939_PGN_ADDRESS_CLAIMED,
			       J1939_SA_NULL, J1939_SA_GLOBAL, nb, sizeof(nb));
	}

	return ret;
}

bool j1939_address_valid(void)
{
	return atomic_get(&addr_valid);
}

uint8_t j1939_source_address(void)
{
	return self_sa;
}

/* ---------------- 接收分发 ---------------- */

static void handle_request(uint8_t sa, uint8_t da,
			   const uint8_t *data, uint8_t dlc)
{
	if (dlc < 3) {
		return;
	}

	/* 规范: 只响应 DA 为本节点或全局的请求 */
	if (da != self_sa && da != J1939_SA_GLOBAL) {
		return;
	}

	uint32_t pgn = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
		       ((uint32_t)data[2] << 16);

	if (pgn == J1939_PGN_ADDRESS_CLAIMED) {
		/* 收到对我方地址声明的请求: 立即重播声明 */
		if (atomic_get(&addr_valid)) {
			uint8_t nb[8];

			sys_put_le64(self_name, nb);
			j1939_send_raw(CLAIM_PRIO, J1939_PGN_ADDRESS_CLAIMED,
				       self_sa, J1939_SA_GLOBAL, nb, sizeof(nb));
		}
		return;
	}

	if (request_handler != NULL) {
		request_handler(pgn, sa);
	}
}

static void j1939_rx_thread(void *p1, void *p2, void *p3)
{
	struct can_frame frame;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_msgq_get(&rx_msgq, &frame, K_FOREVER);

		uint32_t can_id = frame.id;
		uint32_t pgn = j1939_id_to_pgn(can_id);
		uint8_t sa = j1939_id_to_sa(can_id);
		uint8_t da = j1939_id_to_da(can_id);
		uint8_t prio = j1939_id_to_priority(can_id);

		if (pgn == J1939_PGN_ADDRESS_CLAIMED) {
			handle_address_claimed(sa, frame.data, frame.dlc);
			continue;
		}

		if (pgn == J1939_PGN_REQUEST) {
			handle_request(sa, da, frame.data, frame.dlc);
			continue;
		}

		for (int i = 0; i < MAX_HANDLERS; i++) {
			if (handlers[i].fn != NULL && handlers[i].pgn == pgn) {
				handlers[i].fn(pgn, prio, sa, da,
					       frame.data, frame.dlc);
			}
		}
	}
}

/* ---------------- 初始化与注册 ---------------- */

int j1939_init(const struct device *can, uint64_t name)
{
	int ret;

	can_dev = can;
	self_name = name;
	k_mutex_init(&claim_lock);

	/* 启动 CAN 控制器 (板级 DTS 已配好 J1939 要求的 250 kbps) */
	ret = can_start(can_dev);
	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("can_start failed: %d", ret);
		return ret;
	}

	/* 29 位全捕获过滤器, PGN 分发在软件层完成。
	 * 若要硬件过滤某个 PDU2 PGN, 只需匹配 ID[25:8]:
	 *   .id = pgn << 8, .mask = 0x3ffff00, .flags = CAN_FILTER_IDE
	 * (PDU1 PGN 的 PS 位是目标地址, DA 会变化, 无法用单一过滤器匹配,
	 *  需为 DA=本机 与 DA=全局 各建一个。) */
	const struct can_filter filter = {
		.id = 0x00000000,
		.mask = 0x00000000,
		.flags = CAN_FILTER_IDE,
	};

	ret = can_add_rx_filter_msgq(can_dev, &rx_msgq, &filter);
	if (ret < 0) {
		LOG_ERR("can_add_rx_filter_msgq failed: %d", ret);
		return ret;
	}

	LOG_INF("J1939 stack ready (bitrate from DTS)");
	return 0;
}

int j1939_register_handler(uint32_t pgn, j1939_rx_handler_t handler)
{
	for (int i = 0; i < MAX_HANDLERS; i++) {
		if (handlers[i].fn == NULL) {
			handlers[i].pgn = pgn;
			handlers[i].fn = handler;
			return 0;
		}
	}

	return -ENOSPC;
}

void j1939_set_request_handler(j1939_request_handler_t handler)
{
	request_handler = handler;
}
