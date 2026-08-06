/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * n2e-gw application common definitions
 */

#ifndef __N2E_GW_H__
#define __N2E_GW_H__

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <stdbool.h>
#include <stdint.h>

/* ================================================================
 * RF24 地址配置 (信道固定为 1, 不可配)
 * ================================================================ */
#define RF24_ADDR_LEN    5
#define RF24_FIXED_CH    1

/* ================================================================
 * 帧 ID (与 angle-handler 保持一致; nRF24/UDP 帧协议的逻辑标识符,
 * 复用历史 CAN 11-bit 编号, 已与 CAN 总线无关)
 * ================================================================ */
enum can_ids {
	COBID_HEATBEAT = 0x763,
	TEST_FRAME = 0x777,
	HANDLER_STATE = 0x1E3,
	OVERBREAK_LASER = 0x263,
	COORD_XY = 0x363,
	COORD_Z = 0x463,
};

/* UDP 配置命令 (走配置端口 8600, 由 udp_fw_upgrade 库 RX 线程分发到 app_cmd_handler).
 * 0x01-0x05 由库内部处理 (FW_START/DATA/END/GET_VERSION/REBOOT), 不会到达此处.
 * 静态模式下掩码固定 255.255.255.0, 网关 = IP 末段改 1 (a.b.c.1), 均不在帧中传输.
 * DHCP 模式下 IP/掩码/网关由 DHCP 服务器分配, DISCOVER 回复 live interface 地址. */
enum udp_cmd {
	UDP_CMD_SET_IP  = 0x10,   /* [ip 4B] → 回 [1B: 1=成功/0=失败] (持久化, 重启生效) */
	UDP_CMD_GET_NET = 0x11,   /* (空) → [data_port 2B][host_ip 4B][host_port 2B] = 8B */
	UDP_CMD_SET_RF24 = 0x12,   /* [addr 5B] → 回显 5B (信道固定为 1, 不在帧中) */
	UDP_CMD_GET_RF24 = 0x13,   /* (空) → [addr 5B] = 5B */
	UDP_CMD_SET_HOST = 0x14,   /* [host ip 4B][port 2B BE] = 6B → 回显同序 6B (持久化) */
	UDP_CMD_DISCOVER = 0x15,  /* (空) → [ip 4B][config_port 2B] = 6B (广播发现) */
	UDP_CMD_FACTORY_RESET = 0x16, /* (空) → [1B: 1=成功/0=失败] (擦除全部配置后自动重启, 恢复默认) */
};

/* ================================================================
 * 网络默认配置
 * 静态模式: 掩码固定 255.255.255.0, 网关 = IP 末段改 1 (运行时派生, 不存储)
 * DHCP 模式: IP/掩码/网关由 DHCP 服务器分配
 * ================================================================ */
#define N2E_GW_DEFAULT_IP       "192.168.11.100"
#define N2E_GW_DATA_PORT_DEFAULT 9600  /* 数据端口 (本机监听, nRF24↔上位机透传) */
#define GW_USE_DHCP_DEFAULT      0     /* 默认静态 IP (0=静态, 1=DHCP) */
#define N2E_GW_HOST_DEFAULT_IP  "192.168.11.150"  /* 上位机 IP (nRF24 数据转发目标, 可配) */
#define N2E_GW_HOST_PORT_DEFAULT 9602  /* 上位机接收端口 (nRF24 数据转发目标端口, 可配) */

/* ================================================================
 * 全局状态
 * ================================================================ */
typedef struct {
	/* RF24 配置 (信道固定 RF24_FIXED_CH=1, 不存储; 仅地址可配) */
	uint8_t rf24_addr[RF24_ADDR_LEN];

	/* 网络配置 (静态模式下掩码固定 /24, 网关由 IP 派生, 均不存储;
	 *           DHCP 模式下 IP/掩码/网关由 DHCP 分配) */
	char ip_addr[16];     /* 静态 IP (DHCP 模式下仅作 fallback) */
	uint16_t data_port;   /* 数据端口 (可配, 默认 N2E_GW_DATA_PORT_DEFAULT) */
	uint8_t use_dhcp;     /* 0=静态 IP, 1=DHCP (持久化, 重启生效) */

	/* 上位机 (HOST) 目标配置: nRF24 数据固定转发到 host_ip:host_port
	 * (可配, 默认 N2E_GW_HOST_DEFAULT_IP:N2E_GW_HOST_PORT_DEFAULT, 持久化) */
	char host_ip[16];
	uint16_t host_port;

	/* 运行状态 */
	volatile bool running;
	bool log;             /* nRF24 收发详细日志开关 (shell: rf24 log [0/1]) */
	struct k_event event;
} n2e_gw_params_t;

extern n2e_gw_params_t gw_params;

/* 网络链路状态 (main.c 的 NET_EVENT_IF_UP/DOWN 回调维护).
 * gw_udp_send 据此决定是否转发 nRF24 数据到上位机. */
extern volatile bool gw_net_link_up;

/* ================================================================
 * 接口声明
 * ================================================================ */

/* led.c */
void gw_led_init(void);            /* 初始化三路 LED: PA1 rf24常亮收发闪, PA2/PA3灭 */
void gw_led_sys_on(void);          /* 点亮系统灯 PA2 */
void gw_led_error_on(void);        /* 点亮错误灯 PA3 (锁定, 不可灭) */
void gw_led_rf24_activity(void);   /* 标记 2.4G 收发活动 (启动固定频率闪烁) */

/* rf24.c */
void gw_rf24_init(void);
void gw_rf24_set_config(const uint8_t *addr);
bool gw_rf24_send(uint16_t can_id, const uint8_t *data, size_t len);

/**
 * @brief 测试帧 (TEST_FRAME 0x777) 接收回调 (rf24_shell.c 实现)
 */
void rf24_test_handle_rx(const uint8_t *data, uint8_t len);

/* udp.c */
void gw_udp_send(const uint8_t *data, size_t len);

/* 取本机 live IPv4 地址 (DHCP 分配或静态配置的当前地址), 失败返回 NULL.
 * DHCP 模式下 gw_params.ip_addr 是旧静态值, 需从 live interface 读. */
struct in_addr *gw_get_live_ipv4(void);

/* config.c */
void gw_config_save(void);
void gw_config_load(void);

/* persist.c */
void persist_save_rf24_config(void);
void persist_save_network_config(void);

#endif /* __N2E_GW_H__ */
