/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 极简 SAE J1939 协议栈 (演示级, 基于 Zephyr 原生 CAN API)
 *
 * 实现范围:
 *   - 29 位 ID 编解码 (优先级 / PGN / SA / DA)          (J1939/21)
 *   - PDU1 (点对点) 与 PDU2 (纯广播) 寻址
 *   - 地址声明: 竞争窗口 + NAME 仲裁 + 让步重试 (简化)   (J1939/81)
 *   - PGN 59904 Request 请求-应答分发
 *   - TP BAM 多包发送 (j1939_tp.c)                       (J1939/21)
 *
 * 未实现 (按需扩展): TP 收包重组、RTS/CTS 流控、J1939/73 诊断、
 * J1939/74 配置刷写、地址声明完整定时器组 (TR1~TR7)。
 */
#ifndef J1939_H
#define J1939_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 常用 PGN (详见 J1939-DA) ---------- */
#define J1939_PGN_REQUEST         59904u  /* 0xEA00, PDU1, 请求某 PGN */
#define J1939_PGN_TP_DT           60160u  /* 0xEB00, PDU1, TP 数据帧 */
#define J1939_PGN_TP_CM           60416u  /* 0xEC00, PDU1, TP 控制帧 */
#define J1939_PGN_ADDRESS_CLAIMED 60928u  /* 0xEE00, PDU1, 地址声明 */

/* ---------- 优先级 (0 最高, 7 最低) ---------- */
#define J1939_PRIORITY_HIGHEST 0u
#define J1939_PRIORITY_LOWEST  7u

/* ---------- 特殊源地址 ---------- */
#define J1939_SA_NULL   254u  /* 未获得有效地址 */
#define J1939_SA_GLOBAL 255u  /* 全局广播 */

/* J1939/21 TP 单次传输上限: 255 包 x 7 字节 */
#define J1939_MAX_DATA 1785u

/* ---------- 29 位 ID <-> 协议字段 ----------
 *
 *  28...26 | 25 | 24 | 23.....16 | 15......8 | 7......0
 *  PRI     | R  | DP |    PF     |  PS/DA    |   SA
 */
#define J1939_ID_MASK 0x1fffffffu

static inline bool j1939_pgn_is_pdu2(uint32_t pgn)
{
	/* PF >= 0xF0 为 PDU2 (PS 为组扩展, 只能广播) */
	return ((pgn >> 8) & 0xffu) >= 0xf0u;
}

/* 构造 29 位 ID。PDU1 的 PGN 低位字节约定为 0 (如 0xEA00), DA 填入 PS;
 * PDU2 的组扩展已在 PGN 中, DA 参数被忽略。 */
static inline uint32_t j1939_make_id(uint8_t prio, uint32_t pgn,
				     uint8_t sa, uint8_t da)
{
	uint32_t id = ((uint32_t)(prio & 0x07u) << 26) |
		      ((pgn & 0x3ffffu) << 8) | sa;

	if (!j1939_pgn_is_pdu2(pgn)) {
		id |= (uint32_t)da << 8;
	}
	return id;
}

static inline uint8_t j1939_id_to_priority(uint32_t can_id)
{
	return (uint8_t)((can_id >> 26) & 0x07u);
}

static inline uint8_t j1939_id_to_sa(uint32_t can_id)
{
	return (uint8_t)(can_id & 0xffu);
}

/* PDU1 时返回目标地址 (DA), PDU2 时为组扩展 */
static inline uint8_t j1939_id_to_da(uint32_t can_id)
{
	return (uint8_t)((can_id >> 8) & 0xffu);
}

static inline uint32_t j1939_id_to_pgn(uint32_t can_id)
{
	uint8_t pf = (uint8_t)((can_id >> 16) & 0xffu);

	if (pf >= 0xf0u) {
		return (can_id >> 8) & 0x3ffffu;  /* PDU2: 含组扩展 */
	}
	return (can_id >> 8) & 0x3ff00u;      /* PDU1: 去掉 DA 位 */
}

/* ---------- NAME (64 位设备名, J1939/81) ----------
 * 位域: [0:2] ECU实例 [3:7] 功能实例 [8:15] 功能 [16] 保留
 *       [17:23] 车辆系统 [24:27] 车辆系统实例 [28:30] 行业组 [31] 保留
 *       [32:52] 序列号 [53:63] 制造商代码
 */
struct j1939_name {
	uint8_t ecu_instance;            /* 3 bit */
	uint8_t function_instance;       /* 5 bit */
	uint8_t function;                /* 8 bit, 功能码表见 J1939/81 */
	uint8_t vehicle_system;          /* 7 bit */
	uint8_t vehicle_system_instance; /* 4 bit */
	uint8_t industry_group;          /* 3 bit */
	uint16_t manufacturer_code;      /* 11 bit */
	uint32_t identity_number;        /* 21 bit */
};

static inline uint64_t j1939_name_pack(const struct j1939_name *n)
{
	return (uint64_t)(n->ecu_instance & 0x07u) |
	       ((uint64_t)(n->function_instance & 0x1fu) << 3) |
	       ((uint64_t)n->function << 8) |
	       ((uint64_t)(n->vehicle_system & 0x7fu) << 17) |
	       ((uint64_t)(n->vehicle_system_instance & 0x0fu) << 24) |
	       ((uint64_t)(n->industry_group & 0x07u) << 28) |
	       ((uint64_t)(n->identity_number & 0x1fffffu) << 32) |
	       ((uint64_t)(n->manufacturer_code & 0x7ffu) << 53);
}

/* ---------- 应用回调 ---------- */

/* 收到已注册 PGN 的单帧报文 (TP 长报文不在此分发) */
typedef void (*j1939_rx_handler_t)(uint32_t pgn, uint8_t prio, uint8_t sa,
				   uint8_t da, const uint8_t *data,
				   size_t len);

/* 收到对某 PGN 的 Request; 返回 true 表示已处理并应答 */
typedef bool (*j1939_request_handler_t)(uint32_t pgn, uint8_t requester_sa);

/* ---------- API ---------- */

/* 初始化协议栈: 启动 CAN 控制器, 挂 29 位全捕获过滤器, 启动 RX 线程 */
int j1939_init(const struct device *can, uint64_t name);

/* 阻塞式地址声明 (含竞争窗口, 最长 ~1.75s/地址)。
 * 0 = 成功; -EADDRNOTAVAIL = 地址耗尽 (已广播 Cannot Claim)。 */
int j1939_claim_address(uint8_t preferred_sa);

bool j1939_address_valid(void);
uint8_t j1939_source_address(void);

/* 发送单帧 (len <= 8)。da 仅对 PDU1 PGN 有效, PDU2 忽略。 */
int j1939_send(uint8_t prio, uint32_t pgn, uint8_t da,
	       const uint8_t *data, size_t len);

/* 多包 BAM 广播 (J1939/21), 9 ~ 1785 字节, 阻塞执行
 * (帧间隔 55ms, 16 字节约耗时 110ms)。len <= 8 时退化为单帧。 */
int j1939_send_bam(uint32_t pgn, const uint8_t *data, size_t len);

int j1939_register_handler(uint32_t pgn, j1939_rx_handler_t handler);
void j1939_set_request_handler(j1939_request_handler_t handler);

/* 库内部使用: 不校验地址有效性、显式指定 SA 的裸发送 */
int j1939_send_raw(uint8_t prio, uint32_t pgn, uint8_t sa, uint8_t da,
		   const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* J1939_H */
