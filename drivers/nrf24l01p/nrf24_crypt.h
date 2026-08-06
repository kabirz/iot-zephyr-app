/** @file nrf24_crypt.h
 *  @brief nRF24L01+ 链路层 XXTEA-CTR 加密 (可选, 编译期开关).
 *
 *  受 CONFIG_NRF24L01P_CRYPT 控制. 开启后所有应用层帧在空口上以
 *  XXTEA-CTR 流密码加密, 密钥由两端共享的 5 字节 rf24 地址派生.
 *
 *  帧格式 (加密开启):
 *    [ctr 1B][ciphertext = XXTEA-CTR(CAN_ID 2B + payload)]
 *  帧格式 (加密关闭): 保持原样 [CAN_ID 2B][payload].
 *
 *  必须在两端 (mod_handler + gateway) 同时开启或同时关闭,
 *  否则接收方解密失败会静默丢弃帧 (链路表现为不通, 不会崩溃).
 */
#ifndef NRF24_CRYPT_H
#define NRF24_CRYPT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** rf24 地址长度 (与 RF24_ADDR_LEN 一致, 此处独立定义避免循环依赖). */
#define NRF24_CRYPT_ADDR_LEN 5

/**
 * @brief 用 rf24 地址派生加密密钥.
 *
 * 开机或地址变更时调用. 内部把 5B 地址循环填充为 16B 作为 XXTEA 密钥.
 * TX/RX 共享同一密钥 (因为两端地址相同).
 *
 * @param addr 5 字节 rf24 地址
 */
void nrf24_crypt_set_key(const uint8_t addr[NRF24_CRYPT_ADDR_LEN]);

/**
 * @brief 加密一帧 (TX 侧, 发送前调用).
 *
 * 输入 buf 内是明文 [CAN_ID 2B BE][payload], 长度 @p plaintext_len (2..31).
 * 原地加密: 在 buf 开头插入 1B ctr, 后续字节原地 XOR 加密.
 * buf 必须至少有 plaintext_len + 1 字节可用空间 (驱动缓冲区 32B 足够).
 *
 * @param buf           帧 buffer (明文输入, 密文原地输出)
 * @param plaintext_len 明文长度 (含 2B CAN ID), 范围 [2, 31]
 * @return 密文总长 (= plaintext_len + 1, ≤32), 或 0 表示参数非法
 */
uint8_t nrf24_crypt_seal(uint8_t *buf, uint8_t plaintext_len);

/**
 * @brief 解密一帧 (RX 侧, 收到后调用).
 *
 * 输入 buf 内是 [ctr 1B][ciphertext], 长度 @p frame_len (3..32).
 * 原地解密: 去掉 ctr 前缀, 后续字节原地 XOR 解密, 结果从 buf[0] 开始.
 *
 * 含滑动窗口重放保护: ctr 必须在期望值的前向窗口内, 否则拒绝.
 *
 * @param buf       帧 buffer (密文输入, 明文原地输出, 向前移动 1 字节)
 * @param frame_len 密文总长 (含 1B ctr), 范围 [3, 32]
 * @return 明文长度 (含 2B CAN ID, = frame_len - 1), 或 0 表示拒绝 (窗口外/重放/参数非法)
 */
uint8_t nrf24_crypt_open(uint8_t *buf, uint8_t frame_len);

/**
 * @brief 重置 RX 滑动窗口状态 (链路重新建立时调用, 可选).
 *
 * 把期望 ctr 置为 0xFF, 使下一帧任意 ctr 值都落入前向窗口.
 * 用于解决两端不同步启动时的初始对齐问题.
 */
void nrf24_crypt_rx_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* NRF24_CRYPT_H */
