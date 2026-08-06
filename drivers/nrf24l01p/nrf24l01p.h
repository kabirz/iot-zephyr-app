/** @file nrf24l01p.h
 *  @brief Nordic nRF24L01+ Zephyr 驱动公共 API。
 *
 *  中断驱动接收：IRQ 引脚触发后由驱动内部中断线程排空 RX FIFO，
 *  通过用户注册的 msgq 或回调投递帧（参考 Zephyr CAN 驱动的
 *  can_add_rx_filter_msgq 模式）。
 *  TX 在 send 内等待 ACK（TX_DS）/重传耗尽（MAX_RT），可返回耗时与重传次数。
 */
#ifndef NRF24L01P_H
#define NRF24L01P_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nrf24_mode {
	NRF24_MODE_PTX,
	NRF24_MODE_PRX,
	NRF24_MODE_STANDBY,
	NRF24_MODE_POWER_DOWN,
};

enum nrf24_data_rate {
	NRF24_RATE_250K,
	NRF24_RATE_1M,
	NRF24_RATE_2M,
};

enum nrf24_tx_power {
	NRF24_PWR_18DBM,
	NRF24_PWR_12DBM,
	NRF24_PWR_6DBM,
	NRF24_PWR_0DBM,
};

enum nrf24_payload_mode {
	NRF24_PAYLOAD_DYNAMIC,
	NRF24_PAYLOAD_FIXED,
};

enum nrf24_crc_mode {
	NRF24_CRC_1_BYTE,
	NRF24_CRC_2_BYTE,
};

/** 单帧最大载荷字节数。*/
#define NRF24_MAX_PAYLOAD 32

/** 运行时配置：仅覆盖高频可变参数的子集。
 *  payload_mode / rx_payload_width / ard / arc 等仅在 init 时从 DT 应用。
 *  传 NULL 表示按当前 DT 参数重新应用。
 */
struct nrf24_cfg {
	uint8_t channel;
	enum nrf24_data_rate data_rate;
	enum nrf24_tx_power tx_power;
	enum nrf24_crc_mode crc_mode;
	uint8_t address_width;          /* 3/4/5，0 表示不改 */
	const uint8_t *tx_addr;         /* 可空，长度须 = address_width */
};

/** TX 发送结果（send 返回后填充，可为 NULL 表示不关心）。
 *  - acked: true=收到 ACK（TX_DS），false=MAX_RT 重传耗尽
 *  - elapsed_ms: 从 CE 触发到 TX 完成 IRQ 的耗时（ms）
 *  - retransmits: 实际自动重传次数（OBSERVE_TX.ARC_CNT）
 */
struct nrf24_tx_result {
	bool acked;
	uint32_t elapsed_ms;
	uint8_t retransmits;
};

/** 中断线程上下文投递给用户的接收帧（len + 载荷）。*/
struct nrf24_frame {
	uint8_t len;                         /* 实际载荷字节数 */
	uint8_t data[NRF24_MAX_PAYLOAD];     /* 载荷 */
};

/** RX 帧回调（在中断线程上下文调用，不可阻塞）。
 *  @param dev  nRF24 设备
 *  @param buf  帧载荷（仅在本回调内有效）
 *  @param len  载荷字节数
 *  @param user_data 注册时传入的透传指针
 */
typedef void (*nrf24_rx_callback_t)(const struct device *dev, const uint8_t *buf,
				    size_t len, void *user_data);

/** 应用/重新应用配置。cfg=NULL 时按 DT 参数重新应用。*/
int nrf24_configure(const struct device *dev, const struct nrf24_cfg *cfg);

/** 阻塞发送一帧并等待 ACK/重传结果。
 *  @param dev     nRF24 设备
 *  @param buf     载荷
 *  @param len     载荷长度 ∈ [1,32]
 *  @param timeout 等待 ACK 超时
 *  @param result  出参（可空）：acked/elapsed_us/retransmits
 *  @return 0=ACK 成功, -EIO=MAX_RT 重传耗尽, -EAGAIN=超时, -EINVAL=长度非法
 *  @note 发送完成后自动切回 PRX 接收模式。
 */
int nrf24_send(const struct device *dev, const void *buf, size_t len, k_timeout_t timeout,
	       struct nrf24_tx_result *result);

/** 阻塞/超时接收一帧（轮询兼容接口）。
 *  返回载荷字节数（>0），负值为错误码。
 *  @note 默认推荐使用 nrf24_add_rx_msgq() 中断驱动接收。
 */
int nrf24_recv(const struct device *dev, void *buf, size_t max_len, k_timeout_t timeout);

/** 切换 RF 模式。*/
int nrf24_set_mode(const struct device *dev, enum nrf24_mode mode);

/** 控制芯片电源 (需 devicetree 配置 power-gpios; 未配置时空操作)。
 *  - enable=true:  上电 + 等待 POR + 重新应用配置并进入 PRX
 *  - enable=false: 先置 POWER_DOWN 软关机 (保护 SPI 引脚), 再断电
 *  驱动 init 时若配置了 power-gpios 会自动上电, 应用通常无需手动调用;
 *  典型用途为系统休眠/唤醒时关闭/恢复射频。
 *  @return 0 成功, 负值错误码
 */
int nrf24_power_enable(const struct device *dev, bool enable);

/** 便捷：进入 PRX 并拉高 CE 开始接收。*/
int nrf24_start_rx(const struct device *dev);

/** 非阻塞查询是否收到数据（读 STATUS.RX_DR）。*/
bool nrf24_rx_ready(const struct device *dev);

/** 注册 RX msgq：中断线程收到帧后以 K_NO_WAIT 投递 nrf24_frame 到该 msgq。
 *  注册后 IRQ 自动驱动接收，无需轮询。与回调互斥（msgq 优先）。
 *  @return 0 成功
 */
int nrf24_add_rx_msgq(const struct device *dev, struct k_msgq *msgq);

/** 注册 RX 回调：中断线程收到帧后在其中断线程上下文调用。
 *  回调内不可阻塞/不可调用 nrf24_send 等 SPI 接口。与 msgq 互斥。
 *  @return 0 成功
 */
int nrf24_add_rx_callback(const struct device *dev, nrf24_rx_callback_t cb, void *user_data);

/* —— 底层寄存器入口（power user）—— */
int nrf24_read_reg(const struct device *dev, uint8_t reg, uint8_t *val);
int nrf24_write_reg(const struct device *dev, uint8_t reg, uint8_t val);
int nrf24_read_reg_multi(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len);
int nrf24_write_reg_multi(const struct device *dev, uint8_t reg, const uint8_t *buf, size_t len);

/** 通过 LOG_INF 打印一份完整寄存器快照 (调试用). */
void nrf24_dump_regs(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* NRF24L01P_H */
