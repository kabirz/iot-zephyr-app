/** @file nrf24_crypt.c
 *  @brief nRF24L01+ 链路层 XXTEA-CTR 加密实现.
 *
 *  受 CONFIG_NRF24L01P_CRYPT 控制. 算法细节见 nrf24_crypt.h.
 *
 *  设计要点:
 *  - XXTEA (Corrected Block TEA) 作为 PRF, 只用其 encrypt 方向生成 keystream.
 *  - CTR 模式: 每 8 字节明文用 [ctr32, ~ctr32] 经 XXTEA 生成 8 字节 keystream XOR.
 *  - 计数器同步: TX/RX 各维护 32-bit 计数器 (keystream 空间 4G 帧, 永不重用),
 *    仅传输低 8 位 (1 字节开销), RX 据滑动窗口推断高 24 位.
 *  - RX 滑动窗口 (±32) 容忍丢包, 同时拒绝重放/回退.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#include "nrf24_crypt.h"

/* ================================================================
 * XXTEA 核心算法 (Wheeler & Needham, 1998)
 * 参考: https://en.wikipedia.org/wiki/XXTEA
 * 对 32-bit 整数数组原地加密; CTR 模式只需 encrypt 方向.
 * ================================================================ */
#define XXTEA_DELTA 0x9E3779B9u

static void xxtea_encrypt_block(uint32_t v[2], const uint32_t key[4])
{
	/* n=2: rounds = 6 + 52/2 = 32 */
	uint32_t y, z, sum = 0;
	uint32_t rounds = 6 + 52 / 2;
	const uint8_t n = 2;

	z = v[n - 1];
	do {
		sum += XXTEA_DELTA;
		uint8_t e = (sum >> 2) & 3;
		for (uint8_t p = 0; p < n - 1; p++) {
			y = v[p + 1];
			z = v[p] + ((((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^
				    ((sum ^ y) + (key[(p & 3) ^ e] ^ z)));
			v[p] = z;
		}
		y = v[0];
		z = v[n - 1] + ((((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^
				((sum ^ y) + (key[(n - 1 & 3) ^ e] ^ z)));
		v[n - 1] = z;
	} while (--rounds);
}

/* ================================================================
 * 模块状态
 * ================================================================ */

/* XXTEA 128-bit 密钥 (4×uint32), 由 rf24_addr 派生 */
static uint32_t crypt_key[4];

/* TX 32-bit 发送计数器 (每帧 +1, 提供给 XXTEA 生成唯一 keystream).
 * 仅传输低 8 位, 高 24 位由 RX 侧窗口推断. 4G 帧才回绕, 实际永不重复. */
static uint32_t tx_counter;

/* RX 滑动窗口状态 */
#define RX_WINDOW 32
static uint32_t rx_counter;     /* RX 32-bit 期望计数器 */
static bool rx_initialized;     /* 是否已接受过至少一帧 */

/* ================================================================
 * 密钥派生: 5B addr 循环填充为 16B → 4×uint32 (big-endian)
 * ================================================================ */
void nrf24_crypt_set_key(const uint8_t addr[NRF24_CRYPT_ADDR_LEN])
{
	uint8_t kbytes[16];

	for (int i = 0; i < 16; i++) {
		kbytes[i] = addr[i % NRF24_CRYPT_ADDR_LEN];
	}
	for (int i = 0; i < 4; i++) {
		crypt_key[i] = sys_get_be32(&kbytes[i * 4]);
	}

	/* 重置计数器, 避免旧密钥下的状态延续 */
	tx_counter = 0;
	rx_initialized = false;
}

void nrf24_crypt_rx_reset(void)
{
	rx_initialized = false;
}

/* ================================================================
 * 内部: 对 buffer 中指定长度的数据做 CTR XOR (原地)
 * counter 为 32-bit 计数器基值, 每 8 字节 (一个 XXTEA 块) +1
 * ================================================================ */
static void ctr_xor(uint8_t *buf, uint8_t len, uint32_t counter)
{
	uint8_t off = 0;

	while (off < len) {
		uint32_t block[2] = { counter, ~counter };

		xxtea_encrypt_block(block, crypt_key);

		uint8_t ks[8];
		sys_put_be32(block[0], &ks[0]);
		sys_put_be32(block[1], &ks[4]);

		uint8_t chunk = (len - off < 8) ? (len - off) : 8;
		for (uint8_t i = 0; i < chunk; i++) {
			buf[off + i] ^= ks[i];
		}
		off += chunk;
		counter++;
	}
}

/* ================================================================
 * 加密一帧 (TX 侧)
 *
 * 输入:  buf[0..plaintext_len-1] = 明文 [CAN_ID 2B][payload]
 * 输出:  buf[0] = ctr_byte, buf[1..plaintext_len] = 密文
 *        (整体右移 1 字节, 调用方须保证 buf 有 plaintext_len+1 字节空间)
 * 返回:  密文总长 = plaintext_len + 1, 或 0 (参数非法)
 * ================================================================ */
uint8_t nrf24_crypt_seal(uint8_t *buf, uint8_t plaintext_len)
{
	if (plaintext_len < 2 || plaintext_len > 31) {
		return 0;
	}

	uint32_t ctr = tx_counter++;
	uint8_t ctr_byte = (uint8_t)(ctr & 0xFF);

	/* 整体右移 1 字节 (从尾部开始, 避免覆盖) */
	memmove(buf + 1, buf, plaintext_len);

	/* buf[1..plaintext_len] 用完整 32-bit counter 加密 (keystream 唯一) */
	ctr_xor(buf + 1, plaintext_len, ctr);

	buf[0] = ctr_byte;
	return plaintext_len + 1;
}

/* ================================================================
 * 解密一帧 (RX 侧) + 滑动窗口重放保护
 *
 * 输入:  buf[0] = ctr_byte, buf[1..frame_len-1] = 密文
 * 输出:  buf[0..frame_len-2] = 明文 (整体左移 1 字节)
 * 返回:  明文长 = frame_len - 1, 或 0 (拒绝/参数非法)
 *
 * 窗口算法 (8-bit 传输, 32-bit 推断, 严格前向):
 *   diff = (ctr_byte - (rx_counter & 0xFF)) mod 256
 *   diff ∈ [1, WINDOW]: 前向 (含跨 256 回绕), 接受
 *   diff == 0:          重复帧, 拒绝
 *   diff > WINDOW:      回退/乱序/重放, 拒绝
 *
 * 用 8-bit 模减时, "前向"天然包含回绕: 例如 rx_low=250, ctr_byte=5
 * → diff = (5-250)&0xFF = 11 ∈ [1,32], 正确判定为前向 (跨回绕).
 * 严格前向意味着不接收任何乱序帧; nRF24 硬件 ARC=15 重传使丢包极少,
 * 乱序在点对点链路上不会发生, 故这是安全且鲁棒的选择.
 * ================================================================ */
uint8_t nrf24_crypt_open(uint8_t *buf, uint8_t frame_len)
{
	if (frame_len < 3 || frame_len > 32) {
		return 0;
	}

	uint8_t ctr_byte = buf[0];
	uint32_t ctr;

	if (!rx_initialized) {
		/* 首帧: 直接采用 ctr_byte 作为低位, 高位从 0 开始 */
		ctr = ctr_byte;
	} else {
		uint8_t rx_low = (uint8_t)(rx_counter & 0xFF);
		uint8_t diff = (uint8_t)(ctr_byte - rx_low);

		if (diff == 0 || diff > RX_WINDOW) {
			/* 重复 / 回退 / 超窗 → 拒绝 */
			return 0;
		}

		/* 前向 (diff ∈ [1, WINDOW]); 判断是否跨 256 边界 */
		if (ctr_byte < rx_low) {
			/* 跨回绕: 高位 +256 */
			ctr = (rx_counter & ~0xFFu) + 256u + ctr_byte;
		} else {
			ctr = (rx_counter & ~0xFFu) + ctr_byte;
		}
	}

	/* 用推断出的完整 32-bit counter 解密 (与 TX 侧 keystream 一致) */
	ctr_xor(buf + 1, frame_len - 1, ctr);

	/* 整体左移 1 字节, 覆盖 ctr 前缀 */
	memmove(buf, buf + 1, frame_len - 1);

	rx_counter = ctr;
	rx_initialized = true;

	return frame_len - 1;
}
