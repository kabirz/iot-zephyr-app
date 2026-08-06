/* nRF24L01+ SPI 命令、寄存器地址与位定义（移植自 Ebyte SDK nRF24L01P_REG.h） */
#ifndef NRF24L01P_REG_H
#define NRF24L01P_REG_H

/* SPI 命令 */
#define R_REGISTER          0x00
#define W_REGISTER          0x20
#define R_RX_PAYLOAD        0x61
#define W_TX_PAYLOAD        0xA0
#define FLUSH_TX            0xE1
#define FLUSH_RX            0xE2
#define REUSE_TX_PL         0xE3
#define R_RX_PL_WID         0x60
#define W_ACK_PAYLOAD       0xA8
#define W_TX_PAYLOAD_NOACK  0xB0
#define NOP                 0xFF

/* 寄存器地址 */
#define L01REG_CONFIG       0x00
#define L01REG_ENAA         0x01
#define L01REG_EN_RXADDR    0x02
#define L01REG_SETUP_AW     0x03
#define L01REG_SETUP_RETR   0x04
#define L01REG_RF_CH        0x05
#define L01REG_RF_SETUP     0x06
#define L01REG_STATUS       0x07
#define L01REG_OBSERVE_TX   0x08
#define L01REG_RPD          0x09
#define L01REG_RX_ADDR_P0   0x0A
#define L01REG_RX_ADDR_P1   0x0B
#define L01REG_RX_ADDR_P2   0x0C
#define L01REG_RX_ADDR_P3   0x0D
#define L01REG_RX_ADDR_P4   0x0E
#define L01REG_RX_ADDR_P5   0x0F
#define L01REG_TX_ADDR      0x10
#define L01REG_RX_PW_P0     0x11
#define L01REG_RX_PW_P1     0x12
#define L01REG_FIFO_STATUS  0x17
#define L01REG_DYNPD        0x1C
#define L01REG_FEATURE      0x1D

/* CONFIG 位 */
#define MASK_RX_DR          6
#define MASK_TX_DS          5
#define MASK_MAX_PT         4
#define EN_CRC              3
#define CRCO                2
#define PWR_UP              1
#define PRIM_RX             0

/* EN_AA / EN_RXADDR 位 */
#define ENAA_P0             0
#define ERX_P0              0

/* SETUP_AW */
#define AW_3BYTES           0x01
#define AW_4BYTES           0x02
#define AW_5BYTES           0x03

/* SETUP_RETR：ARD[7:4]=步进250us (val=us/250-1)，ARC[3:0]=重传次数 */
#define ARD_US(us)          (((((us) / 250) - 1) & 0x0F) << 4)

/* RF_SETUP 位 */
#define RF_DR_LOW           5
#define RF_DR_HIGH          3
#define PWR_N_18DB          (0x00 << 1)
#define PWR_N_12DB          (0x01 << 1)
#define PWR_N_6DB           (0x02 << 1)
#define PWR_N_0DB           (0x03 << 1)

/* STATUS 位 */
#define RX_DR               6
#define TX_DS               5
#define MAX_RT              4

/* DYNPD / FEATURE 位 */
#define DPL_P0              0
#define EN_DPL              2
#define EN_ACK_PAY          1

/* 便捷 */
#define IRQ_ALL             ((1 << RX_DR) | (1 << TX_DS) | (1 << MAX_RT))
#define NRF24_MAX_PAYLOAD   32

#endif /* NRF24L01P_REG_H */
