/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * io-edge-hub 公共定义: IO 通道数、Modbus 寄存器枚举、历史数据结构、
 * 全局访问函数声明。
 *
 * holding_reg[] / input_reg[] 是唯一的参数与采样数据源:
 *   - settings (FCB) 直接映射 holding_reg[] (modbus/ 命名空间)
 *   - DI/AI 采样线程写入 input_reg[]
 *   - DO 写 holding_reg[0x00] 经回调驱动 GPIO + LED
 */

#ifndef __INIT_H__
#define __INIT_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== IO 通道数量 ==================== */
#define DI_NUM    16	/* 数字输入 */
#define DO_NUM    8	/* 数字输出 */
#define AI_NUM    4	/* 模拟输入 */

/* ==================== Input Registers (只读, 6 个) ==================== */
enum input_reg_idx {
	INPUT_VER_IDX = 0,	/* 固件版本 (major<<8 | minor) */
	INPUT_AI0_IDX,		/* AI1 电流 (0.01mA) */
	INPUT_AI1_IDX,		/* AI2 电流 (0.01mA) */
	INPUT_AI2_IDX,		/* AI3 电压 (0.01V) */
	INPUT_AI3_IDX,		/* AI4 电压 (0.01V) */
	INPUT_DI_IDX,		/* DI1-16 状态 bitmap */
};

/* ==================== Holding Registers (读写, 21 个) ==================== */
enum holding_reg_idx {
	HOLDING_DO_IDX = 0x00,		/* DO1-8 输出控制 */
	HOLDING_DI_EN_IDX,		/* 0x01 DI1-16 使能 */
	HOLDING_AI_EN_IDX,		/* 0x02 AI1-4 使能 */
	HOLDING_DI_SI_IDX,		/* 0x03 DI 采样间隔 (ms) */
	HOLDING_AI_SI_IDX,		/* 0x04 AI 采样间隔 (ms) */
	HOLDING_HIS_SAVE_IDX,		/* 0x05 历史保存使能 */
	HOLDING_CAN_ID_IDX,		/* 0x06 CAN ID */
	HOLDING_CAN_BPS_IDX,		/* 0x07 CAN 波特率 (x1000) */
	HOLDING_RS485_BPS_IDX,		/* 0x08 RS485 波特率 */
	HOLDING_SLAVE_ID_IDX,		/* 0x09 Modbus RTU Slave ID */
	HOLDING_IP_ADDR_1_IDX,		/* 0x0A IP 段1 */
	HOLDING_IP_ADDR_2_IDX,		/* 0x0B IP 段2 */
	HOLDING_IP_ADDR_3_IDX,		/* 0x0C IP 段3 */
	HOLDING_IP_ADDR_4_IDX,		/* 0x0D IP 段4 */
	HOLDING_TIMESTAMPH_IDX,		/* 0x0E 时间戳高16位 */
	HOLDING_TIMESTAMPL_IDX,		/* 0x0F 时间戳低16位 */
	HOLDING_CFG_SAVE_IDX,		/* 0x10 参数保存触发 */
	HOLDING_REBOOT_IDX,		/* 0x11 写1触发重启 */
	HOLDING_HEART_EN_IDX,		/* 0x12 心跳使能 */
	HOLDING_HEART_TIMEOUT_IDX,	/* 0x13 心跳超时 (ms) */
	HOLDING_HEART_IDX,		/* 0x14 心跳值 (写1喂狗) */
};

/* ==================== 历史数据结构 (与 RT-Thread / PC 解析工具兼容) ==================== */
#define DI_TYPE 1
#define AI_TYPE 2

struct his_data {
	uint16_t type;		/* 1=DI, 2=AI */
	uint32_t timestamps;	/* Unix 时间戳 */
	union {
		struct {
			uint16_t di_en_status;	/* DI 使能 bitmap */
			uint16_t di_value;	/* DI 值 bitmap */
		} di;
		struct {
			uint16_t ai_en_status;		/* AI 使能 bitmap (低4位) */
			uint16_t ai_value[AI_NUM];	/* AI 值数组 */
		} ai;
	};
} __packed;

/* ==================== 寄存器访问 (function.c) ==================== */
uint16_t get_holding_reg(uint16_t addr);
int update_holding_reg(uint16_t addr, uint16_t reg);
uint16_t get_input_reg(uint16_t addr);
int update_input_reg(uint16_t addr, uint16_t reg);

/* 触发参数全量保存到 FCB (供 UDP handler 改参数后持久化) */
void holding_reg_save(void);

/* 出厂恢复: 擦除 settings 分区并重置 holding_reg 为默认值 */
int settings_factory_reset(void);

/* ==================== DO 控制 (dio.c) ==================== */
/* 设置 DO 输出 + LED 联动, val bit0-7 对应 DO1-DO8 */
int mb_set_do(uint16_t val);

/* ==================== 心跳看门狗 (init.c) ==================== */
/* Modbus TCP 收到请求时调用, 重置心跳定时器 */
void heart_event_send(void);

/* ==================== 时间管理 (time.c) ==================== */
/* 设置 RTC + 系统时钟 (Modbus 0x0E/0x0F 或 UDP 命令调用) */
void set_timestamp(time_t t);

/* ==================== 历史记录 (history.c) ==================== */
/* 开关历史写入 (holding_reg[0x05] 回调调用) */
void history_enable_write(bool en);
/* 异步提交一条历史记录 (DI/AI 采样线程调用) */
void send_history_data(const struct his_data *data);

/* ==================== 网络状态 (main.c) ==================== */
/* 网络链路是否就绪 (IF_UP/IF_DOWN 维护) */
bool net_link_is_up(void);

#ifdef __cplusplus
}
#endif

#endif /* __INIT_H__ */
