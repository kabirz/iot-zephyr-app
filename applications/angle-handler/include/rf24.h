/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 2.4G 无线通信接口 (nRF24L01+)
 *
 * 帧格式 (单包 ≤ 32B): [CAN ID 2B BE][payload 0~30B]
 *   - 发送 (手柄→网关): ID = HANDLER_STATE (0x1E3),
 *     payload = [x 2B BE][y 2B BE][btn 1B][0xFF][0xFF][0xFF]
 *   - 接收 (网关→手柄): 按 CAN ID 分发到 mod_can_parse_scanner()
 *     (支持 OVERBREAK_LASER / COORD_XY / COORD_Z)
 */

#ifndef __RF24_H__
#define __RF24_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <common.h>

/**
 * @brief nRF24L01+ 运行时初始化 (进入 PRX 接收模式, CE 拉高)
 *
 * 用于 CAN/2.4G 切换和系统唤醒. 设备由 SYS_INIT(POST_KERNEL) 自动初始化,
 * 本函数仅切换到接收模式.
 */
void rf24_init(void);

/**
 * @brief nRF24L01+ 去初始化 (进入 POWER_DOWN)
 *
 * 用于 CAN/2.4G 切换和系统休眠.
 */
void rf24_deinit(void);

/**
 * @brief 发送一帧 [CAN ID][数据] (2.4G 模式)
 *
 * @param can_id CAN 标识符 (写入帧头 2B BE)
 * @param data   载荷数据 (最多 30 字节)
 * @param len    载荷长度
 * @return true 发送成功, false 长度非法或发送失败
 */
bool rf24_data_send(uint16_t can_id, const uint8_t *data, size_t len);

/**
 * @brief 2.4G 链路状态
 *
 * nRF24L01+ 无硬件链路指示, 简化为常返回 true (假设在线).
 *
 * @return true
 */
bool rf24_get_link_status(void);

/**
 * @brief 测试帧 (TEST_FRAME 0x777) 接收回调 (rf24_shell.c 实现)
 *
 * 由 rf24.c RX 线程在收到 TEST_FRAME 时调用. 载荷为 [sub_cmd 1B][...].
 */
void rf24_test_handle_rx(const uint8_t *data, uint8_t len);

#endif /* __RF24_H__ */
