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
	INPUT_VER_IDX = 0,	/* 固件版本 (MAJOR<<12 | MINOR<<8 | PATCH), 主/次<16 */
	INPUT_AI0_IDX,		/* AI1 电流 (0.01mA) */
	INPUT_AI1_IDX,		/* AI2 电流 (0.01mA) */
	INPUT_AI2_IDX,		/* AI3 电压 (0.01V) */
	INPUT_AI3_IDX,		/* AI4 电压 (0.01V) */
	INPUT_DI_IDX,		/* DI1-16 状态 bitmap */
};

/* ==================== Holding Registers (读写, 18 个) ==================== */
enum holding_reg_idx {
	HOLDING_DO_IDX = 0x00,		/* DO1-8 输出控制 */
	HOLDING_DI_ENABLE_IDX,		/* 0x01 DI1-16 使能 */
	HOLDING_AI_ENABLE_IDX,		/* 0x02 AI1-4 使能 */
	HOLDING_DI_SAMPLE_MS_IDX,		/* 0x03 DI 采样间隔 (ms) */
	HOLDING_AI_SAMPLE_MS_IDX,		/* 0x04 AI 采样间隔 (ms) */
	HOLDING_HISTORY_ENABLE_IDX,		/* 0x05 历史保存使能 */
	HOLDING_CAN_ID_IDX,		/* 0x06 CAN ID */
	HOLDING_CAN_BAUDRATE_IDX,		/* 0x07 CAN 波特率 (x1000) */
	HOLDING_RS485_BAUDRATE_IDX,		/* 0x08 RS485 波特率 */
	HOLDING_SLAVE_ID_IDX,		/* 0x09 Modbus RTU Slave ID */
	HOLDING_IP_OCTET1_IDX,		/* 0x0A IP 段1 */
	HOLDING_IP_OCTET2_IDX,		/* 0x0B IP 段2 */
	HOLDING_IP_OCTET3_IDX,		/* 0x0C IP 段3 */
	HOLDING_IP_OCTET4_IDX,		/* 0x0D IP 段4 */
	HOLDING_TIMESTAMP_HI_IDX,		/* 0x0E 时间戳高16位 */
	HOLDING_TIMESTAMP_LO_IDX,		/* 0x0F 时间戳低16位 */
	HOLDING_CONFIG_SAVE_IDX,		/* 0x10 参数保存触发 */
	HOLDING_REBOOT_IDX,		/* 0x11 写1触发重启 */
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

/* 读 holding 寄存器 (与 Modbus FC03 同语义): 时间戳 0x0E/0x0F 返回
 * 实时系统时间, 供 Web /api/regs 与主站读到一致值 */
uint16_t io_read_holding(uint16_t addr);

/* 写 holding 寄存器 (带副作用: DO 输出/历史开关/设时间/保存参数/重启),
 * 与 Modbus FC06/FC16 同语义, 供 Web (HTTP/WS) 共用 */
int io_write_holding(uint16_t addr, uint16_t reg);

/* 单 DO 位写 (加锁读-改-写, 与 Modbus FC05 同语义), bit 0-7 对应 DO1-DO8 */
int io_write_do_bit(uint16_t bit, bool state);

/* 触发参数全量保存到 FCB (供 UDP handler 改参数后持久化) */
void holding_reg_save(void);

/* ==================== IP 合法性校验 ==================== */
/* IPv4 四段是否为可配置的单播地址。拒绝:
 *   - 末字节 0 (网络地址) / 0xFF (广播)
 *   - 首字节 0 (本网络) / 127 (环回) / 224-239 (组播 D 段) / >=240 (保留 E 段)
 */
bool ip_addr_valid(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

/* 出厂恢复: 擦除 settings 分区并重置 holding_reg 为默认值 */
int settings_factory_reset(void);

/* ==================== DO 控制 (dio.c) ==================== */
/* 设置 DO 输出 + LED 联动, val bit0-7 对应 DO1-DO8 */
int mb_set_do(uint16_t val);

/* ==================== 时间管理 (time.c) ==================== */
/* 设置 RTC + 系统时钟 (Modbus 0x0E/0x0F 或 UDP 命令调用).
 * 返回 true=成功, false=参数越界或 RTC 写入失败. */
bool set_timestamp(time_t t);

/* ==================== 历史记录 (history.c) ==================== */
/* 开关历史写入 (holding_reg[0x05] 回调调用) */
void history_enable_write(bool en);
/* 刷出缓存: FTP/HTTP 读文件前调用, 确保数据落盘 */
void history_sync(void);
/* 异步提交一条历史记录 (DI/AI 采样线程调用) */
void send_history_data(const struct his_data *data);

/* ==================== 网络状态 (main.c) ==================== */
/* 网络链路是否就绪 (IF_UP/IF_DOWN 维护) */
bool net_link_is_up(void);

/* ==================== 延迟重启 (main.c) ==================== */
/* 设置延迟重启标志 (主循环刷新日志后重启), 供 UDP 改 IP 等调用 */
void set_reboot_status(bool en);
bool get_reboot_status(void);

/* ==================== Web 服务 (web/httpd.c) ==================== */
#ifdef CONFIG_IO_WEB
/* 启动 HTTP + WebSocket 服务 (main 在网络链路就绪后调用) */
int io_web_start(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __INIT_H__ */
