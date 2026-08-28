# 野火 GD32H759IMK6 BTB 开发板硬件连接说明（核心板 + 底板）

> **信息来源**（交叉验证，可信度从高到低）
> 1. 《GD32H759IMK6引脚分配(BTB开发板_BGA176版).xlsx》——官方引脚分配表（BTB 逐脚映射的权威来源）
> 2. 《[野火]GD32H7 BTB开发板硬件规格书V1.1.pdf》
> 3. 核心板原理图 `EBF410304V1R0_SCH_20240325.pdf`（V0.1）、底板原理图 `EBF410305V1R0_SCH_20240325.pdf`（V0.2）
> 4. 配套例程源码（`2-GPIO_LED`、`CAN`、`ENET_LWIP`、`SDIO_TFCard`、`TLI_LCD`、`TLI_LCD_TOUCH`、`USART_RS232`、`USART_RS485`）的实际初始化代码
>
> 注：核心板原理图图框标题残留"GD32H737IIK6"字样，实际焊接主控以规格书和 MCU 符号为准，为 **GD32H759IMK6**。

---

## 0. 系统组成

```
+==========================+  BTB 60Pin x2   +==========================+
|  核心板 EBF410304V1R0     | <=============> |  底板 EBF410305V1R0       |
|  · GD32H759IMK6 (BGA176) |  J1 公/J2 母     |  · 电源/开关/保护          |
|    600MHz, 3840KB Flash  |    对插组合使用   |  · LED/按键/蜂鸣器         |
|    1024KB SRAM           |                 |  · TF卡/CAN/485/232        |
|  · SDRAM + NOR FLASH     |                 |  · 以太网/LCD/USB/温湿度    |
|  · 25MHz + 32.768kHz     |                 |  · 扩展排针 J17            |
+==========================+                 +==========================+
```

- 芯片共 128 个 IO（不含 USB 与 NRST 相关引脚），核心板独占 49 个（SDRAM/FLASH/晶振），其余经 BTB 座子引到底板
- 核心板尺寸 33×35mm（6 层板），底板尺寸 118.2×69.2mm
- 底板供电：5V@1A Type-C 输入

---

## 1. 核心板 EBF410304V1R0

### 1.1 主控最小系统

| 项目 | 说明 |
|------|------|
| 主控 | GD32H759IMK6：Cortex-M7 @600MHz，3840KB Flash，1024KB SRAM，BGA176 封装 |
| 供电架构 | 片内 **SMPS + LDO 级联**（SMPS & LDO ON），SMPS 外接功率电感 L3（2.2µH）；外部单电源 VDD_3V3 输入（2.7~3.5V，典型 3.3V） |
| 主晶振 | Y1：25MHz（OSC_IN=PH0、OSC_OUT=PH1） |
| RTC 晶振 | Y2：32.768kHz（OSC32_IN=PC14、OSC32_OUT=PC15） |
| 备份电池 | VBAT 引脚经 BAT54C 双二极管自动切换（底板 3.3V / CR1220 电池 3V0_VBAT） |
| 复位 | NRST 引出到底板复位按键与 SWD 座；PDR_ON 经 10K 上拉 |
| BOOT0 | 引出到底板 BOOT 跳线（默认接 GND） |
| BOOT1 | 复用 **PB2**（默认跳 GND；可复用 RTC_OUT、SAI0/2_SD0、EXMC_D10、SPI2_MOSI 等） |

### 1.2 板载存储

| 器件 | 型号 | 容量 | 接口 |
|------|------|------|------|
| SDRAM | U1：IS42S16160J-6BLI | 16 位位宽（规格书标注 16MB） | EXMC（SDRAM 控制器） |
| NOR FLASH | U7：GD25Q64ESIGR | 64Mbit = 8MB | OSPI0（OSPIM_P0_* 信号） |

### 1.3 核心板独占引脚（不引出 BTB，49 个）

**SDRAM（EXMC 接口）：**

| SDRAM 信号 | MCU 引脚 | SDRAM 信号 | MCU 引脚 |
|------------|----------|------------|----------|
| A0~A5 | PF0~PF5 | DM0 / DM1 | PE0 / PE1 |
| A6~A9 | PF12~PF15 | DQ0 / DQ1 | PD14 / PD15 |
| A10~A12 | PG0~PG2 | DQ2 / DQ3 | PD0 / PD1 |
| BA0 / BA1 | PG4 / PG5 | DQ4~DQ7 | PE7~PE10 |
| RAS | PF11 | DQ8 / DQ9 | PE11 / PA5 |
| CAS | PG15 | DQ10~DQ12 | PE13~PE15 |
| WE | PH5 | DQ13~DQ15 | PD8~PD10 |
| CS0 | PC2 | CKE | PC3 |
| CLK | PG8 | | |

**OSPI NOR FLASH（OSPI P 端口 0）：**

| FLASH 信号 | MCU 引脚 |
|------------|----------|
| OSPIM_P0_CSN（片选） | PB10 |
| OSPIM_P0_IO0 | PF8 |
| OSPIM_P0_IO1 | PF9 |
| OSPIM_P0_IO2 | PE2 |
| OSPIM_P0_IO3 | PA6 |
| OSPIM_P0_SCK | PA3 |

**晶振：** PH0/PH1（25MHz）、PC14/PC15（32.768kHz）。

> 因此 UART7（复用于 PE0/PE1 等）、USART2 的 PD8/PD9、SPI3、CAN0 的 PD0/PD1 等，均因 SDRAM 占用而不可用。

---

## 2. 板间连接：BTB J1/J2 全引脚映射（60+60Pin）

核心板 J2（SGDBM 公座）/ J1（SGDBF 母座）与底板 J1（SGDBM）/J2（SGDBF）对插。下表为官方引脚分配表整理的逐脚映射（个别表格笔误已按 AF 复用表与例程代码校正）。

### J1（信号部分）

| 引脚 | 网络 | 芯片引脚 | 底板功能 |
|------|------|----------|----------|
| J1-08 | 3V0_VBAT | — | RTC 电池(+) |
| J1-09 | BOOT1 | PB2 | BOOT 启动选择 |
| J1-11 | BOOT0 | BOOT0 | BOOT 启动选择 |
| J1-12 | PC3_C | PC3_C | ADC2_IN1 → 扩展排针 |
| J1-13 | NRST | NRST | 复位按键 + SWD 座 |
| J1-15 | PA0 | PA0 | KEY1 / WKUP0 |
| J1-16 | PC2_C | PC2_C | ADC2_IN0 → 扩展排针 |
| J1-19 | PF7 | PF7 | LED2 |
| J1-20 | PA1_C | PA1_C | ADC01_IN1 → 扩展排针 |
| J1-21 | PB13 | PB13 | USBHS1_ID（OTG 识别） |
| J1-24 | PA0_C | PA0_C | 板载电位器 RA1（ADC01_IN0） |
| J1-25 | PH2 | PH2 | SDIO0_CD（TF 卡检测） |
| J1-27 | PH3 | PH3 | DHT11 / DS18B20 温湿度 |
| J1-28 | PF6 | PF6 | ETH0_RESET（PHY 复位） |
| J1-31 | PB12 | PB12 | UART4_RX（RS485） |
| J1-32 | PA1 | PA1 | 扩展（ETH0_RMII_REF_CLK 备选） |
| J1-33 | PB6 | PB6 | UART4_TX（RS485）/ EBF-Module |
| J1-34 | PA2 | PA2 | ETH0_MDIO |
| J1-36 | PA7 | PA7 | ETH0_RMII_CRS_DV |
| J1-37 | PB0 | PB0 | 蜂鸣器 |
| J1-38 | PB11 | PB11 | ETH0_RMII_TX_EN |
| J1-39 | PB1 | PB1 | TP_RST（触摸复位） |
| J1-40 | PC1 | PC1 | ETH0_MDC |
| J1-41 | PC13 | PC13 | TP_INT（触摸中断） |
| J1-42 | PC4 | PC4 | ETH0_RMII_RXD0 |
| J1-43 | PH7 | PH7 | TP_I2C_CLK |
| J1-44 | PC5 | PC5 | ETH0_RMII_RXD1 |
| J1-45 | PH8 | PH8 | TP_I2C_SDA |
| J1-46 | PG13 | PG13 | ETH0_RMII_TXD0 |
| J1-47 | PD11 | PD11 | LED1 |
| J1-48 | PG14 | PG14 | ETH0_RMII_TXD1 |
| J1-49 | PD12 | PD12 | CAN2_RX |
| J1-51 | PD13 | PD13 | CAN2_TX |
| J1-52 | PH6 | PH6 | 悬空扩展 |
| J1-54 | PH12 | PH12 | EBF-Module |
| J1-55 | PH13 | PH13 | UART3_TX（RS232 一路）/ EBF-Module IO1 |
| J1-56 | PE12 | PE12 | EBF-Module |
| J1-57 | PH14 | PH14 | UART3_RX（RS232 一路）/ EBF-Module IO2 |
| 其余 | GND / VDD_3V3 | — | 电源与地（J1-01~07、10、14、17、18、22、23、26、29、30、35、50、53、58~60） |

### J2（信号部分）

| 引脚 | 网络 | 芯片引脚 | 底板功能 |
|------|------|----------|----------|
| J2-03 | PB9 | PB9 | TLI_B7 |
| J2-04 | PE6 | PE6 | SAI0_SD0 → 扩展排针 |
| J2-05 | PB8 | PB8 | TLI_B6 |
| J2-06 | PE5 | PE5 | SAI0_SCK0 → 扩展排针 |
| J2-07 | PB5 | PB5 | TLI_B5 |
| J2-08 | PE4 | PE4 | SAI0_FS0 → 扩展排针 |
| J2-09 | PG12 | PG12 | TLI_B4 |
| J2-10 | PE3 | PE3 | SAI0_SD1 → 扩展排针 |
| J2-11 | PG11 | PG11 | TLI_B3 |
| J2-12 | PG7 | PG7 | SAI0_MCLK0 → 扩展排针 |
| J2-13 | PD3 | PD3 | TLI_G7 |
| J2-14 | PG9 | PG9 | SAI0_I2C_CLK → 扩展排针 |
| J2-15 | PC7 | PC7 | TLI_G6 |
| J2-16 | PH9 | PH9 | SAI0_I2C_SDA → 扩展排针 |
| J2-17 | PH4 | PH4 | TLI_G5 |
| J2-19 | PH15 | PH15 | TLI_G4 |
| J2-20 | PB4 | PB4 | 485_DE（收发方向） |
| J2-21 | PG10 | PG10 | TLI_G3 |
| J2-22 | PB7 | PB7 | EBF-Module |
| J2-23 | PC0 | PC0 | TLI_G2 |
| J2-25 | PG6 | PG6 | TLI_R7 |
| J2-26 | PD4 | PD4 | LED3 |
| J2-27 | PA8 | PA8 | CK_OUT0（以太网 50MHz）/ TLI_R6 |
| J2-28 | PD5 | PD5 | USART1_TX（RS232 第二路） |
| J2-29 | PH11 | PH11 | TLI_R5 |
| J2-30 | PD6 | PD6 | USART1_RX（RS232 第二路） |
| J2-31 | PH10 | PH10 | TLI_R4 |
| J2-32 | PD7 | PD7 | KEY2 |
| J2-33 | PA15 | PA15 | TLI_R3（兼 SPI2_NSS 排针选项） |
| J2-36 | PD2 | PD2 | SDIO0_CMD |
| J2-37 | PB3 | PB3 | TLI_PIXCLK |
| J2-38 | PC12 | PC12 | SDIO0_CLK（兼 SPI2_MOSI） |
| J2-40 | PC11 | PC11 | SDIO0_D3（兼 SPI2_MISO） |
| J2-41 | PC6 | PC6 | TLI_HSYNC |
| J2-42 | PC10 | PC10 | SDIO0_D2（兼 SPI2_SCK） |
| J2-43 | PA4 | PA4 | TLI_VSYNC |
| J2-44 | PC9 | PC9 | SDIO0_D1 |
| J2-45 | PF10 | PF10 | TLI_DE |
| J2-46 | PC8 | PC8 | SDIO0_D0 |
| J2-47 | PG3 | PG3 | TLI_BL（背光） |
| J2-50 | USB0_DP | USBHS0_DP | 底板 TYPE-C（供电口/Device） |
| J2-51 | PA14 | PA14 | SWCLK |
| J2-52 | USB0_DM | USBHS0_DM | 同上 |
| J2-53 | PA13 | PA13 | SWDIO |
| J2-55 | PA10 | PA10 | USART0_RX（调试串口） |
| J2-56 | USB1_DP | USBHS1_DP | 底板 OTG TYPE-C |
| J2-57 | PA9 | PA9 | USART0_TX（调试串口） |
| J2-58 | USB1_DM | USBHS1_DM | 同上 |
| 其余 | GND | — | 地（J2-01、02、18、24、34、35、39、48、49、54、59、60） |

---

## 3. 底板 EBF410305V1R0

### 3.1 电源系统（原理图第 3 页）

| 通路 | 器件 | 说明 |
|------|------|------|
| 5V 输入 | J3（TYPE-C 母座） | 推荐 5V@1A；红色电源指示灯 LED4 常亮表示供电正常 |
| 总开关 | SW1 | 拨动开关控制整机电源 |
| 5V→3.3V | U2：RT8097 同步降压 | 输出 VDD_3V3 供全板与核心板 |
| 输入保护 | U1：SR05-N、L1（2.2µH） | ESD 保护与滤波；TYPE-C CC1/CC2 各 5.1K 下拉（默认取电） |
| RTC 电池 | CN1（CR1220 座） | 3V0_VBAT 经 BAT54C 与 3.3V 切换后接核心板 VBAT |

### 3.2 调试与启动

**SWD + 调试串口（J13：XH2.54-7P 直针座）**，丝印：`3V3、DIO、CLK、NRST、GND、TXD、RXD`

| 丝印 | 信号 | 芯片引脚 |
|------|------|----------|
| DIO | SWDIO | PA13 |
| CLK | SWCLK | PA14 |
| NRST | NRST | 复位 |
| TXD | USART0_TX | PA9 |
| RXD | USART0_RX | PA10 |

**BOOT 跳线（J4，HDR 2×3）**：BOOT0、BOOT1 默认均接 GND → **从内部 Flash 启动**；BOOT0=1、BOOT1=0 → ISP（系统 Bootloader）；BOOT0=0、BOOT1=1 → RAM 启动。下载失败时的排查手段：BOOT0 跳 3V3 重新上电（规格书 4.4.2）。

### 3.3 板载用户外设

| 外设 | 芯片引脚 | 说明 |
|------|----------|------|
| LED1 / LED2 / LED3（红色） | PD11 / PF7 / PD4 | 低电平点亮，120Ω 限流 |
| LED4（红色） | — | 电源指示灯，非程序控制 |
| KEY1（SW2，丝印 KEY1/WKUP0） | PA0 | 上拉输入，按下为低；兼 RTC 唤醒 |
| KEY2（SW3/SW4，丝印 KEY2） | PD7 | 上拉输入，按下为低 |
| 复位按键 SW2（NRST） | NRST | 系统复位 |
| 蜂鸣器 PZ1（有源） | PB0 | SS8050 三极管驱动 |
| 板载电位器 RA1（10K 蓝色旋钮） | PA0_C（ADC01_IN0） | ADC 实验输入 |
| DHT11/DS18B20 接口 J11 | PH3 | R6 20K 数据上拉 |
| TF 卡座 J10（自弹式，≤32GB，SD V3.0） | SDIO0：CMD=PD2、CLK=PC12、D0~D3=PC8~PC11、CD=PH2 | |

### 3.4 EBF-Module 扩展接口（6Pin 座 + 6Pin 排针）

- 相关信号：**IO1=PH13、IO2=PH14**（经 J12 跳帽选择功能），另有 PB7、PE12、PH12、PB6
- 直插野火蓝牙/WIFI 串口模块：跳帽接 IO1-PH13、IO2-PH14 作串口
- 直插 OLED / MPU6050 / 心率等 I2C 模块：跳帽接 IO1-PH13、IO2-PH14 作**软件 I2C**（或杜邦线交叉接作硬件 I2C）；U11 OLED 座带 4.7K 上拉
- 注意 PH13/PH14 同时是 RS232 的 UART3，功能互斥

### 3.5 通信接口

**CAN（原理图第 6 页）**

| 项目 | 内容 |
|------|------|
| 控制器/收发器 | CAN2 外设 + U4：SIT1042AQT |
| CAN2_TX / CAN2_RX | PD13（AF5）/ PD12（AF5） |
| 终端电阻 | R37 120Ω |
| 物理接口 | J8：JL301-5000（5mm 接线柱座） |

**RS485（UART4）**

| 项目 | 内容 |
|------|------|
| 收发器 | U6：SIT3088ESA（半双工） |
| UART4_TX / RX | PB6（AF14）/ PB12（AF14） |
| DE/RE 方向 | PB4（高电平发送） |
| 物理接口 | J7：JL301-5000 |

**RS232（双路，SIT3232EESE）**

| 路 | 串口外设 | 芯片引脚 | 物理接口 |
|----|----------|----------|----------|
| 232-1 | UART3 | TX=PH13、RX=PH14（AF8） | J9：JL301-5000 |
| 232-2 | USART1 | TX=PD5、RX=PD6 | 扩展排针 |

**扩展 IO 排针 J17（HDR 2×13，26Pin）**

- 信号：SPI2（NSS=PA15、SCK=PC10、MISO=PC11、MOSI=PC12）、CAN2_RX/TX、CK_OUT0、USART1_TX/RX（PD5/PD6）、UART3_TX/RX、SAI0 全套（MCLK0=PG7、SCK0=PE5、FS0=PE4、SD0=PE6、SD1=PE3、I2C_CLK=PG9、I2C_SDA=PH9）
- 模拟信号：PA1_C（ADC01_IN1）、PC2_C（ADC2_IN0）、PC3_C（ADC2_IN1）
- 其余 GPIO：PA15、PD5、PD6、PE3~PE6、PG7、PG9、PH9、PH12、PH13、PH14、PE12 等，及 VDD_3V3 / VCC_5V0 / GND

### 3.6 以太网（原理图第 7 页）

| 项目 | 内容 |
|------|------|
| PHY | U3：LAN8720A-CP-TR（RMII，10/100Mbps） |
| 时钟 | MCU PA8（CKOUT0，PLL0P/12=50MHz）→ PHY CLKIN；PHY REFCLKO → PA1；PHY 板载 25MHz 晶振 Y1 |
| 网口 | J6：R-RJ45（R08P-C0001，内置变压器，双色指示灯） |

| RMII 信号 | 芯片引脚（均 AF11） |
|-----------|---------------------|
| ETH0_RMII_REF_CLK | PA1 |
| ETH0_MDIO | PA2 |
| ETH0_RMII_CRS_DV | PA7 |
| ETH0_MDC | PC1 |
| ETH0_RMII_RXD0 / RXD1 | PC4 / PC5 |
| ETH0_RMII_TX_EN | PB11 |
| ETH0_RMII_TXD0 / TXD1 | PG13 / PG14 |
| 50MHz 时钟输出 | PA8（CKOUT0） |
| ETH0_RESET（PHY 复位） | PF6（普通 IO，10K 上拉） |

### 3.7 LCD 与 USB

**RGB LCD（J5：FPC-40P，0.5mm，适配野火 4.3/5/7 寸电容屏，RGB565）**

| 信号 | 引脚（复用） | 信号 | 引脚（复用） |
|------|--------------|------|--------------|
| TLI_R3 | PA15（AF9） | TLI_B3 | PG11（AF14） |
| TLI_R4 | PH10（AF14） | TLI_B4 | PG12（AF9） |
| TLI_R5 | PH11（AF14） | TLI_B5 | PB5（AF3） |
| TLI_R6 | PA8（AF14） | TLI_B6 | PB8（AF14） |
| TLI_R7 | PG6（AF14） | TLI_B7 | PB9（AF14） |
| TLI_G2 | PC0（AF11） | TLI_PIXCLK | PB3（AF2） |
| TLI_G3 | PG10（AF9） | TLI_HSYNC | PC6（AF14） |
| TLI_G4 | PH15（AF14） | TLI_VSYNC | PA4（AF14） |
| TLI_G5 | PH4（AF9） | TLI_DE | PF10（AF14） |
| TLI_G6 | PC7（AF14） | TLI_BL | PG3（AF14） |
| TLI_G7 | PD3（AF14） | | |

**电容触摸（随屏 FPC 引入，软件 I2C，支持 GT1151QM/GT917S 系列）**：SCL=PH7、SDA=PH8、RST=PB1、INT=PC13。

**USB（两个 TYPE-C）**

| 接口 | 模式 | 说明 |
|------|------|------|
| J3（与供电共用） | USBHS0 Device | USB0_DP/DM 专用脚；VBUS 经 SR05-N 保护 |
| J16 | USBHS1 OTG | USB1_DP/DM 专用脚；LPW5209AB5F 供电开关 + 2SK3018 OTG 控制；USBHS1_ID=PB13 |

---

## 4. 引脚速查总表

| 功能 | 信号 | 引脚 | 备注 |
|------|------|------|------|
| LED1/2/3 | — | PD11 / PF7 / PD4 | 低电平点亮 |
| KEY1 / KEY2 | — | PA0 / PD7 | 上拉，按下为低 |
| 蜂鸣器 | — | PB0 | 三极管驱动 |
| 电位器 | ADC01_IN0 | PA0_C | 板载 10K |
| 温湿度 | 1-Wire | PH3 | 20K 上拉 |
| TF 卡 | SDIO0 全套 + CD | PD2/PC12/PC8~PC11/PH2 | |
| 调试串口 | USART0 TX/RX | PA9 / PA10 | AF7 |
| SWD | SWDIO/SWCLK | PA13 / PA14 | |
| CAN | CAN2 TX/RX | PD13 / PD12 | AF5 |
| RS485 | UART4 TX/RX + DE | PB6 / PB12 / PB4 | AF14 |
| RS232-1 | UART3 TX/RX | PH13 / PH14 | AF8 |
| RS232-2 | USART1 TX/RX | PD5 / PD6 | 排针 |
| 以太网 | RMII 全套 | PA1/PA2/PA7/PC1/PC4/PC5/PB11/PG13/PG14 | AF11 |
| 以太网时钟/复位 | CKOUT0 / RESET | PA8 / PF6 | |
| LCD RGB565 | 全套 21 信号 | 见 §3.7 | TLI |
| 触摸 | SCL/SDA/RST/INT | PH7/PH8/PB1/PC13 | 软件 I2C |
| USB | USBHS0 / USBHS1 | 专用 DP/DM；ID=PB13 | Device / OTG |
| 扩展排针 | SAI0、SPI2、模拟脚等 | 见 §3.5 | |
| 板外存储（核心板） | EXMC SDRAM + OSPI FLASH | 见 §1.2/1.3 | 独占 49 IO |

---

## 5. 复用冲突与使用注意

1. **PA9/PA10**：调试串口（USART0）专用引出，不要再复用（如 I2C2）。
2. **PA8**：以太网 50MHz 时钟（CKOUT0）与 LCD R6 冲突；**PB11/PG13/PG14/PG11/PG12** 等同时出现在以太网与 LCD 网络 → **LCD 与以太网互斥**。
3. **PH13/PH14**：RS232-1（UART3）与 EBF-Module（IO1/IO2）共用，二选一。
4. **PC10/PC11/PC12/PA15**：TF 卡（SDIO0）为主，兼作 LCD 数据线与扩展排针 SPI2 选项 → TF 卡、LCD、SPI2 不可同时使用。
5. 核心板独占 SDRAM/FLASH 后，**UART7、USART2(PD8/PD9)、SPI3、CAN0(PD0/PD1)、SAI1** 等已不可用（引脚被占用）。
6. TF 卡座最大支持 32GB（SD V3.0）。

---

*整理自官方引脚分配表、硬件规格书、核心板/底板原理图，并经配套例程源码逐项验证。完整 AF 复用功能请查阅《GD32H759IMK6引脚分配(BTB开发板_BGA176版).xlsx》与 GD32H759 数据手册。*
