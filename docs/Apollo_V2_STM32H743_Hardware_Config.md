# 阿波罗 V2 STM32H743 开发板硬件配置

> 依据资料：核心板原理图《STM32H743_CORE V2.0.pdf》、底板原理图《DNF429P_F767P_H743P V2.5.pdf》、《阿波罗V2 STM32H743开发板IO引脚分配表.xlsx》。
> 板卡采用"核心板 + 底板"结构，核心板通过 2 个 2×30P 3710 端子与底板连接，可兼容正点原子 STM32F429/F767 等核心板。

---

## 一、核心板（STM32H743_CORE V2.0）

### 1. 主控
- **MCU：STM32H743IIT6**（LQFP176 封装）
  - ARM Cortex-M7 内核，主频最高 480MHz
  - 片内 2MB Flash、约 1MB RAM

### 2. 时钟
- 主晶振：**25MHz**（PH0/PH1）
- RTC 晶振：**32.768kHz**（PC14/PC15）

### 3. 板载存储
| 器件 | 型号 | 容量 | 接口 |
|---|---|---|---|
| SDRAM | W9825G6KH-6 | 32MB（16 位总线） | FMC（SDRAM 控制器） |
| NAND FLASH | MT29F4G08 | 512MB（8 位总线） | FMC（NAND 控制器） |
| SPI FLASH | W25Q256 | 32MB | QSPI（四线） |
| EEPROM | 24C02 | 256 字节 | I2C（PH4/PH5） |

### 4. 板载外设与接口
- **LED**：DS0（红色，PB1）、PWR 电源灯（蓝色）
- **按键**：WK_UP（PA0，可做待机唤醒）
- **USB**：Type-C 接口（USB OTG FS，PA11/PA12），可给核心板供电、做 USB Slave 连电脑，或经 OTG 线做 USB Host
- **RGB LCD 排针 J3**：RGB888 接口（R3~R7、G2~G7、B3~B7 + CLK/DE/VSYNC/HSYNC + 背光），带电阻触摸屏接口（T_CS/T_CLK/T_DIN/T_DO/T_PEN）
- **SWD 调试排针**（SWDIO/SWDCLK/RESET）
- **USART1 排针**（PA9/PA10）
- **电源**：板载 CAT6219-3.3 LDO（5V→3.3V）；3.3V/5V/GND 电源测试点（可由 5V 测试点直接供电）
- **VREF+ 与 LED 联动设计**：核心板 DS0/PWR 由 VREF+ 电平经 SI2301 控制——VREF+ 为高（底板 P4 跳线短接）时两灯均不亮，拔掉跳线帽后 LED 恢复正常，以减小 VREF 受 LED 回路干扰

---

## 二、底板（DNF429P/F767P/H743P V2.5）

### 1. 电源系统
- 供电方式：DC005 电源座（DC_IN，经 SS34 防反接）/ USB_232 Type-C / USB SLAVE / VUSB
- **RY9125** 同步降压 DC-DC：DC 输入 → 5V（VCC5）
- **AMS1117-3.3** ×2：一路产生数字 3.3V（VCC3.3），一路单独产生模拟 3.3V（VCC3.3A，供 ES8388 等模拟电路）
- K1 电源开关，F1 1000mA 自恢复保险丝，TVS 保护（SMFJ5.0CA/SMFJ3.3CA）
- VOUT1/VOUT2 排针引出 3.3V/5V 供外部使用

### 2. 通信接口
| 接口 | 器件 | 说明 |
|---|---|---|
| 以太网 | **YT8512C** PHY（RMII）+ RJ45（带隔离变压器、LINK/SPEED 指示灯）+ 25MHz 晶振 | 10/100M 自适应，50MHz 参考时钟输出至 PA1 |
| USB 转串口 | **CH340C** + USB Type-C（USB_232） | 连接 USART1（PA9/PA10，P11 跳线），支持 RTS#/DTR# 一键下载（Q2 S8050/Q3 S8550 控制 BOOT0/RESET） |
| RS232 | **TP3232** | 双路：COM2（母头）、COM3（公头） |
| RS485 | **TPT8485** | 与 USART2 复用（P7 切换），方向控制 RS485_RE，带 120Ω 端接/偏置电阻，接线端子输出 |
| CAN | **SIT1042T/TPT1051V** | 与 USB D+/D- 复用（P9 切换），120Ω 端接电阻，接线端子输出 |
| USB HOST | USB-A 母座 | 与 USB OTG（PA11/PA12）共用数据线，VBUS 由 PCF8574 的 USB_PWR 控制 |
| USB SLAVE | USB Type-C | USB OTG FS 从机接口 |
| 无线模块 | WIRELESS 8P 座（NRF24L01 兼容） | SPI2（PB13/PB14/PB15）+ NRF_CS=PG10、NRF_CE=PG12、NRF_IRQ=PI11 |
| ATK MODULE | 6P 模块接口 | 供正点原子蓝牙/GPS 等模块：GBC_TX/GBC_RX（经 P8 接 USART3）、GBC_KEY=PI11、GBC_LED=PA4 |

### 3. 存储与显示
- **TF 卡座**：SDIO 4 位模式（CMD=PD2，CK=PC12，D0~D3=PC8~PC11，47K 上拉）
- **TFTLCD 接口（2×17P）**：FMC 8080 并口 16 位——CS=PD7（FMC_NE1）、RS=PD13（FMC_A18）、WR=PD5、RD=PD4、数据线 D0~D15；背光 LCD_BL=PB5；电阻触摸（T_CS=PI8、T_CLK=PH6、T_DIN=PI3、T_DO=PG3、T_PEN=PH7）
- **RGB LCD**：由核心板 J3 排针直插（RGB888 + 触摸）

### 4. 摄像头
- **OLED&CAMERA 接口**：支持 OV2640 等 DCMI 摄像头——D0~D7=PC6/PC7/PC8/PC9/PC11/PD3/PB8/PB9，PCLK=PA6，XCLK=PA8，VSYNC=PB7，HREF=PH8，SCL=PB4，SDA=PB3，RESET=PA15，PWDN=PCF8574 P2

### 5. 音频系统
- **ES8388** 高保真音频编解码器（24bit）：
  - I2S/SAI1：MCLK=PE2、SCLK=PE5、LRCK=PE4、DAC 数据 DSDIN=PE6、ADC 数据 ASDOUT=PE3；控制接口 I2C（PH4/PH5）
- 输出：PHONE 3.5mm 耳机口（LOUT1/ROUT1）+ **MD8002A 功放**驱动板载喇叭（开发板底部，SPK+/SPK-）
- 输入：LINE_IN 线路输入（耳机座复用）、板载驻极体麦克风 MIC
- PWM_DAC 音频输出：PA3（P1 排针）

### 6. 传感器
| 传感器 | 型号 | 接口 |
|---|---|---|
| 6 轴 IMU（加速度+陀螺仪） | SH3001 | I2C（PH4/PH5），中断 6D_INT 接 PCF8574 P5 |
| 3 轴磁力计 | ST480MC | I2C（PH4/PH5） |
| 光环境/距离传感器 | AP3216C | I2C（PH4/PH5），中断 AP_INT 接 PCF8574 P1 |
| 温湿度/温度 | DHT11、DS18B20 接口 | 单总线 DQ=PB12（1WIRE_DQ，4.7K 上拉） |
| 红外遥控接收 | LF0038（HS0038 类） | REMOTE_IN=PA8 |
| 电容触摸按键 | TPAD | PA5（与 STM_ADC 复用，P10 跳线） |

### 7. 人机交互
- **LED**：LED0/DS0（红，PB1）、LED1/DS1（绿，PB0）、PWR（蓝，电源指示）
- **按键**：KEY_UP=PA0、KEY0=PH3、KEY1=PH2、KEY2=PC13、RESET 复位键
- **蜂鸣器**：有源蜂鸣器，由 8550 三极管驱动，控制脚为 PCF8574 的 P0（BEEP）

### 8. IO 扩展器（PCF8574T，I2C 地址 0x20）
挂接在 I2C 总线（PH4=SCL/PH5=SDA），中断输出 IIC_INT→PB12。8 路扩展 IO 分配：
| 扩展脚 | 功能 |
|---|---|
| P0 | BEEP 蜂鸣器控制 |
| P1 | AP_INT（AP3216C 中断） |
| P2 | DCMI_PWDN（摄像头使能） |
| P3 | USB_PWR（USB HOST 供电控制） |
| P4 | EXIO（预留扩展） |
| P5 | 6D_INT（SH3001 中断） |
| P6 | RS485_RE（RS485 收发方向） |
| P7 | ETH_RESET（以太网 PHY 复位） |

### 9. 调试与系统
- **JTAG 排针（2×10P）**：支持 JTAG/SWD（建议 SWD：PA13=SWDIO、PA14=SWCLK）
- **BOOT 跳线（3×2）**：BOOT0/BOOT1 启动方式选择（10K 电阻）
- **VREF 跳线 P4**：VREF+ 接 3.3V 选择，同时控制核心板 LED 供电
- **IO 扩展排针**：P2/P3（两个 2×22P）引出全部 IO；P5（1×16P，VREF_SEL 等）
- **VBAT**：经 1N4148 由 3.3V 供电（并接备份电容）

---

## 三、关键 IO 引脚分配汇总（按功能域）

### 时钟与系统
| GPIO | 功能 |
|---|---|
| PH0/PH1 | 25MHz 系统晶振（不可做 IO） |
| PC14/PC15 | 32.768kHz RTC 晶振（不可做 IO） |
| PA13/PA14 | SWDIO/SWCLK（SWD 调试） |
| PA15/PB3/PB4 | JTAG：JTDI/JTDO/JTRST（复用 DCMI_RESET/DCMI_SDA/DCMI_SCL） |

### 存储总线
| GPIO | 功能 |
|---|---|
| PB2、PB6、PF6~PF9 | QSPI：W25Q256（PB2=CLK，PB6=CS，PF8=IO0，PF9=IO1，PF7=IO2，PF6=IO3） |
| PC0/PC2/PC3、PF11、PF0~PF5、PF12~PF15、PG0~PG2、PG4/PG5、PG8、PG15、PE0/PE1、PD0/PD1、PD14/PD15、PE7~PE15 | FMC SDRAM（W9825G6KH），未引出 |
| PG9、PD6、PD11、PD12 | FMC NAND（MT29F4G08）：片选 CE3/NWAIT/CLE/ALE |
| PD4/PD5/PD7/PD13 + FMC_D0~D15 | FMC TFTLCD：RD/WR/CS/RS + 16 位数据 |

### 通信外设
| GPIO | 功能 |
|---|---|
| PA1/PA7/PC4/PC5/PG13/PG14/PB11/PC1/PA2 | 以太网 RMII：REF_CLK/CRS_DV/RXD0/RXD1/TXD0/TXD1/TX_EN/MDC/MDIO |
| PA9/PA10 | USART1（CH340C USB 串口） |
| PA2/PA3 | USART2（RS232 COM2 / RS485，P7 切换；PA2 兼 ETH_MDIO） |
| PB10/PB11 | USART3（RS232 COM3 / ATK-MODULE，P8 切换；PB11 兼 RMII_TX_EN） |
| PA11/PA12 | USB OTG FS（D-/D+），P9 可切换为 CAN_RX/CAN_TX |
| PB13/PB14/PB15 | SPI2（WIRELESS 模块座 SCK/MISO/MOSI） |
| PG10/PG12/PI11 | 无线模块 CS/CE/IRQ（PG12 兼 SPDIF_RX 光纤，仅 F7 支持） |
| PC8~PC12、PD2 | SDIO TF 卡（D0~D3/CK/CMD） |
| PE2~PE6 | SAI1/I2S 音频（ES8388：MCLK/SDB/FSA/SCKA/SDA） |

### 显示、摄像头与采集
| GPIO | 功能 |
|---|---|
| PB5 | LCD 背光 BL |
| PH9~PH12、PG6 / PH13~PH15、PI0~PI2 / PG11、PI4~PI7 | RGB LCD R3~R7 / G2~G7 / B3~B7 |
| PG7/PF10/PI9/PI10 | RGB LCD CLK/DE/VSYNC/HSYNC |
| PH6/PG3/PI3/PI8/PH7 | 触摸屏：T_CLK/T_DO/T_DIN/T_CS/T_PEN |
| PA6/PA8/PB7/PH8 + PC6~PC9/PC11/PD3/PB8/PB9 | DCMI 摄像头：PCLK/XCLK/VSYNC/HREF + D0~D7 |
| PA5 | ADC 输入 / TPAD 触摸按键 |
| PA4 | DAC 输出 / ATK 模块 LED |

### 按键、LED 与其他
| GPIO | 功能 |
|---|---|
| PA0 | KEY_UP/WK_UP（待机唤醒） |
| PH3/PH2/PC13 | KEY0/KEY1/KEY2 |
| PB1/PB0 | LED0（红）/LED1（绿） |
| PA8 | 红外遥控接收（兼 DCMI_XCLK） |
| PB12 | 1WIRE_DQ（DHT11/DS18B20）+ PCF8574 中断 IIC_INT |
| PH4/PH5 | I2C 总线（24C02/PCF8574/SH3001/ST480MC/AP3216C/ES8388） |
| PA3 | PWM_DAC 输出 |

---

## 四、使用注意事项（摘自官方引脚表）
1. **引脚编号**对应 STM32H743IIT6 的 LQFP176 封装管脚号，完整 176 脚逐脚分配见《阿波罗V2 STM32H743开发板IO引脚分配表.xlsx》。
2. 共有 **30 个 IO 未引出**（SDRAM、NAND、部分 RGB/RTC/晶振引脚等），不能做普通 IO 使用。
3. "完全独立"列标注为 Y 的 IO，在断开相应跳线（P7/P8/P9/P10/P11 等）或不插外设的情况下可完全悬空当普通 IO 用；标注 N 的 IO 存在板上直连器件（如 YT8512C、SDRAM、W25Q256），使用前需确认复用关系（例如 PA1 常年有 50MHz 时钟、PA7 在 PHY 复位期间才可挪用）。
4. PH4/PH5（I2C）、PB6（QSPI 片选）、SWD 等总线脚带 4.7K/10K 上拉电阻，不建议做普通 IO。
5. 跳线速查：**P4**=VREF+/核心板 LED 使能；**P7**=USART2 在 RS232/RS485 间切换；**P8**=USART3 在 RS232/ATK 模块间切换；**P9**=PA11/PA12 在 USB/CAN 间切换；**P10**=PA5（ADC/TPAD）连接选择；**P11**=USART1 与 CH340 连接选择。
