# io-edge-hub 方案规划

> **项目名称**: `io-edge-hub` — 嵌入式 IO 数据采集边缘节点
> **MCU**: STM32F407VET6 (Cortex-M4, 168MHz, 512KB Flash, 192KB RAM)
> **RTOS**: Zephyr RTOS (最新稳定版)
> **网卡**: W5500 (SPI 接口, 硬件 TCP/IP 协议栈)
> **外部存储**: W25Q128 SPI Flash (16MB)
> **参考实现**: RT-Thread 版本 (已验证硬件) / Zephyr 版本 (已验证实现)
> **代码仓库**: `~/code/app/apps/` (iot-zephyr-app west manifest + Zephyr module)
> **共享库**: `~/code/app/apps/libs/` (`udp_fw_upgrade`, `can_fw_upgrade`)
> **已有 Zephyr 实现**: 已验证实现 (Modbus/FTP/RTC/栈保护/状态 LED 等已验证)
> **文档版本**: v3.4 (v3.3 基础上: UDP 协议精简, 看门狗策略调整, 寄存器重命名, IP 校验统一)
> **日期**: 2026-08-11

---

## 目录

1. [项目概述](#1-项目概述)
2. [硬件架构](#2-硬件架构)
3. [Flash 布局设计](#3-flash-布局设计)
4. [软件架构](#4-软件架构)
5. [模块详细设计](#5-模块详细设计)
6. [通信协议设计](#6-通信协议设计)
7. [项目目录结构](#7-项目目录结构)
8. [Kconfig 配置参考](#8-kconfig-配置参考)
9. [设备树配置参考](#9-设备树配置参考)
10. [构建与部署](#10-构建与部署)
11. [开发计划](#11-开发计划)
12. [风险评估与对策](#12-风险评估与对策)
13. [RT-Thread 到 Zephyr 迁移对照](#13-rt-thread-到-zephyr-迁移对照)

---

## 1. 项目概述

### 1.1 项目定位

`io-edge-hub` 是一个基于 Zephyr RTOS 的工业级 IO 数据采集边缘节点，部署在 STM32F407VET6 MCU 上，通过 W5500 以太网芯片接入网络。设备具备 16 路数字输入采集、8 路数字输出控制、4 路模拟量采集能力，通过 Modbus TCP/RTU 协议向上位机提供实时数据，同时具备历史数据存储、FTP 文件访问、远程固件升级、远程参数配置等完整运维能力。

本项目从已有的 RT-Thread 实现迁移至 Zephyr RTOS，复用已验证的硬件设计和业务逻辑。同时参考已有的 Zephyr 实现，直接复用其已验证的 Modbus RAW ADU TCP 服务器、FTP 服务器、RTC 时间管理、Settings 直接映射保持寄存器等核心模块。主站连接存活检测使用 **TCP Keepalive**（不再使用应用层心跳）。作为 `iot-zephyr-app` 仓库 (`~/code/app/apps/`) 下的一个应用，与已有项目 `n2e-gw` (W5500 + UDP 固件升级) 和 `angle-handler` (CAN 固件升级) 共享底层固件升级库。

> **术语说明**: 本文档沿用工业自动化标准缩写 —
> - **DI** — Digital Input，数字输入（本设备 16 路）
> - **DO** — Digital Output，数字输出（本设备 8 路，含 LED 联动指示）
> - **AI** — Analog Input，模拟输入（本设备 4 路，支持电流 4-20mA / 电压 0-10V）
> - 本设备**无 AO**（Analog Output，模拟输出）。文中 "AI" 均指模拟输入，非人工智能。

### 1.2 功能需求清单

| 编号 | 功能模块 | 说明 | RT-Thread 实现 | Zephyr 实现 |
|------|----------|------|----------------|-------------|
| F1 | 数字输入采集 | 16 路 DI 读取，带使能控制 | GPIO 轮询 + 软件定时器 | **复用已验证实现 `dio.c`** (k_thread + holding_reg) |
| F2 | 数字输出控制 | 8 路 DO 输出 + LED 指示 | GPIO + Modbus 保持寄存器 | **复用已验证实现 `dio.c`** (mb_set_do + LED 联动) |
| F3 | 模拟输入采集 | 4 路 ADC，支持电流(4-20mA)和电压(0-10V) | ADC1 通道 10-13 | **复用已验证实现 `adc.c`** (adc_dt_spec + 工程量转换) |
| F4 | Modbus TCP | 作为 Modbus TCP Server | FreeModbus, 端口 502 | **复用已验证实现 `tcp.c`** (RAW ADU + select() 多路复用) |
| F5 | Modbus RTU | 作为 Modbus RTU Slave (RS485) | UART2 + RS485 | **复用已验证实现 `rtu.c`** (Zephyr modbus serial) |
| F6 | **双通道固件升级** | **同时支持 UDP + CAN 固件升级** | 无 OTA | **共享库 `udp_fw_upgrade` + `can_fw_upgrade`** |
| F7 | SPI Flash 存储 | W25Q128 (16MB) | SFUD + FAL | Zephyr spi_nor + flash_map |
| F8 | LittleFS 文件系统 | 管理历史记录文件 | FATFS | **复用已验证实现 `littlefs_flash` snippet** |
| F9 | FTP 服务 | 历史记录文件下载/上传 | 自定义 FTP + tcpserver | **直接复用已验证实现 `ftp_server/`** (Zephyr 原生, k_mem_slab) |
| F10 | **UDP 参数配置** | 远程配置 IP/采样率/Modbus 参数 | UDP 广播 + 文本协议 | **共享 `udp_fw_upgrade` 库 app handler** |
| F11 | MCUboot 引导 | 安全引导 + 固件签名验证 | 无 bootloader | **MCUboot SWAP_SCRATCH + RSA-2048** |
| F12 | CAN 通信 | CAN1 标准帧收发 + CAN 固件升级 | can_app.c | **共享库 `can_fw_upgrade` + app handler** |
| F13 | 看门狗 | 独立看门狗 (IWDG) + **TCP Keepalive** | 无 | Zephyr watchdog + Modbus TCP **SO_KEEPALIVE** |
| F14 | 时间同步 | RTC | RTC | **复用已验证实现 `time.c`** (RTC→系统时钟同步) |
| F15 | **参数持久化** | 设备参数 Flash 存储 | magic + CRC16 | **Zephyr settings + FCB，直接映射 holding 寄存器** |
| F16 | **密钥哈希校验** | 固件升级时验证签名密钥 | 无 | **`fw_keyhash.h` (SHA-256)** |
| F17 | **版本管理** | git commit 哈希版本号 | 无 | **`fw_gitver.h` (`gen_gitver.py`)** |
| F18 | **TCP Keepalive** | Modbus TCP 主站异常掉线自动断开连接 | 无 | **`SO_KEEPALIVE`** + `CONFIG_NET_TCP_KEEPALIVE` |
| F19 | **网络断连安全** | link down 时清零 DO + 拒绝新连接 | 无 | **复用已验证实现 `tcp.c` NET_EVENT_IF_DOWN** |
| F20 | **栈溢出保护** | 栈溢出时自动重启 | 无 | **复用已验证实现 `main.c` k_sys_fatal_error_handler** |
| F21 | **状态 LED** | MCUboot LED + 主循环闪烁指示 | 无 | **复用已验证实现 `main.c` status LED** |

### 1.3 技术选型依据

- **Zephyr RTOS**: 原生支持 W5500 驱动 (`eth_w5500`)、Modbus 库 (`modbus`)、LittleFS、MCUboot 集成、SPI NOR Flash 驱动 (`spi_nor`)、settings 子系统 (FCB 后端)，生态完整
- **STM32F407VET6**: 512KB Flash 可容纳 MCUboot (64KB) + 应用镜像 (448KB)；192KB RAM 足以支撑网络栈和文件系统
- **W5500**: 硬件 TCP/IP 协议栈降低 MCU 负载，SPI 接口简化硬件设计
- **W25Q128**: 16MB 容量满足 Secondary Slot (448KB) + Scratch (448KB) + 参数存储 (64KB) + 历史记录 (~15MB) 的需求
- **共享库架构**: 复用 `~/code/app/apps/libs/` 下已验证的 `udp_fw_upgrade` 和 `can_fw_upgrade` 库，通过 SYS_INIT 自启动，应用仅需注册 app handler 处理业务命令
- **已验证实现模块复用**: Modbus RAW ADU TCP 服务器 (`tcp.c`)、FTP 服务器 (`ftp_server/`)、ADC/DIO 驱动 (`adc.c`/`dio.c`)、RTC 时间管理 (`time.c`)、历史记录 (`history.c`) 等模块已在 `已验证实现` 项目中验证通过，直接复用可大幅减少开发风险
- **Modbus RAW ADU 模式**: 使用 `MODBUS_MODE_RAW` + 自定义 TCP Server (`select()` 多路复用)，比 Zephyr 内置 Modbus TCP Server 更灵活，可控制客户端连接数、超时、link-down 安全处理
- **Settings 直接映射 holding 寄存器**: 使用 `"modbus/"` 命名空间直接映射 `holding_reg[]` 数组，避免维护两份数据 (参数结构体 + 寄存器数组)，`settings_save()` 全量导出，`settings_save_one()` 增量保存
- **MCUboot SWAP_SCRATCH**: 支持固件回滚 (rollback)，比 Overwrite-only 更安全；与 `n2e-gw` 和 `angle-handler` 项目一致

### 1.4 与已有项目的关系

```
~/code/app/apps/                          # iot-zephyr-app 仓库根目录
├── CMakeLists.txt                        # 顶层 CMake
├── CLAUDE.md                             # 仓库规范
├── libs/                                 # 共享库
│   ├── CMakeLists.txt                    # 生成 fw_gitver.h / fw_keyhash.h
│   ├── udp_fw_upgrade/                   # UDP 固件升级库 (端口 8600)
│   │   ├── udp_fw_upgrade.h
│   │   ├── udp_fw_upgrade.c              # SYS_INIT 自启动, 拥有 RX 线程
│   │   └── Kconfig
│   ├── can_fw_upgrade/                   # CAN 固件升级库 (帧 ID 0x101-0x105)
│   │   ├── can_fw_upgrade.h
│   │   ├── can_fw_upgrade.c              # SYS_INIT 自启动, 拥有 RX 线程
│   │   └── Kconfig
│   ├── gen_gitver.py                     # 生成 fw_gitver.h (git commit hash)
│   └── gen_keyhash.py                    # 生成 fw_keyhash.h (签名密钥 SHA-256)
└── applications/
    ├── n2e-gw/                           # 已有: W5500 + UDP 固件升级参考
    ├── angle-handler/                    # 已有: CAN 固件升级参考
    └── io-edge-hub/                      # 本项目 (同时使用 UDP + CAN)
```

> **关键设计**: `udp_fw_upgrade` 和 `can_fw_upgrade` 是自包含库，通过 `SYS_INIT` 自动初始化，各自拥有独立的 RX 线程和配置端口。应用通过 `udp_fw_set_app_handler()` / `can_fw_set_app_handler()` 注册业务命令回调，固件升级命令 (0x01-0x05 / 0x101-0x105) 由库内部处理，业务命令 (0x10+ / 其他帧 ID) 分发给应用。

---

## 2. 硬件架构

### 2.1 系统硬件框图

```
                    +------------------------------------------------------+
                    |              STM32F407VET6                            |
                    |         (Cortex-M4 @ 168MHz)                          |
                    |         512KB Flash / 192KB RAM                       |
                    |                                                       |
  DI1 --- (光耦) -->|  GPIO: PD3,PD4,PD5,PD6,PB5,PB6,PB7,PB8,             |
  DI2 --- (光耦) -->|        PB9,PB10,PB11,PD2,PB0,PB1,PB3,PB4  (16ch DI)  |
  ...               |                                                       |
  DI16 - (光耦) -->|                                                       |
                    |                                                       |
  DO1 <-- (驱动) --|  GPIO: PD7,PD8,PD9,PD10,PD11,PD12,PD13,PD14 (8ch DO) |
  ...               |  LED:  PE8,PE9,PE10,PE11,PE12,PE13,PE14,PE15 (8ch)  |
  DO8 <-- (驱动) --|                                                       |
                    |                                                       |
  AI1 --- (调理) -->|  ADC1: PC0(IN10),PC1(IN11),PC2(IN12),PC3(IN13)       |
  AI2 --- (调理) -->|        (4ch, 12-bit)                                  |
  AI3 --- (调理) -->|        AI0-1: 电流输入 4-20mA                         |
  AI4 --- (调理) -->|        AI2-3: 电压输入 0-10V                          |
                    |                                                       |
                    |  SPI1 -----------------------------+                  |
                    |    (PA5/PA6/PA7, CS=PA4)           |                  |
                    |                                     v                  |
                    |                   +----------+                       |
                    |                   | W25Q128  |                       |
                    |                   |(16MB SPI |                       |
                    |                   |  Flash)  |                       |
                    |                   +----------+                       |
                    |                                                       |
                    |  SPI2 -----------------------------+                  |
                    |    (PB13/PB14/PB15, CS=PB12)       |                  |
                    |    LED=PE7, RST=PD0, INT=PD1                          v                  |
                    |                   +----------+                       |
                    |                   |  W5500   |---- Ethernet ---------|
                    |                   | (SPI)    |     RJ45              |
                    |                   +----------+                       |
                    |                                                       |
                    |  USART2 ----------------------+                       |
                    |    (PA2/PA3)   DE/RE=PA1     |                       |
                    |                               v                       |
                    |                   +----------+                       |
                    |                   | MAX485   |---- RS485 ------------|
                    |                   |(RS485)   |     Modbus RTU        |
                    |                   +----------+                       |
                    |                                                       |
                    |  USART1 (PA9/PA10) --- Console / Shell               |
                    |  CAN1 (PA11/PA12) --- CAN 总线 + CAN 固件升级       |
                    |                                                       |
  SWD Debug ------>|  PA13/PA14                                            |
                    +------------------------------------------------------+
```

> **时钟配置**: 系统使用 HSE 外部 13MHz 晶振 (`PH0`=OSC_IN / `PH1`=OSC_OUT) 经 PLL 倍频至 168MHz (PLLM=13, PLLN=336, PLLP=2)。HSE 精度远高于 HSI，对 ADC 采样和网络通信更友好。W5500 的 RST/INT 引脚使用 PD0/PD1（与 HSE 无关，HSE 走 PH0/PH1）。W5500 自带 25MHz 独立晶振。

### 2.2 SPI 总线分配

| SPI 总线 | APB 总线 | 最大时钟 | 连接设备 | 建议时钟 | 说明 |
|----------|----------|----------|----------|----------|------|
| SPI1 | APB2 (84MHz) | 84MHz | W25Q128 Flash | 42MHz | Flash 高速读写，支持 SFDP |
| SPI2 | APB1 (42MHz) | 42MHz | W5500 以太网 | 21MHz | APB1 限频，W5500 SPI 最高 80MHz 但 21MHz 足够 |

> **设计决策**（与 RT-Thread 实现一致）: SPI1 连接 W25Q128 Flash（高速读写需求），SPI2 连接 W5500 以太网。两条独立 SPI 总线避免竞争。MCUboot 引导阶段只需访问 SPI1 上的 Flash (Secondary Slot + Scratch)，不涉及 SPI2 上的 W5500。

### 2.3 引脚分配

> 以下引脚分配完全基于 RT-Thread 实现的已验证硬件，板卡为 LCKFB STM32F407VET6。

#### 2.3.1 SPI1 — W25Q128 SPI Flash

| 引脚 | 功能 | 方向 | 说明 |
|------|------|------|------|
| PA4 | SPI1_CS | GPIO Out | Flash 片选，低有效 |
| PA5 | SPI1_SCK | AF | SPI1 时钟 |
| PA6 | SPI1_MISO | AF | Flash 主入从出 |
| PA7 | SPI1_MOSI | AF | Flash 主出从入 |

#### 2.3.2 SPI2 — W5500 以太网

| 引脚 | 功能 | 方向 | 说明 |
|------|------|------|------|
| PB12 | SPI2_CS | GPIO Out | W5500 片选，低有效 |
| PB13 | SPI2_SCK | AF | SPI2 时钟 |
| PB14 | SPI2_MISO | AF | W5500 主入从出 |
| PB15 | SPI2_MOSI | AF | W5500 主出从入 |
| PD0 | W5500_RST | GPIO Out | W5500 硬件复位，低有效 |
| PD1 | W5500_INT | GPIO In | W5500 中断输出，低有效（可选轮询模式） |
| PE7 | ETH_LED | GPIO Out | 以太网状态指示灯 |

> **引脚说明**: HSE 外部晶振使用 `PH0`(OSC_IN)/`PH1`(OSC_OUT)，与 PD0/PD1 无关。PD0/PD1 专用于 W5500 复位/中断控制。

#### 2.3.3 数字输入 (16 通道)

| 引脚 | 编号 | 说明 |
|------|------|------|
| PD3 | DI1 | 数字输入 1，光耦隔离 |
| PD4 | DI2 | 数字输入 2 |
| PD5 | DI3 | 数字输入 3 |
| PD6 | DI4 | 数字输入 4 |
| PB5 | DI5 | 数字输入 5 |
| PB6 | DI6 | 数字输入 6 |
| PB7 | DI7 | 数字输入 7 |
| PB8 | DI8 | 数字输入 8 |
| PB9 | DI9 | 数字输入 9 |
| PB10 | DI10 | 数字输入 10 |
| PB11 | DI11 | 数字输入 11 |
| PD2 | DI12 | 数字输入 12 |
| PB0 | DI13 | 数字输入 13 |
| PB1 | DI14 | 数字输入 14 |
| PB3 | DI15 | 数字输入 15 |
| PB4 | DI16 | 数字输入 16 |

> **引脚模式**: 下拉输入 (`GPIO_INPUT` + pull-down)，光耦隔离输入。

#### 2.3.4 数字输出 (8 通道) + LED 指示

| 引脚 | 编号 | LED 引脚 | 说明 |
|------|------|----------|------|
| PD7 | DO1 | PE8 | 数字输出 1 + 状态 LED |
| PD8 | DO2 | PE9 | 数字输出 2 + 状态 LED |
| PD9 | DO3 | PE10 | 数字输出 3 + 状态 LED |
| PD10 | DO4 | PE11 | 数字输出 4 + 状态 LED |
| PD11 | DO5 | PE12 | 数字输出 5 + 状态 LED |
| PD12 | DO6 | PE13 | 数字输出 6 + 状态 LED |
| PD13 | DO7 | PE14 | 数字输出 7 + 状态 LED |
| PD14 | DO8 | PE15 | 数字输出 8 + 状态 LED |

#### 2.3.5 ADC1 — 模拟输入 (4 通道)

| 引脚 | ADC 通道 | 编号 | 信号类型 | 工程量转换 |
|------|----------|------|----------|------------|
| PC0 | ADC1_IN10 | AI1 | 电流 4-20mA | `value = 7.414 * adc_voltage / 10` (结果x100, 单位 0.01mA) |
| PC1 | ADC1_IN11 | AI2 | 电流 4-20mA | 同上 |
| PC2 | ADC1_IN12 | AI3 | 电压 0-10V | `value = 3.7037 * adc_voltage / 10` (结果x100, 单位 0.01V) |
| PC3 | ADC1_IN13 | AI4 | 电压 0-10V | 同上 |

#### 2.3.6 USART2 — Modbus RTU (RS485)

| 引脚 | 功能 | 方向 | 说明 |
|------|------|------|------|
| PA2 | USART2_TX | AF | RS485 发送 |
| PA3 | USART2_RX | AF | RS485 接收 |
| PA1 | RS485_DE | GPIO Out | MAX485 驱动使能/接收使能 |

#### 2.3.7 USART1 — 控制台

| 引脚 | 功能 | 方向 | 说明 |
|------|------|------|------|
| PA9 | USART1_TX | AF | 调试串口发送 |
| PA10 | USART1_RX | AF | 调试串口接收 |

#### 2.3.8 CAN1 — CAN 总线 + CAN 固件升级

| 引脚 | 功能 | 方向 | 说明 |
|------|------|------|------|
| PA11 | CAN1_RX | AF | CAN 总线接收 |
| PA12 | CAN1_TX | AF | CAN 总线发送 |

> CAN1 同时用于业务通信和 CAN 固件升级。`can_fw_upgrade` 库通过 SYS_INIT 初始化 CAN (全接受滤波器)，帧 ID 0x101-0x105 由库内部处理 (固件升级)，其他帧 ID 分发给应用的 app handler。CAN ID 和波特率可通过 Modbus 保持寄存器配置。

#### 2.3.9 系统引脚

| 引脚 | 功能 | 说明 |
|------|------|------|
| PA13 | SWDIO | SWD 调试接口 |
| PA14 | SWCLK | SWD 调试接口 |

> **HSE 时钟**: 系统使用 HSE 外部 13MHz 晶振 (`PH0`=OSC_IN / `PH1`=OSC_OUT) 经 PLL 倍频到 168MHz (PLLM=13, PLLN=336, PLLP=2, AHB=/1, APB1=/4, APB2=/2)。MAC 地址取自 STM32 96-bit UID（唯一设备 ID），OUI 前缀 `00:08:DC`，确保每板不同。

### 2.4 硬件设计要点

1. **数字输入隔离**: 每路 DI 使用光耦 (如 TLP281) 隔离，输入侧支持 24V 工业电平，MCU 侧下拉输入
2. **数字输出驱动**: DO 经驱动芯片 (如 ULN2003) 输出，LED 指示输出状态
3. **模拟输入调理**: AI1-2 为电流输入 (4-20mA) 需采样电阻+运放调理；AI3-4 为电压输入 (0-10V) 需分压电路
4. **RS485 防护**: RS485 总线端加 TVS 管和热插拔保护，终端电阻 120 欧姆
5. **以太网接口**: W5500 外接 RJ45 + 网络变压器 (或集成变压器 RJ45)
6. **SPI Flash**: W25Q128 工作电压 3.3V，与 STM32 I/O 电平兼容
7. **时钟**: 使用 HSE 外部 13MHz 晶振 (`PH0`/`PH1`)，PLL 倍频至 168MHz；W5500 自带 25MHz 独立晶振

---

## 3. Flash 布局设计

### 3.1 内部 Flash 布局 (512KB)

STM32F407VET6 内部 Flash 共 512KB (0x08000000 - 0x0807FFFF)，划分为两个区域：

```
0x08000000 +-------------------------------+  offset 0x00000
           |                               |
           |    MCUboot Bootloader         |  64KB (0x10000)
           |    (引导加载器)                |
           |                               |
0x08010000 +-------------------------------+  offset 0x10000
           |                               |
           |                               |
           |    Primary Slot (Slot 0)      |  448KB (0x70000)
           |    (应用程序主槽位)            |
           |                               |
           |                               |
0x08080000 +-------------------------------+  offset 0x80000
```

| 区域 | 起始地址 | 大小 | 说明 |
|------|----------|------|------|
| MCUboot | 0x08000000 | 64KB | 引导加载器，负责镜像验证和升级 |
| Primary Slot (Slot 0) | 0x08010000 | 448KB | 应用程序主运行槽位 |

> **设计说明**: Secondary Slot 和 Scratch 区域放在外部 SPI Flash 上，释放内部 Flash 空间给应用程序。RT-Thread 原实现无 bootloader，内部 512KB 全部用于应用。迁移至 Zephyr + MCUboot 后，应用可用空间为 448KB。

### 3.2 外部 SPI Flash 布局 (16MB, W25Q128)

> **v3.0 变更**: MCUboot 模式从 Overwrite-only 改为 SWAP_SCRATCH，增加 Scratch 分区。Settings 分区从 1MB 缩减至 64KB (FCB 后端)。

```
0x00000000 +-------------------------------+
           |                               |
           |   MCUboot Secondary Slot      |  448KB (0x70000)
           |   (Slot 1, 升级镜像暂存)       |  与内部 Primary Slot 大小一致
           |                               |
0x00070000 +-------------------------------+
           |                               |
           |   MCUboot Scratch             |  448KB (0x70000)
           |   (SWAP_SCRATCH 交换暂存区)    |  SWAP_SCRATCH 模式必需
           |                               |
0x000E0000 +-------------------------------+
           |                               |
           |   Settings Storage (FCB)      |  64KB (0x10000)
           |   (设备参数持久化)             |  Zephyr settings FCB 后端
           |                               |
0x000F0000 +-------------------------------+
           |                               |
           |                               |
           |   LittleFS Partition          |  ~15MB (0xF10000)
           |   (历史记录文件系统)           |
           |                               |
           |                               |
0x01000000 +-------------------------------+  (16MB 总容量)
```

| 分区 | 偏移地址 | 大小 | 用途 |
|------|----------|------|------|
| Slot 1 (Secondary) | 0x000000 | 448KB | MCUboot 升级镜像暂存区，UDP/CAN 固件升级写入目标 |
| Scratch | 0x070000 | 448KB | MCUboot SWAP_SCRATCH 交换暂存区 |
| Settings (FCB) | 0x0E0000 | 64KB | Zephyr settings FCB 后端，设备参数持久化 |
| LittleFS (历史) | 0x0F0000 | ~15MB | IO 历史记录文件存储 |

> **分区调整说明**: 相比 v2.0 (Overwrite-only, 无 Scratch)，v3.0 增加 Scratch 分区以支持 SWAP_SCRATCH 模式。Settings 分区从 1MB 缩减至 64KB (FCB 后端高效，无需 1MB)。LittleFS 分区仍保留 ~15MB，支持 10 个 1MB 历史文件的轮转存储。

### 3.3 MCUboot 配置策略

| 配置项 | 选择 | 说明 |
|--------|------|------|
| **升级模式** | **SWAP_SCRATCH** | 交换 Primary/Secondary Slot 镜像，支持固件回滚 (rollback)，与 n2e-gw / angle-handler 一致 |
| 镜像签名 | **RSA-2048** | 签名验证，防止未授权固件刷入 (与仓库 n2e-gw/angle-handler 一致；`fw_keyhash`/`gen_keyhash.py` 仅支持 RSA，详见 §3.3 注) |
| 签名密钥 | `boards/${BOARD}.pem` | 每个应用独立签名密钥 (与仓库约定一致) |
| **密钥哈希校验** | **`fw_keyhash.h`** | 固件升级时校验签名密钥 SHA-256 (32B)，防止错误密钥签名的固件刷入 |
| Secondary Slot 位置 | 外部 SPI Flash (SPI1) | MCUboot 需配置 SPI Flash 驱动 |
| Scratch 位置 | 外部 SPI Flash (SPI1) | SWAP_SCRATCH 模式必需 |
| **回滚支持** | **是** | SWAP_SCRATCH 模式天然支持回滚，升级失败可恢复旧固件 |
| 日志 | 可选 | 开发阶段开启，生产关闭以减小 bootloader 体积 |

> **MCUboot SWAP_SCRATCH 工作流程**:
> 1. 上电 -> MCUboot 启动
> 2. 检查 Primary Slot 镜像有效性 (image trailer)
> 3. 检查 Secondary Slot (外部 Flash SPI1) 是否有新镜像
> 4. 若有新镜像且签名验证通过:
>    - 将 Primary Slot 镜像复制到 Scratch 区域
>    - 将 Secondary Slot 镜像复制到 Primary Slot
>    - 将 Scratch 区域镜像复制到 Secondary Slot (旧固件变为可回滚)
>    - 标记已升级
> 5. 跳转到 Primary Slot 执行应用程序
> 6. 若无新镜像 -> 直接跳转到 Primary Slot
> 7. 若新固件运行异常 -> MCUboot 下次启动时自动回滚到旧固件

> **sysbuild.conf 配置** (与 n2e-gw 一致):
> ```
> SB_CONFIG_MCUBOOT_MODE_SWAP_SCRATCH=y
> SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="${APP_DIR}/boards/${BOARD}.pem"
> ```

### 3.4 参数存储结构 (Zephyr settings + FCB — 直接映射 holding_reg)

> **v3.1 变更**: 不再使用独立 `persist.c` 和 `io/` 命名空间。Settings 直接映射 `holding_reg[]` 数组，使用 `modbus/` 命名空间。复用已验证实现 `function.c` 的 settings handler 实现。

#### 3.4.1 FCB 后端

- **分区**: `storage_partition` (外部 Flash 0x0E0000, 64KB)
- **设备树**: `zephyr,settings-partition = &storage_partition` (chosen 节点)
- **磨损均衡**: FCB 采用追加写入 + 循环覆盖，天然支持磨损均衡
- **全量保存**: 写 `HOLDING_CONFIG_SAVE` (0x10) 寄存器触发 `settings_save()` 全量导出

#### 3.4.2 参数键值定义 (直接映射 holding_reg, modbus/ 命名空间)

> **v3.1 变更**: 复用已验证实现 settings handler，键值直接映射 `holding_reg[]` 数组元素，无需中间结构体。

| Settings Key | 对应 holding_reg | 大小 | 默认值 | 说明 | Modbus 地址 |
|--------------|-------------------|------|--------|------|-------------|
| `modbus/ai/enable` | HOLDING_AI_ENABLE_IDX | 2B | 0x000F | AI 使能 bitmap | 0x02 |
| `modbus/ai/time` | HOLDING_AI_SAMPLE_MS_IDX | 2B | 200 | AI 采样间隔 (ms) | 0x04 |
| `modbus/di/enable` | HOLDING_DI_ENABLE_IDX | 2B | 0xFFFF | DI 使能 bitmap | 0x01 |
| `modbus/di/time` | HOLDING_DI_SAMPLE_MS_IDX | 2B | 200 | DI 采样间隔 (ms) | 0x03 |
| `modbus/history` | HOLDING_HISTORY_ENABLE_IDX | 2B | 0 | 历史记录保存使能 | 0x05 |
| `modbus/can/id` | HOLDING_CAN_ID_IDX | 2B | 0x0111 | CAN ID (标准帧) | 0x06 |
| `modbus/can/bps` | HOLDING_CAN_BAUDRATE_IDX | 2B | 10 | CAN 波特率 (x1000) | 0x07 |
| `modbus/rs485_bps` | HOLDING_RS485_BAUDRATE_IDX | 2B | 9600 | RS485 波特率 | 0x08 |
| `modbus/slave_id` | HOLDING_SLAVE_ID_IDX | 2B | 1 | Modbus RTU Slave ID | 0x09 |
| `modbus/ip` | HOLDING_IP_OCTET1..4 | 8B | 192.168.12.101 | IP 地址 (4x uint16_t) | 0x0A-0x0D |

#### 3.4.3 实现模式 (复用已验证实现 function.c)

```c
/* 静态 handler 定义 — 直接映射 holding_reg[] (复用已验证实现) */
SETTINGS_STATIC_HANDLER_DEFINE(modbus, "modbus",
    mb_handle_get,      /* get: 从 holding_reg 读取 */
    mb_handle_set,      /* set: 从 FCB 加载到 holding_reg */
    NULL,               /* commit */
    mb_handle_export    /* export: 导出 holding_reg 到 FCB */
);

/* 全量保存: 写 HOLDING_CONFIG_SAVE (0x10) 寄存器触发 */
static int holding_reg_wr(uint16_t addr, uint16_t reg) {
    holding_reg[addr] = reg;
    if (addr == HOLDING_CONFIG_SAVE_IDX) {
        holding_reg[addr] = 0;  /* 恢复为 0 */
        settings_save();        /* 全量导出到 FCB */
    }
    return 0;
}

/* 后端初始化 (SYS_INIT, priority 10) */
static int main_settings_init(void) {
    return settings_subsys_init();
}
SYS_INIT(main_settings_init, APPLICATION, 10);
```

> **IP 合法性检查** (统一公共函数 `ip_addr_valid(a,b,c,d)`，定义在 `function.c`、声明在 `init.h`):
> 拒绝以下 IP：末字节为 `0` (网络地址) / `0xFF` (广播)；首字节为 `0` (本网络) / `127` (环回) / `224-239` (组播 D 段) / `>=240` (保留 E 段，含限定广播)。非法 IP 既不会被 `UDP_CMD_SET_IP` 写入，也不会被 settings 导出，保留上次有效值。

> **出厂恢复**: 出厂恢复通过擦除整个 `storage_partition` 实现:
> ```c
> const struct flash_area *fa;
> flash_area_open(FLASH_AREA_ID(storage), &fa);
> flash_area_erase(fa, 0, fa->fa_size);
> flash_area_close(fa);
> settings_subsys_init();  /* 重新初始化 */
> ```

### 3.5 双通道固件升级流程

#### 3.5.1 UDP 固件升级流程 (共享库 `udp_fw_upgrade`)

```
上位机                              io-edge-hub                     MCUboot
  |                                    |                               |
  |-- FW_START(0x01) ----------------->|                               |
  |   [size 4B LE][keyhash 32B]        |                               |
  |                                    |-- 校验 keyhash vs fw_keyhash.h |
  |                                    |-- 擦除 Slot1 (外部Flash)       |
  |                                    |-- flash_img_init()             |
  |<-- FW_START_ACK -------------------|                               |
  |                                    |                               |
  |-- FW_DATA(0x02) ------------------>|                               |
  |   [data <=511B]                    |-- flash_img_buffered_write()   |
  |<-- FW_DATA_ACK(offset) ------------|                               |
  |       ... (重复) ...               |                               |
  |                                    |                               |
  |-- FW_END(0x03) ------------------->|                               |
  |   [test 1B][crc 2B LE]             |-- CRC16-CCITT 校验             |
  |                                    |-- flash_img_buffered_write flush|
  |                                    |-- boot_request_upgrade()       |
  |<-- FW_END_ACK(status) -------------|                               |
  |                                    |-- 系统重启                     |
  |                                    |                               |
  |-- GET_VERSION(0x04) -------------->|                               |
  |<-- 版本字符串 ----------------------|  (fw_gitver.h: v<M>.<m>.<p>_<6hex>)
  |                                    |                               |
  |                                    |        +-----------------------+
  |                                    |        | SWAP_SCRATCH:         |
  |                                    |        | 检测 Slot1 新镜像     |
  |                                    |        | 验证 RSA-2048 签名    |
  |                                    |        | 交换 Slot0 <-> Slot1  |
  |                                    |        | 跳转应用 ------------>| (新固件运行)
```

> **UDP 固件升级库特性** (`udp_fw_upgrade`):
> - **端口**: 8600 (`CONFIG_UDP_FW_CONFIG_PORT`, 可配置)
> - **自启动**: `SYS_INIT` 创建配置 socket，拥有独立 RX 线程
> - **网络等待**: 等待 `NET_EVENT_IF_UP` 后才绑定 socket
> - **命令处理**: 0x01-0x05 由库内部处理，0x10+ 分发给 app handler
> - **跨子网回复**: `is_same_subnet()` 判断回复路由 (同子网单播，跨子网广播)
> - **广播限制**: `CONFIG_UDP_FW_REPLY_BCAST_RESTRICT=y`，仅允许的命令跨子网广播回复

#### 3.5.2 CAN 固件升级流程 (共享库 `can_fw_upgrade`)

```
上位机 (CAN)                         io-edge-hub                     MCUboot
  |                                    |                               |
  |-- 0x104: Keyhash 帧 x5 ----------->|                               |
  |   [seq 1B][chunk 7B] x5 = 32B     |-- 累积到 rx_keybuf[32]        |
  |                                    |                               |
  |-- 0x101: START_UPDATE ------------>|                               |
  |                                    |-- 校验 keyhash vs fw_keyhash.h |
  |                                    |-- 擦除 Slot1 (外部Flash)       |
  |                                    |-- flash_img_init()             |
  |<-- 0x102: START_REPLY -------------|                               |
  |                                    |                               |
  |-- 0x103: FW_DATA xN -------------->|                               |
  |   [8B per frame]                   |-- flash_img_buffered_write(8B) |
  |                                    |-- 每 64B 回复一次 OFFSET       |
  |<-- 0x102: OFFSET_REPLY ------------|                               |
  |       ... (重复) ...               |                               |
  |                                    |                               |
  |-- 0x101: CONFIRM ----------------->|                               |
  |                                    |-- flash_img flush             |
  |                                    |-- 校验镜像大小                 |
  |                                    |-- boot_request_upgrade()       |
  |<-- 0x102: CONFIRM_REPLY -----------|                               |
  |                                    |-- 系统重启                     |
  |                                    |                               |
  |-- 0x101: VERSION ----------------->|                               |
  |<-- 0x105: 版本字符串帧 ------------|  (fw_gitver.h: v<M>.<m>.<p>_<6hex>)
  |                                    |                               |
  |                                    |        +-----------------------+
  |                                    |        | SWAP_SCRATCH:         |
  |                                    |        | 检测 Slot1 新镜像     |
  |                                    |        | 验证 RSA-2048 签名    |
  |                                    |        | 交换 Slot0 <-> Slot1  |
  |                                    |        | 跳转应用 ------------>| (新固件运行)
```

> **CAN 固件升级库特性** (`can_fw_upgrade`):
> - **自启动**: `SYS_INIT` 初始化 CAN (设置波特率 + 启动 + 全接受滤波器)，拥有独立 RX 线程
> - **帧 ID**: 0x101 (cmd), 0x102 (reply), 0x103 (data), 0x104 (keyhash), 0x105 (version)
> - **波特率**: `CONFIG_CAN_FW_UPGRADE_BITRATE` (默认 250000)
> - **数据帧**: 每帧 8B 数据，每 64B 回复一次 OFFSET 确认
> - **密钥哈希**: 5 帧 x 7B = 35B (有效 32B SHA-256)
> - **业务帧分发**: 非 0x101-0x105 的帧分发给 app handler

---

## 4. 软件架构

### 4.1 软件分层架构

```
+-----------------------------------------------------------------------------+
|                        应用层 (Application Layer)                            |
|  +----------+ +----------+ +----------+ +----------+ +----------+          |
|  | IO 采集   | | Modbus   | | FTP 服务  | | UDP App  | | CAN App  |          |
|  | dio.c     | | modbus/  | |ftp_server| | udp.c    | | can.c    |          |
|  | adc.c     | | tcp/rtu  | |          | |(0x10+cmd)| |(业务帧)  |          |
|  +-----+----+ +-----+----+ +-----+----+ +-----+----+ +-----+----+          |
+-------+-----------+-----------+-----------+-----------+--------------------+
|       |   服务层 (Service Layer)  |            |            |                |
|  +----v------------v-----------v-----------v-----------v------+            |
|  |  历史存储 (history.c)      |  DO 控制 (dio.c mb_set_do)   |            |
|  |  时间同步 (time.c)         |  看门狗 (watchdog.c)          |            |
|  |  Settings (function.c)     |  系统初始化 (main.c)          |            |
|  +----+----------------------+-----------------+------------+              |
+-------+----------------------------------------+----------------------------+
|       |     共享库层 (Shared Libraries, libs/)  |                          |
|  +----v----------------------+  +--------------v-----------+              |
|  | udp_fw_upgrade            |  | can_fw_upgrade             |             |
|  | (SYS_INIT 自启动)          |  | (SYS_INIT 自启动)          |             |
|  | 端口 8600, RX 线程         |  | 帧ID 0x101-0x105, RX 线程 |             |
|  | FW_START/DATA/END/VERSION  |  | START_UPDATE/CONFIRM/...  |             |
|  | app handler (0x10+)        |  | app handler (业务帧)      |             |
|  +----+----------------------+  +--------------+-----------+              |
+-------+----------------------------------------+----------------------------+
|       |        存储层 (Storage Layer)            |                          |
|  +----v----+  +--------------+  +-------------+  +-----v-----------+      |
|  |LittleFS |  | Flash 驱动    |  | Settings/FCB |  | Flash Map       |      |
|  |(fs)     |  |(spi_nor)     |  |(参数持久化)   |  |(分区管理)       |      |
|  +----+----+  +------+-------+  +------+------+  +-----------------+      |
+-------+--------------+------------+------+---------------------------------+
|       |     Zephyr 内核 / 驱动层 (Kernel / Driver Layer)                   |
|  +----v--------------v-----------v---------------------------------------+  |
|  |  GPIO  |  ADC  |  SPI  |  UART  |  ETH(W5500)  |  CAN  | IWDG | RTC  |  |
|  |  线程调度  |  内存管理  |  网络栈 (TCP/UDP)  |  Socket API  | Net Mgmt |  |
|  +---------------------------------------------------------------------+  |
+----------------------------------------------------------------------------+
|                    硬件层 (Hardware)                                         |
|  STM32F407VET6 | W5500 | W25Q128 | MAX485 | 光耦/ADC | CAN收发器             |
+----------------------------------------------------------------------------+
```

> **v3.1 架构变化**: 大量模块从 已验证实现 直接复用 (Modbus RAW ADU TCP/RTU, FTP, DI/DO/AI, 心跳, RTC, 栈溢出保护, 状态 LED, 历史记录)。Settings 直接映射 `holding_reg[]`，消除独立参数结构体。应用层仅需实现 UDP/CAN app handler 和网络初始化适配。

### 4.2 线程模型

| 线程名称 | Zephyr 优先级 | 栈大小 | 职责 | 来源 |
|----------|--------------|--------|------|------|
| main | 0 (最高) | 2048B | 系统初始化，MAC 设置，静态 IP 配置，等待网络 link up，状态 LED 闪烁 | main.c |
| **udp_fw_rx** | 8 | 1024B | **UDP 固件升级 + app 命令处理** (共享库) | udp_fw_upgrade.c (SYS_INIT) |
| **can_fw_rx** | 8 | 1024B | **CAN 固件升级 + 业务帧分发** (共享库) | can_fw_upgrade.c (SYS_INIT) |
| **di** | 1 | 512B | **DI 定时采样** (复用已验证实现 `dio.c`) | dio.c (K_THREAD_DEFINE) |
| **adc_io** | 1 | 512B | **AI 定时采样** (复用已验证实现 `adc.c`) | adc.c (K_THREAD_DEFINE) |
| modbus_rtu | 13 | 1024B | Modbus RTU Slave (复用已验证实现 `rtu.c`) | rtu.c (SYS_INIT) |
| **mb_tcp** | 13 | 2048B | **Modbus TCP RAW ADU Server** — select() 多路复用，3 客户端 (复用已验证实现 `tcp.c`) | tcp.c (K_THREAD_DEFINE) |
| **ftp** | 13 | 2048B | **FTP 服务器** — 控制连接 + 数据连接 (复用已验证实现 `ftp_server/`) | ftpd.c (K_THREAD_DEFINE) |
| history_writer | 6 | 2048B | 从 k_work + k_fifo 取数据写入 LittleFS (复用已验证实现 `history.c`) | history.c (K_WORK) |
| **udp_tx** | 7 | 1024B | **UDP 异步发送** (msgq + 低优先级线程) | udp.c |

> **v3.1 线程变化**: 从 已验证实现 复用的线程使用其原始优先级和栈大小。DI/AI 采样线程栈仅 512B (已验证实现足够)。Modbus TCP 栈 2048B (select() 需要较大栈)。总栈消耗约 15KB。
>
> **v3.3 变更**: 移除 `heart` 心跳线程（应用层心跳废除，改由 Modbus TCP socket 的 `SO_KEEPALIVE` 检测主站连接存活，无独立线程开销）。
>
> **固件升级库线程优先级**: `udp_fw_rx`/`can_fw_rx` 优先级为库 Kconfig 默认 `CONFIG_UDP_FW_RX_PRIORITY` / `CONFIG_CAN_FW_UPGRADE_RX_PRIORITY`（均为 8），并在 §8.1 prj.conf 显式固定为 8。如需更高优先级，改这两个 Kconfig 即可。

### 4.3 网络初始化流程

> **v3.1 变更**: 静态 IP 从 `holding_reg[]` 读取 (复用已验证实现 `init.c`)。`NET_EVENT_IF_DOWN` 回调清零 DO 输出 + 设 `link_down` 标志拒绝新连接 (复用已验证实现 `tcp.c`)。

```
上电
  |
  v
MCUboot 引导 -> 跳转应用
  |
  v
main()
  |-- settings_subsys_init() (SYS_INIT, priority 10, 加载持久化参数到 holding_reg[])
  |-- 初始化 Flash + LittleFS (SYS_INIT)
  |-- 初始化 IO (DI/AI/DO) (SYS_INIT, 复用已验证实现 dio_init)
  |-- udp_fw_upgrade SYS_INIT (创建配置 socket, 等待 NET_EVENT_IF_UP)
  |-- can_fw_upgrade SYS_INIT (初始化 CAN, 启动 RX 线程)
  |-- modbus_init() (SYS_INIT, priority 11):
  |   |-- 从 holding_regs[] 默认值初始化所有寄存器
  |   |-- settings_load() 加载持久化参数覆盖默认值
  |   |-- 从 holding_reg[HOLDING_IP_OCTET1..4] 读取 IP
  |   |-- net_if_ipv4_addr_add() + netmask /24
  |   +-- 启动 RTU (不依赖网络)
  |-- 启动 DI/AI 采样线程 (K_THREAD_DEFINE, 复用已验证实现)
  +-- main 线程:
       |
       |-- derive_mac_from_uid()           # 从 STM32 96-bit UID 生成 MAC
       |   # OUI: 00:08:DC, 后3字节从 UID 计算
       |
       |-- net_mgmt(SET_MAC_ADDRESS)       # 设置 MAC (接口 up 之前)
       |   # 需要 CONFIG_ETH_NET_IF_NO_AUTO_START=y
       |
       |-- net_if_up()                     # 启动网络接口
       |
       |-- k_sem_take(&net_link_sem, 5s)   # 等待 NET_EVENT_IF_UP
       |   # NET_MGMT_REGISTER_EVENT_HANDLER 监听 IF_UP/IF_DOWN
       |   # IF_DOWN: 清零 DO + 设 link_down 标志 (复用已验证实现)
       |
       +-- 网络服务启动 (link up 后):
           |-- Modbus TCP Server (端口 502, K_THREAD_DEFINE, 复用已验证实现 tcp.c)
           |-- FTP Server (端口 21, K_THREAD_DEFINE, 复用已验证实现 ftp_server/)
           +-- udp_fw_upgrade 自动绑定 socket (NET_EVENT_IF_UP 触发)
```

> **关键配置** (prj.conf):
> - `CONFIG_ETH_NET_IF_NO_AUTO_START=y` — 禁止网络接口自动启动，允许在接口 up 之前设置 MAC
> - `CONFIG_NET_L2_ETHERNET_MGMT=y` — 运行时 MAC 地址设置
> - **不配置** `CONFIG_NET_DHCPV4` — 纯静态 IP，无 DHCP
> - `CONFIG_NET_CONFIG_AUTO_INIT=n` — 禁用 Zephyr 默认网络初始化，应用自行管理

> **网络事件管理**:
> ```c
> NET_MGMT_REGISTER_EVENT_HANDLER(net_event_handler,
>     NET_EVENT_IF_UP | NET_EVENT_IF_DOWN | NET_EVENT_IPV4_ADDR_ADD,
>     net_event_cb, NULL);
>
> static void net_event_cb(struct net_mgmt_event_callback *cb)
> {
>     if (cb->event == NET_EVENT_IF_UP) {
>         k_sem_give(&net_link_sem);  /* 通知主线程网络就绪 */
>     } else if (cb->event == NET_EVENT_IF_DOWN) {
>         /* 标记网络断开，暂停网络服务 */
>     }
> }
> ```

### 4.4 共享数据结构

```c
/* IO 通道数量定义 (与 RT-Thread / 已验证实现 一致) */
#define DI_NUM    16
#define AI_NUM    4
#define DO_NUM    8

/* Modbus 寄存器数组 (复用已验证实现 function.c 模式)
 * holding_reg[] 和 input_reg[] 是唯一的参数数据源
 * settings 直接映射到 holding_reg[]，无需额外参数结构体
 */
static uint16_t holding_reg[CONFIG_MODBUS_HOLDING_REGISTER_NUMBERS]; /* 18 个 */
static uint16_t input_reg[CONFIG_MODBUS_INPUT_REGISTER_NUMBERS];     /* 6 个 */

/* 全局访问函数 (复用已验证实现 function.c) */
uint16_t get_holding_reg(uint16_t addr);
int update_holding_reg(uint16_t addr, uint16_t reg);
int update_input_reg(uint16_t addr, uint16_t reg);
uint16_t get_input_reg(size_t index);
int mb_set_do(uint16_t val);

/* 历史数据结构 (复用已验证实现 init.h, 与 RT-Thread 兼容) */
#define DI_TYPE 1
#define AI_TYPE 2
struct his_data {
    uint16_t type;
    uint32_t timestamps;
    union {
        struct { uint16_t di_en_status; uint16_t di_value; } di __packed;
        struct { uint16_t ai_en_status; uint16_t ai_value[AI_NUM]; } ai __packed;
    } __packed;
} __packed;
```

### 4.5 内存规划 (192KB RAM)

| 用途 | 预估占用 | 说明 |
|------|----------|------|
| 线程栈 (应用) | ~15KB | 11 个应用/共享库线程 (见 §4.2) |
| 线程栈 (系统) | ~4KB | idle, log, workqueue 等 |
| 堆内存 (Heap) | 16KB | 动态分配 (文件操作、网络缓冲) |
| 网络缓冲 | ~8KB | Net pkt/buf 池 |
| LittleFS 缓存 | ~8KB | 读/写缓存 + 文件描述符 |
| IO 数据 + 全局变量 | ~4KB | 共享数据、寄存器映射、消息队列 |
| **合计** | **~55KB** | 剩余 ~137KB 可用余量 |

---

## 5. 模块详细设计

### 5.1 IO 采集模块 (`src/modbus/dio.c` + `src/modbus/adc.c` — 复用已验证实现)

> **v3.1 变更**: IO 采集模块不再独立为 `src/io/` 目录，而是复用已验证实现 的 `dio.c` 和 `adc.c` 到 `src/modbus/` 目录，与 Modbus 寄存器管理紧密耦合。

#### 5.1.1 数字输入/输出 (`dio.c/h` — 复用已验证实现)

> **复用来源**: `modbus/dio.c`

- **功能**: 16 路数字输入 (DI1-DI16) + 8 路数字输出 (DO1-DO8) + 8 路 LED 指示
- **DI 引脚**: PD3,PD4,PD5,PD6,PB5,PB6,PB7,PB8,PB9,PB10,PB11,PD2,PB0,PB1,PB3,PB4
- **DO 引脚**: PD7,PD8,PD9,PD10,PD11,PD12,PD13,PD14
- **LED 引脚**: PE8,PE9,PE10,PE11,PE12,PE13,PE14,PE15
- **引脚模式**: DI 为输入 (下拉)，DO/LED 为输出
- **使能控制**: DI 每路独立使能 (16-bit bitmap, `holding_reg[HOLDING_DI_ENABLE_IDX]`)，仅使能的通道参与采样
- **采样方式**: `K_THREAD_DEFINE` 独立线程，周期从 `holding_reg[HOLDING_DI_SAMPLE_MS_IDX]` 读取 (默认 200ms)
- **DO 控制**: `mb_set_do(uint16_t val)` — 写 `holding_reg[HOLDING_DO_IDX]` 时由寄存器回调调用，LED 自动跟随 DO
- **历史记录**: 当 DI 使能且历史保存开启时，采样数据送入 `k_fifo` 异步写入
- **初始化**: `SYS_INIT(dio_init, APPLICATION, 12)`

```c
/* 接口 (复用已验证实现 dio.c) */
int mb_set_do(uint16_t val);  /* 设置 DO 输出 + LED 联动，val bit0-7 对应 DO1-DO8 */
/* DI 采样结果直接写入 input_reg[INPUT_DI_IDX] */
```

#### 5.1.2 模拟输入 (`adc.c/h` — 复用已验证实现)

> **复用来源**: `modbus/adc.c`

- **功能**: 读取 4 路 ADC 模拟输入 (12-bit)
- **引脚**: PC0(IN10), PC1(IN11), PC2(IN12), PC3(IN13) — ADC1
- **信号类型**: AI1-2 电流输入 (4-20mA)，AI3-4 电压输入 (0-10V)
- **使能控制**: 4-bit bitmap (`holding_reg[HOLDING_AI_ENABLE_IDX]`)，仅使能的通道参与采样
- **采样方式**: `K_THREAD_DEFINE(adc_io, ...)` 独立线程，周期从 `holding_reg[HOLDING_AI_SAMPLE_MS_IDX]` 读取 (默认 200ms)
- **工程量转换** (复用已验证实现 `adc.c`):
  - AI0/AI1 (电流): `value = 7.414 * adc_voltage / 10`，单位 0.01mA
  - AI2/AI3 (电压): `value = 3.7037 * adc_voltage / 10`，单位 0.01V
- **采样结果**: 直接写入 `input_reg[INPUT_AI0_IDX..INPUT_AI3_IDX]`

```c
/* 接口 (复用已验证实现 adc.c) */
/* AI 采样结果直接写入 input_reg[]，无需手动调用 */
```

### 5.2 Modbus 模块 (`src/modbus/`)

> **v3.1 变更**: Modbus TCP 从 Zephyr 内置 Server 改为 RAW ADU 模式 + 自定义 TCP Server (复用已验证实现 `tcp.c`)。Modbus RTU 复用已验证实现 `rtu.c`。寄存器管理复用已验证实现 `function.c`。

#### 5.2.1 Modbus TCP Server (`tcp.c/h` — 复用已验证实现)

> **复用来源**: `modbus/tcp.c`

- **模式**: `MODBUS_MODE_RAW` — 使用 Zephyr modbus RAW ADU 接口 (`modbus_raw_get_header` / `modbus_raw_submit_rx` / `modbus_raw_put_header`)
- **TCP Server**: 自定义 socket + `select()` 多路复用 (POSIX API)
- **端口**: 502
- **最大客户端**: 3 (同一时刻仅允许 1 个活跃连接，新连接等待)
- **会话超时**: 30 秒无数据自动断开; 客户端 socket 设 `SO_RCVTIMEO`/`SO_SNDTIMEO` 5s 兜底 (防恶意/慢客户端挂死服务线程)
- **TCP Keepalive**: 客户端 socket 启用 `SO_KEEPALIVE` (`CONFIG_NET_TCP_KEEPALIVE`, 空闲 30s / 探测 5s / 3 次)，主站异常掉线时协议栈自动断开连接
- **网络事件**: `NET_EVENT_IF_DOWN` 时清零 DO 输出 + 设 `link_down` 标志 (拒绝新连接)
- **响应匹配**: 每个请求使用独立 `struct modbus_adu` + 按 MBAP `trans_id` 匹配响应，消除多客户端响应交叉污染 (库为异步处理)

```c
/* 核心流程 (复用已验证实现 tcp.c) */
K_THREAD_DEFINE(mb_tcp, CONFIG_MODBUS_TCP_STACK_SIZE, tcp_poll, ...);

static void tcp_poll(void) {
    /* 1. 注册 NET_EVENT_IF_DOWN/IF_UP 回调 */
    /* 2. init_modbus_server() — modbus_init_server(RAW mode) */
    /* 3. socket() + bind() + listen() on port 502 */
    /* 4. while(1): select() 多路复用 */
    /*    - accept 新连接 (检查连接数/超时/link_down; 设 SO_KEEPALIVE + SO_RCVTIMEO/SO_SNDTIMEO) */
    /*    - recv MBAP header (8B) + modbus_raw_get_header() (校验 proto_id/length) */
    /*    - recv data + 校验 unit_id + modbus_raw_submit_rx() */
    /*    - 等待响应并按 trans_id 匹配 (k_sem_take + 超时) */
    /*    - modbus_raw_put_header() + send response */
}
```

#### 5.2.2 Modbus RTU Slave (`rtu.c/h` — 复用已验证实现)

> **复用来源**: `modbus/rtu.c`

- **角色**: Modbus RTU Slave
- **UART**: USART2 (PA2/PA3)，DE/RE 控制 PA1
- **模式**: `MODBUS_MODE_RTU`
- **波特率**: 从 `holding_reg[HOLDING_RS485_BAUDRATE_IDX]` 读取 (默认 9600)
- **Slave ID**: 从 `holding_reg[HOLDING_SLAVE_ID_IDX]` 读取 (默认 1)
- **初始化**: `SYS_INIT(rtu_init, APPLICATION, 13)`

#### 5.2.3 寄存器管理 (`function.c/h` — 复用已验证实现)

> **复用来源**: `modbus/function.c` + `init.h`

**Input Registers (只读, 6 个)**:

| 地址 | 名称 | 说明 |
|------|------|------|
| 0x00 | INPUT_VER | 固件版本 (高字节=主版本, 低字节=次版本) |
| 0x01 | INPUT_AI0 | AI1 值 (0.01mA) |
| 0x02 | INPUT_AI1 | AI2 值 (0.01mA) |
| 0x03 | INPUT_AI2 | AI3 值 (0.01V) |
| 0x04 | INPUT_AI3 | AI4 值 (0.01V) |
| 0x05 | INPUT_DI | DI1-16 状态 (16-bit bitmap) |

**Holding Registers (读写, 18 个)**:

> **v3.1 变更**: 从 15 个扩展到 21 个，新增 timestamp/reboot/heartbeat 寄存器。寄存器布局与已验证实现一致。
>
> **v3.3 变更**: 移除 heartbeat 寄存器 (0x12-0x14)，Holding 从 21 个缩减为 18 个 (心跳改由 TCP Keepalive 实现，不占寄存器)。
>
> **v3.4 变更**: 寄存器数量与 v3.3 一致 (18 个，`CONFIG_MODBUS_HOLDING_REGISTER_NUMBERS` 默认值由 21 校正为 18)。寄存器枚举重命名以提升可读性：`HOLDING_DI_EN_IDX` → `HOLDING_DI_ENABLE_IDX`、`HOLDING_DI_SI_IDX` → `HOLDING_DI_SAMPLE_MS_IDX`、`HOLDING_HIS_SAVE_IDX` → `HOLDING_HISTORY_ENABLE_IDX`、`HOLDING_CAN_BPS_IDX` → `HOLDING_CAN_BAUDRATE_IDX`、`HOLDING_RS485_BPS_IDX` → `HOLDING_RS485_BAUDRATE_IDX`、`HOLDING_IP_ADDR_1..4_IDX` → `HOLDING_IP_OCTET1..4_IDX`、`HOLDING_TIMESTAMPH/L_IDX` → `HOLDING_TIMESTAMP_HI/LO_IDX`、`HOLDING_CFG_SAVE_IDX` → `HOLDING_CONFIG_SAVE_IDX` (settings 键名字符串不变，已部署设备无影响)。`HOLDING_TIMESTAMP_HI/LO` 读时不再返回数组陈旧值，改为实时 `time(NULL)` 拆分。

| 地址 | 名称 | 默认值 | 说明 |
|------|------|--------|------|
| 0x00 | HOLDING_DO | 0 | DO1-8 输出控制 (8-bit) |
| 0x01 | HOLDING_DI_ENABLE | 0xFFFF | DI1-16 使能 |
| 0x02 | HOLDING_AI_ENABLE | 0x000F | AI1-4 使能 |
| 0x03 | HOLDING_DI_SAMPLE_MS | 200 | DI 采样间隔 (ms) |
| 0x04 | HOLDING_AI_SAMPLE_MS | 200 | AI 采样间隔 (ms) |
| 0x05 | HOLDING_HISTORY_ENABLE | 0 | 历史保存使能 |
| 0x06 | HOLDING_CAN_ID | 0x0111 | CAN ID |
| 0x07 | HOLDING_CAN_BAUDRATE | 10 | CAN 波特率 (x1000, 即 10=10K) |
| 0x08 | HOLDING_RS485_BAUDRATE | 9600 | RS485 波特率 |
| 0x09 | HOLDING_SLAVE_ID | 1 | Modbus RTU Slave ID |
| 0x0A | HOLDING_IP_OCTET1 | 192 | IP 地址段1 |
| 0x0B | HOLDING_IP_OCTET2 | 168 | IP 地址段2 |
| 0x0C | HOLDING_IP_OCTET3 | 12 | IP 地址段3 |
| 0x0D | HOLDING_IP_OCTET4 | 101 | IP 地址段4 |
| 0x0E | HOLDING_TIMESTAMP_HI | 0 | 时间戳高16位 (设置时间) |
| 0x0F | HOLDING_TIMESTAMP_LO | 0 | 时间戳低16位 (设置时间) |
| 0x10 | HOLDING_CONFIG_SAVE | 0 | 参数保存触发 (写非0 → settings_save) |
| 0x11 | HOLDING_REBOOT | 0 | 写1触发系统重启 |

**保持寄存器写入回调** (复用已验证实现 `function.c` holding_reg_wr):

| 寄存器 | 回调动作 | 说明 |
|--------|----------|------|
| 0x00 (DO) | `mb_set_do(reg & 0xff)` | 更新 DO 输出 + LED |
| 0x05 (HISTORY_ENABLE) | `history_enable_write(!!reg)` | 开关历史记录写入 |
| 0x0E-0x0F (TIMESTAMP_HI/LO) | `set_timestamp(combine)` | 设置 RTC 时间 |
| 0x10 (CONFIG_SAVE) | `settings_save()` + 恢复为0 | 全量保存参数到 FCB |
| 0x11 (REBOOT) | `sys_reboot(SYS_REBOOT_COLD)` | 系统重启 |

### 5.3 FTP 服务模块 (`src/ftp_server/` — 直接复用已验证实现)

> **v3.1 变更**: FTP 服务器不再从 RT-Thread 迁移，直接复用已验证实现的 Zephyr 原生实现。

**复用来源**: `ftp_server/` (`ftpd.c`, `ftp_cmds.c`, `ftp_handler.c`, `ftp.h`)

- **端口**: 21
- **最大客户端**: 4 (同一时刻允许 2 个活跃连接)
- **会话超时**: 120 秒
- **认证**: 用户名 `admin` / 密码 `admin`，支持 anonymous (只读)
- **根目录**: LittleFS 挂载点 (自动检测 `lfs1` 或 `flash-disk`)
- **传输模式**: Binary (TYPE I) + ASCII
- **内存管理**: `K_MEM_SLAB_DEFINE_STATIC(ftp_buf_slab, 1024, 2, 4)` — 定长缓冲池替代 malloc/free，`CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=0`
- **多路复用**: `select()` + `FD_SET` (POSIX API)
- **线程**: `K_THREAD_DEFINE(ftp, CONFIG_FTP_STACK_SIZE, ftp_poll, ...)`, 优先级 13

**支持的 FTP 命令** (复用已验证实现 `ftp_cmds.c`):

| 命令 | 说明 |
|------|------|
| USER / PASS | 认证 (admin/admin 或 anonymous) |
| PORT / EPRT | 主动模式数据连接 |
| PASV / EPSV | 被动模式数据连接 |
| PWD / CWD / CDUP | 目录操作 |
| TYPE | 设置传输类型 (I/ASCII) |
| SYST | 系统类型 (返回 "Zephyr RTOS") |
| LIST / NLST | 列出文件 (详细/简略) |
| RETR | 下载文件 (支持 REST 断点续传) |
| STOR | 上传文件 |
| SIZE | 查询文件大小 |
| REST | 设置续传偏移 |
| MKD / RMD | 创建/删除目录 |
| DELE | 删除文件 |
| QUIT | 断开连接 |

### 5.4 双通道固件升级 (共享库)

> **v3.0 核心变更**: 固件升级不再由应用层实现，而是复用 `~/code/app/apps/libs/` 下的共享库。应用仅需注册 app handler 处理业务命令。

#### 5.4.1 UDP 固件升级 (共享库 `udp_fw_upgrade`)

**库内部处理** (应用无需关心):
- `SYS_INIT` 创建配置 socket (端口 8600)，等待 `NET_EVENT_IF_UP` 后绑定
- 拥有独立 RX 线程 (`udp_fw_rx`, 栈 1024B, 优先级 `CONFIG_UDP_FW_RX_PRIORITY`, 默认 8)
- 命令 0x01-0x05 内部处理:
  - `FW_START (0x01)`: 校验 keyhash -> 擦除 Slot1 -> `flash_img_init()`
  - `FW_DATA (0x02)`: `flash_img_buffered_write()` 写入镜像数据 (<=511B/帧)
  - `FW_END (0x03)`: CRC16-CCITT 校验 -> flush -> `boot_request_upgrade()`
  - `GET_VERSION (0x04)`: 返回 `fw_gitver.h` 中的版本字符串
  - `REBOOT (0x05)`: 系统重启
- 跨子网回复路由: `is_same_subnet()` 判断，`CONFIG_UDP_FW_REPLY_BCAST_RESTRICT` 限制

**应用集成** (`src/udp.c`):

```c
/* 注册 app handler (udp_app_init SYS_INIT, priority 80, 在库 priority 90 之前) */
udp_fw_set_app_handler(app_cmd_handler, NULL);

/* 允许 GET_IP 命令跨子网广播回复 (LAN 内广播 0x11 即可发现设备) */
udp_fw_allow_broadcast_cmd(UDP_CMD_GET_IP);

/* App 命令处理 (0x10+) — 直接操作 holding_reg[]
 * 回调签名必须与 udp_fw_app_cmd_cb_t 一致:
 *   bool (*)(uint8_t cmd, const uint8_t *data, size_t len, void *user_data)
 * (在库 RX 线程上下文执行, 不可长时间阻塞) */
static bool app_cmd_handler(uint8_t cmd, const uint8_t *data,
                             size_t len, void *user_data)
{
    switch (cmd) {
    case UDP_CMD_SET_IP:        /* 0x10: 设置 IP (持久化, 需手动重启) */
        /* 校验 ip_addr_valid(a,b,c,d) → 写 holding_reg[HOLDING_IP_OCTET1..4]
         * → holding_reg_save(); 不再自动 set_reboot_status */
        break;
    case UDP_CMD_GET_IP:        /* 0x11: 返回 4B IP (同时承担广播发现) */
        break;
    case UDP_CMD_SET_MODBUS:    /* 0x12: slave_id(1B) + rs485_baud(2B) */
        break;
    case UDP_CMD_GET_MODBUS:    /* 0x13: → slave_id(1B) + rs485_baud(2B) */
        break;
    case UDP_CMD_SET_TIME:      /* 0x14: unix 时间戳(4B 大端) → set_timestamp() 设 RTC */
        break;
    case UDP_CMD_FACTORY_RESET: /* 0x19: 擦 storage_partition + 冷重启 */
        break;
    default:
        return false;  /* 未处理 */
    }
    return true;
}
```

> **v3.4 协议精简**: 移除 SET_SAMPLE/GET_SAMPLE (0x14/0x15)、SET_CAN/GET_CAN (0x16/0x17)、SET_HIS/GET_HIS (0x1A/0x1B)；`DISCOVER` (0x18) 与 `GET_NET` (0x11) 合并为 `GET_IP` (0x11, 仅返回 4B IP, 注册为广播允许命令)；新增 `SET_TIME` (0x14)。最终保留 6 条应用命令：SET_IP / GET_IP / SET_MODBUS / GET_MODBUS / SET_TIME / FACTORY_RESET。SET_IP 不再自动重启，由客户端通过 holding 0x11=1 或重新上电使新 IP 生效。

> **回复发送**: 配置端口回复通过库的 `udp_fw_reply(cmd, data, len)` 在**库 RX 线程内同步** `sendto()` 发送（`udp_fw_upgrade.c` 无内部发送队列）。n2e-gw 的 `udp_tx` 线程 + `k_msgq` 是**数据端口**（nRF24→上位机）的转发路径，与配置端口回复无关。注意:
> - 回复缓冲固定 64B（`udp_fw_reply` 内 `uint8_t buf[64]`），**数据部分 >63B 被截断**；当前最长回复为 GET_MODBUS 的 3B，余量充足。
> - 跨子网广播回复发往 **`CONFIG_UDP_FW_CONFIG_PORT + 1` (8601)** 端口，上位机需监听 8601 接收广播回复。
> - 若确需异步化，可在 handler 内自行入队（参考 n2e-gw 数据端口模式），默认配置端口回复为同步。

#### 5.4.2 CAN 固件升级 (共享库 `can_fw_upgrade`)

**库内部处理** (应用无需关心):
- `SYS_INIT` 初始化 CAN: 设置波特率 (`CONFIG_CAN_FW_UPGRADE_BITRATE`) + 启动 + 全接受滤波器
- 拥有独立 RX 线程 (`can_fw_rx`, 栈 1024B, 优先级 `CONFIG_CAN_FW_UPGRADE_RX_PRIORITY`, 默认 8)
- 帧 ID 0x101-0x105 内部处理:
  - `0x101 (cmd)`: START_UPDATE / CONFIRM / VERSION / REBOOT
  - `0x102 (reply)`: 回复帧
  - `0x103 (data)`: 固件数据 (8B/帧)，每 64B 回复 OFFSET
  - `0x104 (keyhash)`: 密钥哈希 (5 帧 x 7B = 32B SHA-256)
  - `0x105 (version)`: 版本字符串帧
- START_UPDATE: 校验 keyhash -> 擦除 Slot1 -> `flash_img_init()`
- CONFIRM: flush -> 大小校验 -> `boot_request_upgrade()`

**应用集成** (`src/can.c`):

```c
/* 注册 app handler，获取 CAN 设备指针 (main.c 初始化时调用) */
const struct device *can_dev = can_fw_set_app_handler(mod_can_app_rx, NULL);

/* 业务帧处理 (非 0x101-0x105 的帧)
 * 回调签名必须与 can_fw_app_rx_cb_t 一致: bool (*)(struct can_frame *, void *)
 * (frame 为非 const, 与库 typedef 保持一致) */
static bool mod_can_app_rx(struct can_frame *frame, void *user_data)
{
    if (frame->id == get_holding_reg(HOLDING_CAN_ID_IDX)) {
        /* 处理业务 CAN 帧 */
        /* ... */
        return true;   /* 已处理 */
    }
    return false;  /* 未处理，库继续检查是否为固件升级帧 */
}

/* 发送 CAN 业务帧 */
int mod_can_send(uint32_t id, const uint8_t *data, uint8_t len)
{
    struct can_frame frame = { .id = id, .dlc = len };
    memcpy(frame.data, data, len);
    return can_send(can_dev, &frame, K_MSEC(100), NULL, NULL);
}
```

### 5.5 参数持久化模块 (`src/modbus/function.c` — 复用已验证实现)

> **v3.1 变更**: 不再使用独立 `persist.c` 和 `g_params` 结构体。Settings 直接映射 `holding_reg[]` 数组，`"modbus/"` 命名空间。复用已验证实现 `function.c` 中的 settings handler 实现。

#### 5.5.1 Settings Handler (直接映射 holding 寄存器)

> **复用来源**: `modbus/function.c` (mb_handle_set / mb_handle_get / mb_handle_export)

```c
/* 命名空间: "modbus"，直接映射 holding_reg[] 数组 */
SETTINGS_STATIC_HANDLER_DEFINE(modbus, "modbus",
    mb_handle_get,      /* get: 从 holding_reg 读取 */
    mb_handle_set,      /* set: 从 FCB 加载到 holding_reg */
    NULL,               /* commit */
    mb_handle_export    /* export: 导出 holding_reg 到 FCB */
);
```

**Settings 键值映射表**:

| Settings Key | 对应 holding_reg | 大小 | 说明 |
|--------------|-------------------|------|------|
| `modbus/ai/enable` | HOLDING_AI_ENABLE_IDX | 2B | AI 使能 |
| `modbus/ai/time` | HOLDING_AI_SAMPLE_MS_IDX | 2B | AI 采样间隔 |
| `modbus/di/enable` | HOLDING_DI_ENABLE_IDX | 2B | DI 使能 |
| `modbus/di/time` | HOLDING_DI_SAMPLE_MS_IDX | 2B | DI 采样间隔 |
| `modbus/history` | HOLDING_HISTORY_ENABLE_IDX | 2B | 历史保存使能 |
| `modbus/can/id` | HOLDING_CAN_ID_IDX | 2B | CAN ID |
| `modbus/can/bps` | HOLDING_CAN_BAUDRATE_IDX | 2B | CAN 波特率 |
| `modbus/rs485_bps` | HOLDING_RS485_BAUDRATE_IDX | 2B | RS485 波特率 |
| `modbus/slave_id` | HOLDING_SLAVE_ID_IDX | 2B | Modbus Slave ID |
| `modbus/ip` | HOLDING_IP_OCTET1..4 | 8B | IP 地址 (4x uint16_t) |

#### 5.5.2 参数加载与保存

```c
/* 加载: settings_subsys_init() (SYS_INIT, priority 10) 自动调用 mb_handle_set() 逐键加载 */
/* 未找到的键使用 holding_regs[] 中的默认值 */

/* 全量保存: 写 HOLDING_CONFIG_SAVE (0x10) 寄存器触发 */
static int holding_reg_wr(uint16_t addr, uint16_t reg) {
    holding_reg[addr] = reg;
    switch (addr) {
    case HOLDING_CONFIG_SAVE_IDX:
        holding_reg[addr] = 0;  /* 恢复为 0 */
        settings_save();        /* 全量导出到 FCB */
        break;
    /* ... 其他回调 ... */
    }
    return 0;
}

/* mb_handle_export: 导出当前所有 holding_reg 到 FCB */
int mb_handle_export(int (*cb)(const char *name, const void *value, size_t val_len)) {
    cb("modbus/ai/enable", holding_reg + HOLDING_AI_ENABLE_IDX, sizeof(uint16_t));
    cb("modbus/ai/time", holding_reg + HOLDING_AI_SAMPLE_MS_IDX, sizeof(uint16_t));
    cb("modbus/di/enable", holding_reg + HOLDING_DI_ENABLE_IDX, sizeof(uint16_t));
    cb("modbus/di/time", holding_reg + HOLDING_DI_SAMPLE_MS_IDX, sizeof(uint16_t));
    cb("modbus/history", holding_reg + HOLDING_HISTORY_ENABLE_IDX, sizeof(uint16_t));
    cb("modbus/can/id", holding_reg + HOLDING_CAN_ID_IDX, sizeof(uint16_t));
    cb("modbus/can/bps", holding_reg + HOLDING_CAN_BAUDRATE_IDX, sizeof(uint16_t));
    cb("modbus/rs485_bps", holding_reg + HOLDING_RS485_BAUDRATE_IDX, sizeof(uint16_t));
    cb("modbus/slave_id", holding_reg + HOLDING_SLAVE_ID_IDX, sizeof(uint16_t));
    /* IP 合法性检查后才导出 */
    if (get_holding_reg(HOLDING_IP_OCTET4_IDX) != 0xff &&
        get_holding_reg(HOLDING_IP_OCTET4_IDX) != 0 &&
        !IN_RANGE(get_holding_reg(HOLDING_IP_OCTET1_IDX), 224, 239)) {
        cb("modbus/ip", holding_reg + HOLDING_IP_OCTET1_IDX, sizeof(uint16_t) * 4);
    }
    return 0;
}
```

#### 5.5.3 后端初始化

```c
/* SYS_INIT, priority 10 (在 modbus_init priority 11 之前) */
static int main_settings_init(void) {
    return settings_subsys_init();
}
SYS_INIT(main_settings_init, APPLICATION, 10);
```

> **出厂恢复**: 通过 UDP app handler `FACTORY_RESET` 命令擦除 `storage_partition`:
> ```c
> const struct flash_area *fa;
> flash_area_open(FLASH_AREA_ID(storage), &fa);
> flash_area_erase(fa, 0, fa->fa_size);
> flash_area_close(fa);
> settings_subsys_init();  /* 重新初始化，加载默认值 */
> ```

### 5.6 历史记录存储模块 (`src/modbus/history.c` — 复用已验证实现)

> **v3.1 变更**: 历史记录模块从 `src/storage/history_store.c` 改为复用已验证实现 的 `modbus/history.c`。文件轮转策略和异步写入机制与已验证实现一致。

> **复用来源**: `modbus/history.c`

#### 5.6.1 历史数据结构

> 与 RT-Thread `init.h` 中的 `struct his_data` 完全一致，确保 PC 端解析工具兼容。

```c
#define DI_TYPE   1
#define AI_TYPE   2

struct his_data {
    uint16_t type;           /* 1=DI, 2=AI */
    uint32_t timestamps;     /* Unix 时间戳 */
    union {
        struct {
            uint16_t di_en_status;  /* DI 使能 bitmap */
            uint16_t di_value;      /* DI 值 bitmap */
        } di;
        struct {
            uint16_t ai_en_status;  /* AI 使能 bitmap (低4位) */
            uint16_t ai_value[AI_NUM]; /* AI 值数组 */
        } ai;
    };
} __packed;

/* DI 记录大小: 2+4+2+2 = 10 字节 */
/* AI 记录大小: 2+4+2+8 = 16 字节 */
```

#### 5.6.2 文件轮转策略 (复用已验证实现)

- **最大文件数**: 10 个 (10MB / 1MB)
- **单文件最大大小**: 1MB (1024x1024 字节)
- **文件命名**: `data_MMDD_HHMM.raw` (如 `data_0915_1120.raw`)
- **轮转规则**: 文件数达到 10 个时，删除最旧文件 (按 mtime)，创建新文件
- **写入策略**: 通过 `k_fifo` + `k_work` 异步写入 (复用已验证实现 `history.c`)
  - `send_history_data()` 将数据放入 `k_fifo`
  - `his_process_save()` 作为 `k_work` 回调在 workqueue 中执行文件写入
- **历史保存使能**: 由 `holding_reg[HOLDING_HISTORY_ENABLE_IDX]` 控制

### 5.7 系统管理模块 (`src/sys/` + `src/main.c`)

#### 5.7.1 主站连接保活 (TCP Keepalive — 取代应用层心跳)

> **v3.3 变更**: 废除应用层心跳 (原 `heart_poll` 线程 + HEART 寄存器 0x12-0x14 + `heart_event_send`)。Modbus 寄存器层不引入心跳机制，主站连接存活由 Modbus TCP 客户端 socket 的 **TCP Keepalive** 检测。

- **原理**: `accept()` 后对客户端 socket 执行 `setsockopt(SOL_SOCKET, SO_KEEPALIVE, 1)`，Zephyr 网络栈周期性发送 keepalive 探测包；主站异常掉线 (断电/网线拔出/进程崩溃) 时，探测无响应即判定连接断开并自动关闭该 socket (`select` 循环中 `recv` 返回 0/错误)。
- **探测参数** (prj.conf，全局限定):
  ```
  CONFIG_NET_TCP_KEEPALIVE=y
  CONFIG_NET_TCP_KEEPIDLE_DEFAULT=30   # 空闲 30s 后开始探测
  CONFIG_NET_TCP_KEEPINTVL_DEFAULT=5   # 每 5s 一次探测
  CONFIG_NET_TCP_KEEPCNT_DEFAULT=3     # 连续 3 次无响应判定掉线 (~45s)
  ```
- **DO 安全**: 仅由 `NET_EVENT_IF_DOWN` (以太网链路断开) 与 30s 会话超时关闭连接时处理，应用层不再有"超时无请求清零 DO"逻辑。

```c
/* tcp.c — accept 后启用 Keepalive */
int ka = 1;
(void)setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
```

#### 5.7.2 RTC 时间管理 (`time.c` — 复用已验证实现)

> **复用来源**: `time.c`

- **RTC 设备**: STM32 内部 RTC (`DT_NODELABEL(rtc)`)
- **启动同步**: `SYS_INIT(clock_init, POST_KERNEL, 41)` — 从 RTC 读取时间设置系统时钟
- **日志时间戳**: `log_set_timestamp_func(sync_rtc_timestamp_get, 1)` — 日志使用 RTC 时间
- **时间设置**: `set_timestamp(time_t t)` — 通过 Modbus 寄存器 0x0E/0x0F 或 UDP 命令设置

```c
/* 复用已验证实现 time.c */
void set_timestamp(time_t t) {
    struct rtc_time *rt = (struct rtc_time *)gmtime(&t);
    rtc_set_time(rtc_dev, rt);
    struct timespec ts = { .tv_sec = mktime((struct tm *)rt), .tv_nsec = 0 };
    clock_settime(CLOCK_REALTIME, &ts);
}
```

#### 5.7.3 栈溢出保护 (`main.c` — 复用已验证实现)

> **复用来源**: `main.c` (k_sys_fatal_error_handler)

```c
/* 复用已验证实现 main.c */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *pEsf) {
    if (reason == K_ERR_STACK_CHK_FAIL) {
        sys_reboot(SYS_REBOOT_WARM);  /* 栈溢出时自动重启 */
    }
}
```

#### 5.7.4 状态 LED (`main.c` — 复用已验证实现)

> **复用来源**: `main.c` + sysbuild/mcuboot.conf

- **MCUboot LED**: `SB_CONFIG_MCUBOOT_INDICATION_LED=y` (见 §8.3 sysbuild.conf) — 该符号属
  **MCUboot 镜像** Kconfig (`bootloader/mcuboot/boot/zephyr/Kconfig`)，必须经 sysbuild 设置，
  放在应用 prj.conf 无效
- **运行时闪烁**: `mcuboot_led0` 别名，主循环 300ms on / 2700ms off
- **延迟重启**: `set_reboot_status(true)` 设置标志，主循环刷新日志后重启

```c
/* 复用已验证实现 main.c */
int main(void) {
    const struct gpio_dt_spec status_gpios = GPIO_DT_SPEC_GET(DT_ALIAS(mcuboot_led0), gpios);
    gpio_pin_configure_dt(&status_gpios, GPIO_OUTPUT_INACTIVE);
    while (1) {
        gpio_pin_set_dt(&status_gpios, 0);
        k_msleep(2700);
        gpio_pin_set_dt(&status_gpios, 1);
        k_msleep(300);
        if (get_reboot_status()) {
            while (log_process());  /* 刷新日志 */
            k_msleep(1000);
            sys_reboot(SYS_REBOOT_COLD);
        }
    }
}
```

#### 5.7.5 网络断连 DO 安全 (`tcp.c` — 复用已验证实现)

> **复用来源**: `modbus/tcp.c` (net_event_handler)

```c
/* 复用已验证实现 tcp.c */
static void net_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event, struct net_if *iface) {
    if (mgmt_event == NET_EVENT_IF_DOWN) {
        update_holding_reg(HOLDING_DO_IDX, 0);
        mb_set_do(0);           /* 安全断开所有 DO */
        link_down = true;       /* 拒绝新 TCP 连接 */
        LOG_WRN("Network interface link down");
    } else if (mgmt_event == NET_EVENT_IF_UP) {
        link_down = false;
        LOG_INF("Network interface link up");
    }
}
```

#### 5.7.6 硬件看门狗 (`watchdog.c/h`)

- 使用 STM32 独立看门狗 (IWDG)
- 超时时间: 30 秒 (`WDG_TIMEOUT_MS = 30000`)
- 喂狗策略 (v3.4):
  - **main 主循环** (`main.c`) 每次迭代 (~3s) 喂一次狗，保护主循环不卡死
  - **`fs_littlefs.c::littlefs_init`** mkfs 擦整个 15MB SPI NOR 前后各喂一次狗（mkfs 耗时数十秒，超过 30s 看门狗窗口，且此时 `main` 尚未运行，需事件型喂狗防止文件系统半损坏）
  - **DI/AI 采样线程不再喂狗**（v3.3 及之前由 `dio`/`adc_io` 线程周期喂狗）

> **DTS 使能**: `CONFIG_IWDG_STM32` 由设备树 `st,stm32-watchdog` 节点自动拉起
> (`depends on DT_HAS_ST_STM32_WATCHDOG_ENABLED`)，需在 overlay 中使能节点:
> ```dts
> &iwdg {
>     status = "okay";
> };
> ```
> **超时配置**: IWDG 预分频/重载由驱动根据 `watchdog_install_timeout()` 传入的超时 (us)
> 自动换算（见 `wdt_iwdg_stm32.c` 的 `iwdg_stm32_convert_timeout`），30s 超时在
> `watchdog.c` 代码中通过 watchdog API 设置，不写进设备树。

> **语义变化**: v3.3 及之前由 DI/AI 采样线程喂狗，语义是"采样线程冻结则系统复位"。v3.4 改为 main 喂狗后这层保护丢失——采样线程死循环时 main 仍正常喂狗，看门狗不会触发。看门狗现在只保护 main 主循环不卡死。

#### 5.7.7 系统初始化 (`src/main.c` — 参考 已验证实现)

> **复用来源**: `main.c` + `modbus/init.c` (modbus_init)

```c
int main(void)
{
    /* 1. settings_subsys_init() (SYS_INIT, priority 10) — 加载 holding_reg[] */
    /* 2. Flash + LittleFS (SYS_INIT) — 初始化文件系统 */
    /* 3. dio_init (SYS_INIT, priority 12) — 初始化 DI/DO/LED GPIO */
    /* 4. modbus_init (SYS_INIT, priority 11):
     *    - 初始化 holding_reg[] 默认值
     *    - settings_load() 加载持久化参数
     *    - 从 holding_reg 读取 IP 设置静态 IP
     *    - 启动 Modbus RTU
     */
    /* 5. rtu_init (SYS_INIT, priority 13) — Modbus RTU Server */
    /* 6. udp_fw_upgrade SYS_INIT — 创建配置 socket, 等待 NET_EVENT_IF_UP */
    /* 7. can_fw_upgrade SYS_INIT — 初始化 CAN, 启动 RX 线程 */
    /* 8. DI/AI 采样线程 (K_THREAD_DEFINE) — 自动启动 */
    /* 9. Modbus TCP 线程 (K_THREAD_DEFINE) — 自动启动 */
    /* 10. FTP 线程 (K_THREAD_DEFINE) — 自动启动 */
    /* 11. 注册固件升级 app handler */
    udp_fw_set_app_handler(app_cmd_handler, NULL);
    udp_fw_allow_broadcast_cmd(UDP_CMD_GET_IP);
    can_fw_set_app_handler(mod_can_app_rx, NULL);

    /* 13. 网络初始化 */
    derive_mac_from_uid(mac);
    net_mgmt_set_mac(mac);
    /* 静态 IP 已在 modbus_init 中配置 */
    net_if_up(iface);
    k_sem_take(&net_link_sem, K_SECONDS(5));

    /* 14. 主循环 — 状态 LED 闪烁 + 延迟重启 */
    const struct gpio_dt_spec status_gpios = GPIO_DT_SPEC_GET(DT_ALIAS(mcuboot_led0), gpios);
    gpio_pin_configure_dt(&status_gpios, GPIO_OUTPUT_INACTIVE);
    while (1) {
        gpio_pin_set_dt(&status_gpios, 0);
        k_msleep(2700);
        gpio_pin_set_dt(&status_gpios, 1);
        k_msleep(300);
        if (get_reboot_status()) {
            while (log_process());
            k_msleep(1000);
            sys_reboot(SYS_REBOOT_COLD);
        }
    }
    return 0;
}
```

> **与 已验证实现 的差异**: io-edge-hub 额外添加了 MAC 从 UID 生成、CAN 固件升级、UDP 固件升级共享库。其余初始化流程与已验证实现一致。

### 5.8 版本管理与密钥哈希

> **v3.0 新增**: 由 `libs/CMakeLists.txt` 自动生成。

#### 5.8.1 版本字符串 (`fw_gitver.h`)

由 `gen_gitver.py` 在构建时生成:
- 格式: `v<Major>.<Minor>.<Patch>_<6hex>` (如 `v1.0.0_a1b2c3`)
- `<6hex>` 为 git commit hash 的低 6 位十六进制
- `GET_VERSION (0x04)` 命令返回此字符串
- CAN `VERSION` 命令通过 0x105 帧发送此字符串

#### 5.8.2 密钥哈希 (`fw_keyhash.h`)

由 `gen_keyhash.py` 在构建时生成 (当 `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE` 设置时):
- 内容: MCUboot 签名密钥的 SHA-256 哈希 (32 字节)
- 用途: 固件升级时校验固件是否使用正确密钥签名
- UDP: `FW_START` 命令可选携带 32B keyhash，库校验与 `fw_keyhash.h` 一致
- CAN: `0x104` 帧发送 5 x 7B = 35B (有效 32B)，库在 `START_UPDATE` 时校验

---

## 6. 通信协议设计

### 6.1 UDP 固件升级协议 (共享库 `udp_fw_upgrade`)

**端口**: 8600 (`CONFIG_UDP_FW_CONFIG_PORT`, 可配置)
**传输**: UDP

> **v3.0 变更**: 替换 v2.0 的自定义协议 (端口 5000, magic 0xAE)，使用共享库标准协议。端口 8600 同时承载固件升级命令 (0x01-0x05) 和应用业务命令 (0x10+)。

#### 6.1.1 固件升级命令 (库内部处理)

| Cmd | 名称 | 方向 | Payload | 说明 |
|-----|------|------|---------|------|
| 0x01 | FW_START | 上位机->设备 | image_size(4B LE) + keyhash(32B, 可选) | 开始升级，校验密钥哈希 |
| 0x02 | FW_DATA | 上位机->设备 | data(<=511B) | 固件数据块 |
| 0x03 | FW_END | 上位机->设备 | test(1B) + crc16(2B LE) | 传输完成，CRC16-CCITT 校验 |
| 0x04 | GET_VERSION | 上位机->设备 | (无) | 查询版本字符串 |
| 0x05 | REBOOT | 上位机->设备 | (无) | 系统重启 |

**FW_START 流程**:
1. 设备接收 image_size 和 keyhash (32B)
2. 校验 keyhash 与编译时 `fw_keyhash.h` 中的哈希一致
3. 擦除 Slot1 (外部 Flash Secondary Slot)
4. `flash_img_init()` 初始化镜像写入
5. 回复 ACK

**FW_END 流程**:
1. 设备接收 test 标志和 CRC16 (2B LE)
2. `flash_img_buffered_write()` flush
3. CRC16-CCITT 校验全镜像
4. 校验通过 -> `boot_request_upgrade(BOOT_UPGRADE_TEST)`
5. 回复 ACK，系统重启

**回复路由**:
- 同子网: 单播回复到发送方源端口
- 跨子网: 定向广播回复到 **`CONFIG_UDP_FW_CONFIG_PORT + 1` (8601)** 端口
  (受 `CONFIG_UDP_FW_REPLY_BCAST_RESTRICT` 限制，仅允许的命令; 上位机需监听 8601)
- **回复长度限制**: 库回复缓冲固定 64B（`udp_fw_reply` 内 `uint8_t buf[64]`），数据部分
  超过 63B 被截断，命令回复需精简 (<63B)
- **回复时机**: `udp_fw_reply()` 在库 RX 线程内同步发送，无内部发送队列

#### 6.1.2 应用业务命令 (app handler, 0x10+)

| Cmd | 名称 | 方向 | Payload | 说明 |
|-----|------|------|---------|------|
| 0x10 | SET_IP | 上位机->设备 | ip(4B) | 设置静态 IP 地址 (持久化, 不自动重启) |
| 0x11 | GET_IP | 双向 | ip(4B) | 返回当前 IP (允许广播发现) |
| 0x12 | SET_MODBUS | 上位机->设备 | slave_id(1B) + rs485_baud(2B) | 设置 Modbus 参数 |
| 0x13 | GET_MODBUS | 双向 | 同 SET_MODBUS | 查询 Modbus 参数 |
| 0x14 | SET_TIME | 上位机->设备 | unix 时间戳(4B 大端) | 设置 RTC 时间 |
| 0x19 | FACTORY_RESET | 上位机->设备 | (无) | 恢复出厂设置 |

> **GET_IP 命令**: 允许跨子网广播回复 (`udp_fw_allow_broadcast_cmd(0x11)`)，承担 LAN 内设备发现职责。回复固定 4B IP，远小于 63B 缓冲限制。

> **FACTORY_RESET 命令**: 擦除整个 `storage_partition`，重新初始化 settings，恢复所有参数为默认值。

### 6.2 CAN 固件升级协议 (共享库 `can_fw_upgrade`)

> **v3.0 新增**: CAN 通道固件升级，与 UDP 固件升级并行支持。

#### 6.2.1 帧定义

| 帧 ID | 方向 | DLC | 说明 |
|-------|------|-----|------|
| 0x101 | 上位机->设备 | 8B | 命令帧 (START_UPDATE / CONFIRM / VERSION / REBOOT) |
| 0x102 | 设备->上位机 | 8B | 回复帧 (ACK / OFFSET / 状态) |
| 0x103 | 上位机->设备 | 8B | 固件数据帧 (8B/帧) |
| 0x104 | 上位机->设备 | 8B | 密钥哈希帧 ([seq 1B][chunk 7B] x5 = 35B) |
| 0x105 | 设备->上位机 | 8B | 版本字符串帧 |

#### 6.2.2 命令流程

| 命令 | 帧流 | 说明 |
|------|------|------|
| Keyhash 发送 | 0x104 x5 -> | 5 帧 x [seq(1B) + chunk(7B)] = 35B (有效 32B SHA-256) |
| START_UPDATE | -> 0x101(cmd=START) -> | 库校验 keyhash -> 擦除 Slot1 -> flash_img_init |
| | <- 0x102(ACK) | |
| 固件数据 | -> 0x103 xN -> | 每帧 8B，flash_img_buffered_write |
| | <- 0x102(OFFSET) <- | 每 64B (8帧) 回复一次当前 OFFSET |
| CONFIRM | -> 0x101(cmd=CONFIRM) -> | flush -> 大小校验 -> boot_request_upgrade |
| | <- 0x102(RESULT) | |
| VERSION | -> 0x101(cmd=VERSION) -> | |
| | <- 0x105 xN <- | 版本字符串分帧发送 |
| REBOOT | -> 0x101(cmd=REBOOT) -> | 系统重启 |

> **CAN 波特率**: 由 `CONFIG_CAN_FW_UPGRADE_BITRATE` 设置 (默认 250000)。CAN 业务通信使用相同波特率。

### 6.3 Modbus 寄存器映射

> 与 RT-Thread 实现完全一致，确保上位机软件兼容。

#### Input Registers (只读, 6 个, 地址 30001-30006)

| 地址 | 寄存器 | 说明 |
|------|--------|------|
| 30001 | 0x00 | 固件版本 (major<<8 \| minor) |
| 30002 | 0x01 | AI1 值 (电流, 0.01mA) |
| 30003 | 0x02 | AI2 值 (电流, 0.01mA) |
| 30004 | 0x03 | AI3 值 (电压, 0.01V) |
| 30005 | 0x04 | AI4 值 (电压, 0.01V) |
| 30006 | 0x05 | DI1-16 状态 (16-bit bitmap) |

#### Holding Registers (读写, 18 个, 地址 40001-40018)

> **v3.1 变更**: 从 15 个扩展到 21 个，新增 timestamp/reboot/heartbeat 寄存器。布局与已验证实现一致。
>
> **v3.3 变更**: 移除 heartbeat 寄存器 (0x12-0x14)，Holding 从 21 个缩减为 18 个。

| 地址 | 寄存器 | 默认值 | 说明 |
|------|--------|--------|------|
| 40001 | 0x00 | 0 | DO1-8 输出控制 |
| 40002 | 0x01 | 0xFFFF | DI1-16 使能 |
| 40003 | 0x02 | 0x000F | AI1-4 使能 |
| 40004 | 0x03 | 200 | DI 采样间隔 (ms) |
| 40005 | 0x04 | 200 | AI 采样间隔 (ms) |
| 40006 | 0x05 | 0 | 历史保存使能 |
| 40007 | 0x06 | 0x0111 | CAN ID |
| 40008 | 0x07 | 10 | CAN 波特率 (x1000, 10=10K) |
| 40009 | 0x08 | 9600 | RS485 波特率 |
| 40010 | 0x09 | 1 | Modbus RTU Slave ID |
| 40011 | 0x0A | 192 | IP 地址段1 |
| 40012 | 0x0B | 168 | IP 地址段2 |
| 40013 | 0x0C | 12 | IP 地址段3 |
| 40014 | 0x0D | 101 | IP 地址段4 |
| 40015 | 0x0E | 0 | 时间戳高16位 (写后设置 RTC) |
| 40016 | 0x0F | 0 | 时间戳低16位 |
| 40017 | 0x10 | 0 | 参数保存触发 (写非0 → settings_save) |
| 40018 | 0x11 | 0 | 写1触发系统重启 |

---

## 7. 项目目录结构

> **v3.0 变更**: 项目作为 `iot-zephyr-app` 仓库 (`~/code/app/apps/`) 下的一个应用，与 `n2e-gw` 和 `angle-handler` 并列，共享 `libs/` 下的固件升级库。

```
~/code/app/apps/                             # iot-zephyr-app 仓库根目录
├── CMakeLists.txt                           # 顶层 CMake
├── CLAUDE.md                                # 仓库规范
├── west.yml                                 # West 依赖管理清单
│
├── libs/                                    # 共享库 (已存在)
│   ├── CMakeLists.txt                       # 生成 fw_gitver.h / fw_keyhash.h
│   ├── gen_gitver.py                        # git commit hash -> fw_gitver.h
│   ├── gen_keyhash.py                       # MCUboot 密钥 SHA-256 -> fw_keyhash.h
│   ├── udp_fw_upgrade/                      # UDP 固件升级库
│   │   ├── CMakeLists.txt
│   │   ├── Kconfig
│   │   ├── udp_fw_upgrade.h
│   │   └── udp_fw_upgrade.c
│   └── can_fw_upgrade/                      # CAN 固件升级库
│       ├── CMakeLists.txt
│       ├── Kconfig
│       ├── can_fw_upgrade.h
│       └── can_fw_upgrade.c
│
└── applications/
    ├── n2e-gw/                              # 已有: W5500 + UDP 固件升级
    ├── angle-handler/                       # 已有: CAN 固件升级
    │
    └── io-edge-hub/                         # <-- 本项目
        ├── CMakeLists.txt                   # 应用 CMake (链接 libs/)
        ├── prj.conf                         # 应用 Kconfig 配置 (调试模式)
        ├── prj_release.conf                 # 发布模式 Kconfig 配置覆盖
        ├── sysbuild.conf                    # Sysbuild 配置 (MCUboot SWAP_SCRATCH)
        ├── VERSION                          # 版本号文件
        │
        ├── boards/
        │   ├── io_edge_hub.overlay          # 设备树覆盖 (Flash 分区, W5500, GPIO)
        │   └── io_edge_hub.pem              # MCUboot 签名密钥 (RSA-2048)
        │
        ├── include/
        │   └── init.h                       # 公共头文件 (寄存器枚举, his_data, 函数声明)
        │
        └── src/
            ├── main.c                       # 主入口: 状态LED + 延迟重启 + 栈溢出处理 (复用已验证实现)
            ├── udp.c                        # UDP app handler (0x10+ 业务命令)
            ├── udp.h
            ├── can.c                        # CAN app handler (业务帧收发)
            ├── can.h
            │
            ├── modbus/                      # Modbus 通信模块 (复用已验证实现)
            │   ├── CMakeLists.txt
            │   ├── init.c                   # modbus_init: settings_subsys_init + load (恢复 holding_reg + 历史使能)
            │   ├── init.h                   # 寄存器枚举, his_data, 函数声明
            │   ├── function.c               # 寄存器读写回调 + Settings handler (modbus/ 命名空间)
            │   ├── tcp.c                    # Modbus TCP RAW ADU Server (select() 多路复用)
            │   ├── rtu.c                    # Modbus RTU Slave (RS485)
            │   ├── adc.c                    # 4 路 AI 驱动 (adc_dt_spec + 工程量转换)
            │   ├── dio.c                    # 16 路 DI + 8 路 DO + LED (GPIO + K_THREAD)
            │   └── history.c               # 历史记录 (k_work + k_fifo + 文件轮转)
            │
            ├── ftp_server/                  # FTP 服务器 (复用已验证实现)
            │   ├── CMakeLists.txt
            │   ├── ftp.h
            │   ├── ftpd.c                   # FTP 主线程 (select() + 会话管理)
            │   ├── ftp_cmds.c               # FTP 命令实现 (PORT/PASV/LIST/RETR/STOR/...)
            │   └── ftp_handler.c            # FTP 会话状态机 (USER→PASS→OK)
            │
            ├── storage/                     # 存储模块
            │   ├── fs_littlefs.c            # LittleFS 挂载 (复用已验证实现 snippet)
            │   └── fs_littlefs.h
            │
            └── sys/                         # 系统管理模块
                ├── time.c                   # RTC 时间管理 (复用已验证实现)
                ├── watchdog.c               # IWDG 硬件看门狗
                └── watchdog.h
```

> **与已有项目的差异**: io-edge-hub 同时启用 `CONFIG_UDP_FW_UPGRADE=y` 和 `CONFIG_CAN_FW_UPGRADE=y`，是仓库中首个同时使用双通道固件升级的应用。UDP app handler (`udp.c`) 参考 `n2e-gw/src/udp.c`，CAN app handler (`can.c`) 参考 `angle-handler/src/can.c`。

---

## 8. Kconfig 配置参考

### 8.1 应用 prj.conf

```kconfig
# ==================== 基础配置 ====================
CONFIG_MAIN_STACK_SIZE=2048
CONFIG_HEAP_MEM_POOL_SIZE=16384
CONFIG_MULTITHREADING=y
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048
CONFIG_POSIX_API=y                           # 复用已验证实现: select/FD_SET/POSIX socket

# libc: FTP 使用 k_mem_slab 替代 malloc，关闭 heap arena (复用已验证实现)
CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=0

# ==================== 硬件驱动 ====================
CONFIG_GPIO=y
CONFIG_ADC=y
CONFIG_ADC_STM32=y
CONFIG_SPI=y
CONFIG_SPI_STM32=y
CONFIG_FLASH=y
CONFIG_FLASH_JESD216=y
CONFIG_SPI_NOR=y
CONFIG_CAN=y
CONFIG_CAN_STM32_BXCAN=y

# ==================== 文件系统 ====================
CONFIG_FILE_SYSTEM=y
CONFIG_FILE_SYSTEM_LITTLEFS=y
CONFIG_FS_LITTLEFS_NUM_FILES=8
CONFIG_FS_LITTLEFS_NUM_DIRS=4
CONFIG_FS_LITTLEFS_BLK_DEV=y
# LittleFS 挂载/读写使用内核堆 (k_malloc), 由基础配置的 CONFIG_HEAP_MEM_POOL_SIZE=16384 提供

# ==================== 参数持久化 (Zephyr settings + FCB, 直接映射 holding_reg) ====================
CONFIG_SETTINGS=y
CONFIG_SETTINGS_RUNTIME=y
CONFIG_FCB=y

# ==================== 网络 (静态 IP, 无 DHCP, 复用已验证实现) ====================
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_IPV6=n
CONFIG_NET_TCP=y
CONFIG_NET_UDP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_SOCKETS_POSIX_NAMES=y
CONFIG_NET_MAX_CONN=15
CONFIG_NET_MAX_CONTEXTS=15
CONFIG_ZVFS_POLL_MAX=20
CONFIG_NET_BUF_DATA_SIZE=512
CONFIG_NET_BUF_RX_COUNT=8
CONFIG_NET_BUF_TX_COUNT=8
CONFIG_NET_CONNECTION_MANAGER=y
CONFIG_NET_MGMT=y              /* 网络管理子系统; NET_MGMT_EVENT 依赖它 (非 select, 必须显式开) */
CONFIG_NET_MGMT_EVENT=y        /* 网络事件 (NET_EVENT_IF_UP/DOWN); udp_fw_upgrade 库 depends 它 */
CONFIG_NET_L2_ETHERNET=y
CONFIG_NET_ARP=y
# 无多播需求, 不启用 CONFIG_NET_IPV4_IGMP

# W5500 以太网驱动 (SPI2): 由设备树 wiznet,w5500 节点自动拉起 (见 §9.3),
# 无需显式 CONFIG_ETH_W5500; 注意 v4.4.0 无 CONFIG_ETH_W5500_SPI 符号

# 静态 IP: 禁用 DHCP, 禁用自动启动 (允许设置 MAC)
CONFIG_NET_DHCPV4=n
CONFIG_ETH_NET_IF_NO_AUTO_START=y
CONFIG_NET_L2_ETHERNET_MGMT=y

# ==================== Modbus (RAW ADU 模式, 复用已验证实现) ====================
CONFIG_MODBUS=y
CONFIG_MODBUS_ROLE_SERVER=y
CONFIG_MODBUS_RAW_ADU=y
CONFIG_MODBUS_NUMOF_RAW_ADU=1

# ==================== TCP Keepalive (检测主站连接存活, 取代应用层心跳) ====================
CONFIG_NET_TCP_KEEPALIVE=y
CONFIG_NET_TCP_KEEPIDLE_DEFAULT=30
CONFIG_NET_TCP_KEEPINTVL_DEFAULT=5
CONFIG_NET_TCP_KEEPCNT_DEFAULT=3

# ==================== 固件升级 (共享库) ====================
# UDP 固件升级 (端口 8600)
CONFIG_UDP_FW_UPGRADE=y
CONFIG_UDP_FW_CONFIG_PORT=8600
CONFIG_UDP_FW_RX_STACK_SIZE=1024
CONFIG_UDP_FW_RX_PRIORITY=8                # 库默认; 如要更高优先级调小 (见 §4.2)
CONFIG_UDP_FW_UPGRADE_MAX_HANDLERS=4
CONFIG_UDP_FW_REPLY_BCAST_RESTRICT=y

# CAN 固件升级 (帧 ID 0x101-0x105)
CONFIG_CAN_FW_UPGRADE=y
CONFIG_CAN_FW_UPGRADE_BITRATE=250000
CONFIG_CAN_FW_UPGRADE_RX_STACK_SIZE=1024
CONFIG_CAN_FW_UPGRADE_RX_PRIORITY=8        # 库默认; 如要更高优先级调小 (见 §4.2)
CONFIG_CAN_FW_UPGRADE_MAX_HANDLERS=4

# ==================== MCUboot ====================
CONFIG_BOOTLOADER_MCUBOOT=y
CONFIG_MCUBOOT_IMG_MANAGER=y
CONFIG_IMG_MANAGER=y
# MCUboot LED 指示属 MCUboot 镜像 Kconfig, 在 sysbuild.conf 设 SB_CONFIG_MCUBOOT_INDICATION_LED=y (见 §8.3)

# ==================== 时间同步 (复用已验证实现 time.c) ====================
CONFIG_RTC=y
CONFIG_RTC_STM32=y

# ==================== 看门狗 ====================
CONFIG_WATCHDOG=y
CONFIG_IWDG_STM32=y

# ==================== 日志 ====================
CONFIG_LOG=y
CONFIG_LOG_BUFFER_SIZE=1024
CONFIG_LOG_OUTPUT_FORMAT_DATE_TIMESTAMP=y
CONFIG_LOG_DEFAULT_LEVEL=3

# ==================== 调试 ====================
CONFIG_THREAD_NAME=y
CONFIG_THREAD_MONITOR=y
CONFIG_THREAD_STACK_INFO=y
CONFIG_SHELL=y
CONFIG_SHELL_BACKEND_SERIAL=y
CONFIG_SHELL_VT100_COLORS=y
CONFIG_SHELL_TAB=y
CONFIG_NET_SHELL=y
CONFIG_FLASH_SHELL=y
CONFIG_FLASH_MAP_SHELL=y
CONFIG_FILE_SYSTEM_SHELL=y
CONFIG_RTC_SHELL=y
CONFIG_SETTINGS_SHELL=y
CONFIG_FAULT_DUMP=2

# ==================== JSON ====================
# (v3.1 无 JSON: UDP 配置已改为 udp_fw_upgrade app handler, 不启用 CONFIG_JSON_LIBRARY)
```

### 8.2 发布模式 prj_release.conf

```kconfig
CONFIG_LOG=n
CONFIG_SHELL=n
CONFIG_THREAD_MONITOR=n
CONFIG_FAULT_DUMP=1

# libc: 保持 COMMON_LIBC + k_mem_slab (复用已验证实现, 不使用 newlib)
CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=0

CONFIG_NET_PKT_RX_COUNT=4
CONFIG_NET_PKT_TX_COUNT=4
CONFIG_NET_BUF_RX_COUNT=8
CONFIG_NET_BUF_TX_COUNT=8
```

### 8.3 sysbuild.conf (MCUboot 配置)

> **v3.0 变更**: MCUboot 模式从 Overwrite-only 改为 SWAP_SCRATCH，与 n2e-gw / angle-handler 一致。

```kconfig
# MCUboot 升级模式: SWAP_SCRATCH (支持回滚)
SB_CONFIG_MCUBOOT_MODE_SWAP_SCRATCH=y

# 签名密钥 (每个应用独立)
SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="${APP_DIR}/boards/${BOARD}.pem"

# 签名类型: RSA-2048 (与仓库 n2e-gw/angle-handler 一致; fw_keyhash/gen_keyhash.py 仅支持 RSA)
SB_CONFIG_BOOT_SIGNATURE_TYPE_RSA=y
# RSA 长度默认 2048 (MCUboot BOOT_SIGNATURE_TYPE_RSA_LEN default 2048)

# MCUboot 引导阶段 LED 指示 (属 MCUboot 镜像 Kconfig, 必须经 sysbuild 设置)
SB_CONFIG_MCUBOOT_INDICATION_LED=y
```

---

## 9. 设备树配置参考

> **v3.1 变更**: GPIO/ADC/USART2 引脚分配参考已验证实现，确保引脚定义与已验证实现一致。新增时钟配置 (HSE 13MHz 外部晶振)。

### 9.1 时钟配置 (HSE 13MHz 外部晶振)

> **v3.1 新增**: 系统时钟使用 HSE 外部 13MHz 晶振 (`PH0`=OSC_IN / `PH1`=OSC_OUT)，PLL 倍频至 168MHz。HSE 精度远高于 HSI (内部 RC)，有利于 ADC 采样精度和通信稳定性。

```dts
/* 板级 DTS: 时钟配置 */
&clk_hse {
    clock-frequency = <DT_FREQ_M(13)>;   /* 外部 13MHz 晶振 */
    status = "okay";
};

&pll {
    div-m = <13>;     /* VCO 输入 = 13MHz / 13 = 1MHz */
    mul-n = <336>;    /* VCO 输出 = 1MHz * 336 = 336MHz */
    div-p = <2>;      /* SYSCLK = 336MHz / 2 = 168MHz */
    div-q = <7>;      /* USB OTG = 336MHz / 7 = 48MHz */
    clocks = <&clk_hse>;
    status = "okay";
};

&rcc {
    clocks = <&pll>;
    clock-frequency = <DT_FREQ_M(168)>;  /* 168MHz SYSCLK */
    ahb-prescaler = <1>;                 /* HCLK = 168MHz */
    apb1-prescaler = <4>;                /* PCLK1 = 42MHz */
    apb2-prescaler = <2>;                /* PCLK2 = 84MHz */
};
```

> **注意**: HSE 外部晶振使用 `PH0`(OSC_IN)/`PH1`(OSC_OUT)，与 PD0/PD1 无关。PD0/PD1 专用于 W5500 复位/中断控制（见 §9.3）。

### 9.2 SPI1 — W25Q128 SPI Flash (含分区)

> **v3.0 变更**: 增加 Scratch 分区 (SWAP_SCRATCH 模式必需)，Settings 分区缩减至 64KB (FCB 后端)。

```dts
/* boards/io_edge_hub.overlay (片段) */

&spi1 {
    status = "okay";
    pinctrl-0 = <&spi1_sck_pa5 &spi1_miso_pa6 &spi1_mosi_pa7>;
    pinctrl-names = "default";
    cs-gpios = <&gpioa 4 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;

    w25q128: spi-flash@0 {
        compatible = "jedec,spi-nor";
        reg = <0>;
        spi-max-frequency = <42000000>;
        jedec-id = [ef 40 18];  /* Winbond W25Q128 */
        size = <0x800000>;      /* 16MB = 8M * 8bits */
        status = "okay";

        partitions {
            compatible = "fixed-partitions";
            #address-cells = <1>;
            #size-cells = <1>;

            /* MCUboot Secondary Slot */
            slot1_partition: partition@0 {
                label = "slot1";
                reg = <0x000000 0x070000>;  /* 448KB */
            };

            /* MCUboot Scratch - SWAP_SCRATCH 模式必需 */
            scratch_partition: partition@70000 {
                label = "scratch";
                reg = <0x070000 0x070000>;  /* 448KB */
            };

            /* Settings Storage (FCB) - Zephyr settings 后端 */
            storage_partition: partition@e0000 {
                label = "storage";
                reg = <0x0e0000 0x010000>;  /* 64KB */
            };

            /* LittleFS 历史记录分区 */
            littlefs_partition: partition@f0000 {
                label = "littlefs";
                reg = <0x0f0000 0xf10000>;  /* ~15MB */
            };
        };
    };
};
```

### 9.3 SPI2 — W5500 以太网

> **v3.1 变更**: W5500 的 RST/INT 使用 PD0/PD1（与 HSE 无关，HSE 走 PH0/PH1）。MAC 地址运行时从 UID 设置 (不从设备树读取)。

```dts
&spi2 {
    status = "okay";
    pinctrl-0 = <&spi2_sck_pb13 &spi2_miso_pb14 &spi2_mosi_pb15>;
    pinctrl-names = "default";
    cs-gpios = <&gpiob 12 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;

    w5500: ethernet@0 {
        compatible = "wiznet,w5500";
        reg = <0>;
        spi-max-frequency = <21000000>;
        reset-gpios = <&gpiod 0 GPIO_ACTIVE_LOW>;   /* PD0: W5500 硬件复位 */
        int-gpios = <&gpiod 1 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;  /* PD1: W5500 中断输出 */
        status = "okay";
    };
};

/* W5500 LED 指示灯 */
/ {
    eth_led: eth-led {
        compatible = "gpio-leds";
        eth_led_pin: eth_led_pin {
            gpios = <&gpioe 7 0>;
            label = "ETH_LED";
        };
    };
};
```

### 9.4 chosen 节点 (Settings 分区绑定)

> **v3.0 新增**: 将 `storage_partition` 绑定为 Zephyr settings 后端分区。

```dts
/ {
    chosen {
        zephyr,flash = &flash0;
        zephyr,code-partition = &slot0_partition;
        zephyr,settings-partition = &storage_partition;  /* FCB 后端 */
    };
};
```

### 9.5 内部 Flash 分区

```dts
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        boot_partition: partition@0 {
            label = "mcuboot";
            reg = <0x00000000 0x00010000>;  /* 64KB */
        };

        slot0_partition: partition@10000 {
            label = "slot0";
            reg = <0x00010000 0x00070000>;  /* 448KB */
        };
    };
};
```

### 9.6 ADC1 — 4 通道模拟输入

> **v3.3 变更**: 通道号与工程量转换系数均配置到设备树 (`/zephyr,user` 的 `io-channels` 引用 `&adc1` 下 `channel@` 子节点；系数放 `ai-coeffs`，与 `io-channels` 顺序一一对应)。代码用 `ADC_DT_SPEC_GET_BY_IDX` / `adc_channel_setup_dt` / `adc_read_dt` 读取，不再硬编码 `ai_channel_id[]` / `ai_coeff[]`。

```dts
/ {
    zephyr,user {
        io-channels = <&adc1 10>, <&adc1 11>, <&adc1 12>, <&adc1 13>;  /* IN10-13 */
        ai-coeffs = <7414>, <7414>, <3704>, <3704>;  /* 电流 7.414 / 电压 3.7037, x1e4 */
    };
};

&adc1 {
    status = "okay";
    pinctrl-0 = <&adc1_in10_pc0 &adc1_in11_pc1
                 &adc1_in12_pc2 &adc1_in13_pc3>;
    pinctrl-names = "default";
    st,adc-clock-source = "SYNC";
    st,adc-prescaler = <2>;
    #address-cells = <1>;
    #size-cells = <0>;

    channel@a { reg = <0xa>; zephyr,gain = "ADC_GAIN_1"; zephyr,reference = "ADC_REF_INTERNAL"; zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>; zephyr,resolution = <12>; };
    channel@b { reg = <0xb>; zephyr,gain = "ADC_GAIN_1"; zephyr,reference = "ADC_REF_INTERNAL"; zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>; zephyr,resolution = <12>; };
    channel@c { reg = <0xc>; zephyr,gain = "ADC_GAIN_1"; zephyr,reference = "ADC_REF_INTERNAL"; zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>; zephyr,resolution = <12>; };
    channel@d { reg = <0xd>; zephyr,gain = "ADC_GAIN_1"; zephyr,reference = "ADC_REF_INTERNAL"; zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>; zephyr,resolution = <12>; };
};
```

> 注意: 自定义系数属性 `st,coeff` 若放在 `&adc1` 子节点会触发 Zephyr binding 校验 (未在 `st,stm32f4-adc.yaml` 声明)，因此系数放 `/zephyr,user` 的 `ai-coeffs`（该节点无 binding，属性自由）。

### 9.7 USART2 — Modbus RTU (RS485)

```dts
&usart2 {
    status = "okay";
    pinctrl-0 = <&usart2_tx_pa2 &usart2_rx_pa3>;
    pinctrl-names = "default";
    current-speed = <9600>;

    /* RS485 DE/RE 控制引脚 */
    de-gpios = <&gpioa 1 GPIO_ACTIVE_HIGH>;
};
```

### 9.8 CAN1 — CAN 总线 + CAN 固件升级

```dts
&can1 {
    status = "okay";
    pinctrl-0 = <&can1_rx_pa11 &can1_tx_pa12>;
    pinctrl-names = "default";
    bus-speed = <250000>;
    sample-point = <875>;
};
```

> CAN1 设备树 `bus-speed` 应与 `CONFIG_CAN_FW_UPGRADE_BITRATE` 一致 (默认 250000)。`can_fw_upgrade` 库在 SYS_INIT 时会设置波特率。

### 9.9 数字输入/输出 GPIO 定义

```dts
/* DI 引脚定义 (16 路) */
/ {
    di_gpios: di-gpios {
        compatible = "gpio-leds";
        di1: di1 { gpios = <&gpiod 3 0>; };
        di2: di2 { gpios = <&gpiod 4 0>; };
        di3: di3 { gpios = <&gpiod 5 0>; };
        di4: di4 { gpios = <&gpiod 6 0>; };
        di5: di5 { gpios = <&gpiob 5 0>; };
        di6: di6 { gpios = <&gpiob 6 0>; };
        di7: di7 { gpios = <&gpiob 7 0>; };
        di8: di8 { gpios = <&gpiob 8 0>; };
        di9: di9 { gpios = <&gpiob 9 0>; };
        di10: di10 { gpios = <&gpiob 10 0>; };
        di11: di11 { gpios = <&gpiob 11 0>; };
        di12: di12 { gpios = <&gpiod 2 0>; };
        di13: di13 { gpios = <&gpiob 0 0>; };
        di14: di14 { gpios = <&gpiob 1 0>; };
        di15: di15 { gpios = <&gpiob 3 0>; };
        di16: di16 { gpios = <&gpiob 4 0>; };
    };

    /* DO 引脚定义 (8 路) */
    do_gpios: do-gpios {
        compatible = "gpio-leds";
        do1: do1 { gpios = <&gpiod 7 0>; };
        do2: do2 { gpios = <&gpiod 8 0>; };
        do3: do3 { gpios = <&gpiod 9 0>; };
        do4: do4 { gpios = <&gpiod 10 0>; };
        do5: do5 { gpios = <&gpiod 11 0>; };
        do6: do6 { gpios = <&gpiod 12 0>; };
        do7: do7 { gpios = <&gpiod 13 0>; };
        do8: do8 { gpios = <&gpiod 14 0>; };
    };

    /* LED 引脚定义 (8 路, 跟随 DO) */
    led_gpios: led-gpios {
        compatible = "gpio-leds";
        led1: led1 { gpios = <&gpioe 8 0>; };
        led2: led2 { gpios = <&gpioe 9 0>; };
        led3: led3 { gpios = <&gpioe 10 0>; };
        led4: led4 { gpios = <&gpioe 11 0>; };
        led5: led5 { gpios = <&gpioe 12 0>; };
        led6: led6 { gpios = <&gpioe 13 0>; };
        led7: led7 { gpios = <&gpioe 14 0>; };
        led8: led8 { gpios = <&gpioe 15 0>; };
    };
};
```

---

## 10. 构建与部署

### 10.1 依赖项

| 工具 | 版本要求 | 说明 |
|------|----------|------|
| Zephyr SDK | >= 0.16.x | 包含 ARM GCC 工具链 |
| West | 最新 | Zephyr 构建系统管理工具 |
| Python | >= 3.10 | 脚本和工具依赖 |
| CMake | >= 3.20 | 构建系统 |
| imgtool | 最新 | MCUboot 镜像签名工具 (sysbuild 自动调用) |
| pyOCD / probe-rs | 最新 | 烧录调试 |

### 10.2 构建步骤 (sysbuild 模式)

> **v3.0 变更**: 使用 sysbuild 模式，自动构建 MCUboot + 应用镜像。与 n2e-gw / angle-handler 构建方式一致。

```bash
# 1. 进入仓库根目录
cd ~/code/app/apps

# 2. 构建 (sysbuild 自动构建 MCUboot + 应用)
#    完整板定义放在仓库根 boards/io_edge_hub/ (module.yml board_root), 无需 -DBOARD_ROOT
west build -b io_edge_hub applications/io-edge-hub --sysbuild

# 发布模式
west build -b io_edge_hub applications/io-edge-hub --sysbuild \
    -DCONF_FILE=prj_release.conf

# 3. 烧录 MCUboot + 应用 (一次烧录两个镜像)
west flash

# 4. 后续升级: UDP 固件升级
python tools/udp_upgrade.py --ip 192.168.12.101 --port 8600 --file zephyr.signed.bin

# 5. 后续升级: CAN 固件升级
python tools/can_upgrade.py --interface can0 --file zephyr.signed.bin
```

> **sysbuild 说明**: `sysbuild.conf` 中的 `SB_CONFIG_MCUBOOT_MODE_SWAP_SCRATCH=y` 会让 sysbuild 自动配置 MCUboot 为 SWAP_SCRATCH 模式，并使用 `boards/io_edge_hub.pem` 签名密钥。构建产物包含 `mcuboot.bin` 和 `zephyr.signed.bin`。

### 10.3 烧录方式

```bash
# 方式1: west flash (sysbuild 模式, 一次烧录 MCUboot + 应用)
west flash

# 方式2: pyOCD (分别烧录)
pyocd load -e chip -t stm32f407vetx build/mcuboot/mcuboot.hex
pyocd load -e chip -t stm32f407vetx build/zephyr/zephyr.hex

# 方式3: probe-rs
cargo-flash --probe <probe_id> --chip STM32F407VETx --path build/zephyr/zephyr.elf
```

### 10.4 历史数据解析工具

> 从 RT-Thread `tools/data_parser.c` 迁移，数据格式完全兼容。

```bash
# 编译解析工具
cd tools && cmake . && make

# 解析历史数据文件
./parse data_0915_1120.raw
# 生成 data_0915_1120.raw.log (人类可读格式)
```

解析输出示例:
```
2024-09-15-11:20:01 DI data: ( 0: on 1: off 2: on )
2024-09-15-11:20:01 AI data: ( 0:12.50mA 1:8.30mA 2:5.00V 3:3.30V )
```

---

## 11. 开发计划

> **v3.1 变更**: 大量模块从 已验证实现 直接复用，开发周期显著缩短。标注 `[复用]` 的项为直接复用 已验证实现代码。

### Phase 1: 基础框架搭建 (Week 1)

- [ ] 创建 `applications/io-edge-hub/` 目录结构和 CMake
- [ ] 创建完整 F407 板定义 `boards/io_edge_hub/` (board.cmake/board.yml/*.dts/*_defconfig，
      含 HSE 13MHz + PLL 168MHz + 512KB Flash 分区，参考仓库根 `boards/nrf24_f103rct6/`)
- [ ] 创建板卡 overlay (`boards/io_edge_hub.overlay`) [复用已验证实现 overlay]
- [ ] 生成 MCUboot 签名密钥 (`boards/io_edge_hub.pem`, RSA-2048)
- [ ] 基础 Zephyr 编译通过 (sysbuild 模式)
- [ ] SWD 调试通道验证
- [ ] 日志输出验证 (UART1 Console)
- [ ] 验证 HSE 时钟配置 (外部 13MHz 晶振，PLL 168MHz: M=13/N=336/P=2)

### Phase 2: 存储子系统 (Week 2)

- [ ] SPI1 驱动验证，W25Q128 识别 (JEDEC ID: ef 40 18)
- [ ] Flash 分区表配置 (slot1 + scratch + storage + littlefs)
- [ ] **Zephyr settings + FCB 后端验证** (`modbus/` 命名空间，直接映射 holding_reg) [复用已验证实现]
- [ ] LittleFS 挂载和读写测试 [复用已验证实现]
- [ ] 验证 `zephyr,settings-partition` 设备树绑定

### Phase 3: 网络基础 (Week 3)

- [ ] SPI2 驱动验证，W5500 初始化
- [ ] W5500 RST / INT 控制验证 (RST=PD0 硬件复位, INT=PD1 中断, 与 HSE 无关, HSE 走 PH0/PH1)
- [ ] **MAC 地址从 STM32 UID 生成** (OUI: 00:08:DC)
- [ ] **静态 IP 配置** (从 holding_reg 读取，默认 192.168.12.101/24) [复用已验证实现 init.c]
- [ ] `CONFIG_ETH_NET_IF_NO_AUTO_START=y` 验证 (MAC 设置后 net_if_up)
- [ ] **`NET_EVENT_IF_UP/DOWN` 事件处理验证** (IF_DOWN 清零 DO + link_down 标志) [复用已验证实现 tcp.c]
- [ ] TCP/UDP Socket 通信基础测试 (POSIX API, select/FD_SET)
- [ ] **RTC 时间同步验证** (rtc_set_time + clock_settime + log_set_timestamp_func) [复用已验证实现 time.c]

### Phase 4: IO 采集 (Week 4)

- [ ] **16 路 DI + 8 路 DO + 8 路 LED GPIO** [复用已验证实现 dio.c]
- [ ] **4 路 AI ADC 读取 + 工程量转换** (电流 7.414x / 电压 3.7037x) [复用已验证实现 adc.c]
- [ ] DI/AI 独立线程采样 (默认 200ms，周期从 holding_reg 读取)
- [ ] DO 控制 + LED 联动 (`mb_set_do`)

### Phase 5: Modbus 通信 (Week 5-6)

- [ ] **Modbus 寄存器映射** (6 Input + 21 Holding，与已验证实现一致) [复用已验证实现 init.h]
- [ ] **保持寄存器写入回调** (DO/CFG_SAVE/REBOOT/TIMESTAMP/HIS_SAVE) [复用已验证实现 function.c]
- [ ] **Modbus TCP RAW ADU Server** (select() 多路复用，端口 502) [复用已验证实现 tcp.c]
- [ ] **Modbus RTU Slave** (UART2 + RS485, PA1 DE 控制) [复用已验证实现 rtu.c]
- [ ] **参数保存流程验证** (写 0x10 HOLDING_CONFIG_SAVE 触发 settings_save 全量导出)
- [ ] **参数加载流程验证** (settings_load 从 FCB 恢复到 holding_reg)
- [ ] **TCP Keepalive 验证** (主站掉线 ~45s 自动断开连接)
- [ ] TCP + RTU 并发运行验证

### Phase 6: FTP 服务 (Week 7)

- [ ] **FTP 服务器直接复用已验证实现** (`ftp_server/ftpd.c + ftp_cmds.c + ftp_handler.c`)
- [ ] USER/PASS 认证 (admin/admin)
- [ ] PORT/EPRT/PASV/EPSV 模式数据连接
- [ ] LIST / RETR / STOR / SIZE / REST / MKD / RMD / DELE 命令
- [ ] LittleFS 文件读写集成
- [ ] k_mem_slab 缓冲池验证 (无 malloc)
- [ ] FTP 客户端测试 (命令行 ftp / wget)

### Phase 7: 双通道固件升级 (Week 8-9)

- [ ] **UDP 固件升级** — 启用 `CONFIG_UDP_FW_UPGRADE=y`
- [ ] UDP app handler 注册 (`udp_fw_set_app_handler`) — 操作 holding_reg
- [ ] UDP app 命令实现 (SET_IP/GET_IP/SET_MODBUS/GET_MODBUS/SET_TIME/FACTORY_RESET)
- [ ] UDP 固件升级全流程测试 (FW_START -> FW_DATA -> FW_END -> 重启)
- [ ] **CAN 固件升级** — 启用 `CONFIG_CAN_FW_UPGRADE=y`
- [ ] CAN app handler 注册 (`can_fw_set_app_handler`)
- [ ] CAN 业务帧收发验证 (frame ID 从 holding_reg 读取)
- [ ] CAN 固件升级全流程测试 (Keyhash -> START_UPDATE -> DATA -> CONFIRM)
- [ ] **密钥哈希校验验证** (fw_keyhash.h，错误密钥拒绝升级)
- [ ] **版本字符串验证** (fw_gitver.h，GET_VERSION / 0x105 帧)
- [ ] **双通道同时工作验证** (UDP + CAN 同时可用)

### Phase 8: MCUboot 集成 (Week 10)

- [ ] MCUboot SWAP_SCRATCH 配置和编译 (sysbuild)
- [ ] **MCUboot LED 指示** (`SB_CONFIG_MCUBOOT_INDICATION_LED=y` in sysbuild.conf)
- [ ] 外部 Flash Secondary Slot + Scratch 分区配置
- [ ] 镜像签名验证 (RSA-2048, `boards/io_edge_hub.pem`)
- [ ] **SWAP_SCRATCH 升级 + 回滚测试** (升级 -> 验证 -> 回滚)
- [ ] 签名密钥管理流程

### Phase 9: 历史记录系统 (Week 11)

- [ ] **历史记录数据结构** (his_data, DI/AI 分离) [复用已验证实现 init.h]
- [ ] **k_fifo + k_work 异步写入** [复用已验证实现 history.c]
- [ ] 文件轮转 (10 文件 x 1MB, data_MMDD_HHMM.raw)
- [ ] FTP 下载历史文件验证
- [ ] PC 端解析工具兼容性验证 (data_parser.c)

### Phase 10: 集成测试与优化 (Week 12-13)

- [ ] 全功能集成测试
- [ ] **栈溢出保护验证** (k_sys_fatal_error_handler → warm reboot) [复用已验证实现 main.c]
- [ ] **状态 LED 验证** (300ms on / 2700ms off) [复用已验证实现 main.c]
- [ ] 看门狗和异常恢复测试
- [ ] 长时间稳定性测试 (24h+)
- [ ] 网络断连恢复测试 (link down → DO 清零 → link up 恢复)
- [ ] **双通道固件升级压力测试** (UDP + CAN 交替升级)
- [ ] Flash 写入寿命评估 (FCB 磨损均衡)
- [ ] 发布镜像优化
- [ ] 生产烧录流程文档

---

## 12. 风险评估与对策

### 12.1 技术风险

| 风险 | 等级 | 影响 | 对策 |
|------|------|------|------|
| W5500 Zephyr 驱动兼容性 | 中 | 网络功能不可用 | n2e-gw 已验证 Zephyr `eth_w5500` 驱动；参考其设备树配置 |
| HSE 晶振频率非标 (13MHz) | 低 | PLL 配置错误 | PLL 参数 M=13/N=336/P=2 已在已验证实现中确认；13MHz 晶振为板载硬件 |
| MCUboot SWAP_SCRATCH 外部 Flash | 高 | OTA 升级不可用 | 需验证 MCUboot 访问 SPI1 上的 Secondary Slot + Scratch；参考 Zephyr mcuboot sample |
| 内部 Flash 448KB 不足 | 中 | 应用镜像超限 | 使用 prj_release.conf 裁剪；关闭 Shell/调试 |
| LittleFS 替换 FATFS 兼容性 | 低 | 历史文件格式变化 | 历史数据结构 (`his_data`) 不变，仅文件系统层变化 |
| **FTP 服务器** | **低** | FTP 功能异常 | **直接复用已验证实现的 Zephyr 原生实现，k_mem_slab 无 malloc** |
| 双通道固件升级并发 | 低 | 升级冲突 | UDP 和 CAN 各有独立 RX 线程；同时升级时先到的先处理 |
| settings/FCB 参数持久化 | 低 | 参数丢失 | FCB 有磨损均衡和写入原子性保障；已验证实现 settings 直接映射 holding_reg |
| CAN 固件升级速度 (250kbps) | 低 | 升级耗时 | 448KB @ 250kbps ≈ 15s (8B/帧)；可接受 |
| 静态 IP 无 DHCP | 低 | 网络配置不灵活 | 通过 UDP SET_IP 命令远程修改 IP；Modbus 寄存器 0x0A-0x0D |
| **POSIX API 兼容性** | **低** | select/FD_SET 不可用 | **已验证实现 `CONFIG_POSIX_API=y` + Zephyr socket 兼容** |
| **栈溢出** | **低** | 系统崩溃 | **k_sys_fatal_error_handler 捕获 K_ERR_STACK_CHK_FAIL → warm reboot (已验证实现)** |
| **网络断连 DO 安全** | **低** | DO 输出失控 | **NET_EVENT_IF_DOWN 回调清零 DO + link_down 拒绝新连接 (已验证实现)** |

### 12.2 关键验证点

1. **HSE + PLL 168MHz 验证** (Phase 1): 外部 13MHz 晶振，确认 PLL (M=13/N=336/P=2) 可达 168MHz
2. **W5500 驱动 + 静态 IP 验证** (Phase 3): 确认 Zephyr `eth_w5500` + `CONFIG_ETH_NET_IF_NO_AUTO_START` + MAC from UID + 静态 IP (从 holding_reg 读取)
3. **MCUboot SWAP_SCRATCH 验证** (Phase 8): 确认 MCUboot 能访问 SPI1 上的 Secondary Slot + Scratch，并完成交换
4. **Flash 分区表验证** (Phase 2): 确认 DTS 分区表正确 (slot1: 0-448K, scratch: 448K-896K, storage: 896K-960K, lfs: 960K-16M)
5. **settings/FCB 参数持久化** (Phase 2): 确认 `modbus/` 命名空间参数写入/读取/出厂恢复正常
6. **双通道固件升级验证** (Phase 7): UDP 和 CAN 各自独立完成固件升级全流程
7. **Modbus 寄存器兼容性** (Phase 5): 确认上位机软件 (Modbus Poll) 可直接操作 18 个 holding 寄存器
8. **TCP Keepalive 验证** (Phase 5): 主站异常掉线 (拔网线/进程崩溃) 后 ~45s 内连接被自动断开
9. **网络断连 DO 安全验证** (Phase 3): 确认 NET_EVENT_IF_DOWN 时 DO 清零 + 新连接被拒绝

---

## 13. RT-Thread 到 Zephyr 迁移对照

### 13.1 API 映射表

| 功能 | RT-Thread API | Zephyr API |
|------|---------------|------------|
| 线程创建 | `rt_thread_create()` | `k_thread_create()` |
| 线程睡眠 | `rt_thread_mdelay()` | `k_sleep(K_MSEC())` |
| 定时器 | `rt_timer_create()` | `k_timer_init()` + `k_timer_start()` |
| GPIO 读写 | `rt_pin_read/write()` | `gpio_pin_get/set()` |
| GPIO 模式 | `rt_pin_mode()` | `gpio_pin_configure()` |
| ADC 读取 | `rt_adc_voltage()` | `adc_read()` |
| 消息队列 | `rt_mq_create/send/recv()` | `k_msgq_init/put/get()` |
| 互斥锁 | `rt_mutex_take/release()` | `k_mutex_lock/unlock()` |
| Socket | RT-Thread SAL socket | Zephyr `net/socket.h` |
| 文件操作 | DFS `open/read/write/close()` | POSIX `open/read/write/close()` |
| Flash 操作 | FAL `fal_partition_*()` | `flash_read/write/erase()` |
| 设备查找 | `rt_device_find()` | Device Tree + `DEVICE_DT_GET()` |
| **参数存储** | **magic+CRC16 (`config_write`)** | **`settings_save()` + FCB 后端 (modbus/ 命名空间, 直接映射 holding_reg)** |
| **固件升级** | **无** | **`udp_fw_upgrade` / `can_fw_upgrade` 共享库** |
| **网络初始化** | **`netdev_set_ipaddr` (DHCP/静态)** | **`net_if_ipv4_addr_add` (仅静态, IP 从 holding_reg 读取)** |
| **Socket 多路复用** | **RT-Thread SAL select** | **POSIX `select()` + `FD_SET` (`CONFIG_POSIX_API=y`)** |
| **时间管理** | **无** | **`rtc_set_time()` + `clock_settime()` + `log_set_timestamp_func()`** |

### 13.2 模块迁移对照

> **v3.1 变更**: 大量模块直接复用已验证的 Zephyr 实现，而非从 RT-Thread 迁移。

| RT-Thread 模块 | RT-Thread 文件 | Zephyr 对应 (io-edge-hub) | 迁移策略 |
|----------------|----------------|---------------------------|----------|
| 数字输入/输出 | `digital.c` | `modbus/dio.c` | **直接复用已验证实现** (16 DI + 8 DO + 8 LED, K_THREAD) |
| 模拟输入 | `adc.c` | `modbus/adc.c` | **直接复用已验证实现** (adc_dt_spec + 工程量转换) |
| Modbus TCP | `freemodbus` 包 | `modbus/tcp.c` | **直接复用已验证实现** (RAW ADU + select() 多路复用) |
| Modbus RTU | `freemodbus` 包 | `modbus/rtu.c` | **直接复用已验证实现** (MODBUS_MODE_RTU) |
| Modbus 寄存器 | `init.c` | `modbus/function.c` + `modbus/init.c` | **直接复用已验证实现** (21 holding + 6 input, settings 直接映射) |
| 历史记录 | `history.c` | `modbus/history.c` | **直接复用已验证实现** (k_fifo + k_work + 文件轮转) |
| **参数存储** | **`flash.c` (magic+CRC16)** | **`modbus/function.c` (settings/FCB, modbus/ 命名空间)** | **直接复用已验证实现 settings handler，直接映射 holding_reg[]** |
| FTP 服务 | `ftpd.c + ftp_cmds.c` | `ftp_server/ftpd.c + ftp_cmds.c + ftp_handler.c` | **直接复用已验证实现** (Zephyr 原生, k_mem_slab, select()) |
| **时间管理** | **无** | **`sys/time.c`** | **直接复用已验证实现** (RTC + clock_settime + log timestamp) |
| **主站保活** | **无** | **`modbus/tcp.c` (SO_KEEPALIVE)** | **TCP Keepalive** 检测主站连接存活 (取代应用层心跳) |
| **栈溢出保护** | **无** | **`main.c` (k_sys_fatal_error_handler)** | **直接复用已验证实现** (K_ERR_STACK_CHK_FAIL → warm reboot) |
| **状态 LED** | **无** | **`main.c` (mcuboot_led0)** | **直接复用已验证实现** (300ms on / 2700ms off) |
| **UDP 配置** | **`udp_bcast.c` (文本协议, 9002)** | **`udp.c` (app handler, 8600)** | **重写**，使用 `udp_fw_upgrade` app handler 模式，操作 holding_reg |
| **固件升级** | **无** | **`udp_fw_upgrade` + `can_fw_upgrade` (共享库)** | **直接复用共享库**，无需开发 |
| CAN | `can_app.c` | `can.c` (app handler) | 注册 `can_fw_set_app_handler`，业务帧 ID 从 holding_reg 读取 |
| 网络初始化 | `main.c + spi_eth_init.c` | `modbus/init.c` + `main.c` | **直接复用已验证实现** (静态 IP 从 holding_reg, NET_EVENT_IF_DOWN DO 安全) |
| 命令行 | `cmd.c` (MSH_CMD_EXPORT) | Zephyr Shell | 重写为 Shell 命令 |

### 13.3 配置迁移对照

| RT-Thread 配置 | Zephyr 配置 |
|----------------|-------------|
| `.config` (Kconfig) | `prj.conf` (Kconfig) |
| `rtconfig.h` | `autoconf.h` (自动生成) |
| `board/Kconfig` | `boards/io_edge_hub.overlay` |
| `fal_cfg.h` (FAL 分区表) | DTS `fixed-partitions` |
| `SConstruct` (SCons) | `CMakeLists.txt` (CMake + West + sysbuild) |
| **DHCP/静态 IP 切换** | **仅静态 IP (`CONFIG_NET_DHCPV4=n`), IP 从 holding_reg 读取** |
| **magic+CRC16 参数** | **Zephyr settings + FCB (`CONFIG_SETTINGS=y`), `modbus/` 命名空间直接映射 holding_reg** |
| **无 OTA** | **MCUboot SWAP_SCRATCH (`SB_CONFIG_MCUBOOT_MODE_SWAP_SCRATCH=y`)** |
| **无 POSIX** | **`CONFIG_POSIX_API=y` (select/FD_SET for Modbus TCP + FTP)** |
| **无 RTC 日志** | **`CONFIG_RTC=y` + `log_set_timestamp_func()` (RTC 时间戳)** |

---

## 附录 A: 关键 Zephyr 子系统对应关系

| 功能需求 | Zephyr 子系统/驱动 | Kconfig | 设备树 compatible |
|----------|---------------------|---------|-------------------|
| W5500 以太网 | `drivers/ethernet/eth_w5500.c` | `CONFIG_ETH_W5500` | `wiznet,w5500` |
| SPI NOR Flash | `drivers/flash/spi_nor.c` | `CONFIG_SPI_NOR` | `jedec,spi-nor` |
| LittleFS | `subsys/fs/littlefs` | `CONFIG_FILE_SYSTEM_LITTLEFS` | - |
| Modbus | `subsys/modbus` | `CONFIG_MODBUS` | - |
| ADC | `drivers/adc/adc_stm32.c` | `CONFIG_ADC_STM32` | - |
| CAN | `drivers/can/can_stm32_bxcan.c` | `CONFIG_CAN_STM32_BXCAN` | - |
| RTC | `drivers/rtc/rtc_stm32.c` | `CONFIG_RTC_STM32` | - |
| MCUboot | `boot/zephyr/` | `CONFIG_BOOTLOADER_MCUBOOT` | - |
| 看门狗 | `drivers/watchdog/iwdg_stm32.c` | `CONFIG_IWDG_STM32` | - |
| RTC | `drivers/rtc/rtc_stm32.c` | `CONFIG_RTC_STM32` | - |
| Socket API | `subsys/net/lib/sockets/` | `CONFIG_NET_SOCKETS` | - |
| Shell | `subsys/shell/` | `CONFIG_SHELL` | - |
| **Settings (FCB)** | **`subsys/settings/`** | **`CONFIG_SETTINGS` + `CONFIG_FCB`** | **`zephyr,settings-partition`** |
| **Modbus RAW ADU** | **`subsys/modbus`** | **`CONFIG_MODBUS_RAW_ADU`** | - |
| **POSIX API** | **`subsys/posix/`** | **`CONFIG_POSIX_API`** | - |
| **UDP 固件升级** | **`libs/udp_fw_upgrade`** | **`CONFIG_UDP_FW_UPGRADE`** | - |
| **CAN 固件升级** | **`libs/can_fw_upgrade`** | **`CONFIG_CAN_FW_UPGRADE`** | - |

## 附录 B: 默认设备参数

> **v3.1 变更**: Settings 命名空间从 `io/` 改为 `modbus/`，直接映射 holding_reg[]。寄存器布局与已验证实现一致 (v3.3 起 18 个 holding 寄存器)。

| 参数 | 默认值 | 说明 | 对应 settings key | 对应 Modbus 寄存器 |
|------|--------|------|-------------------|-------------------|
| **IP 地址** | **192.168.12.101** | 出厂默认静态 IP (参考 RT-Thread) | `modbus/ip` | 0x0A-0x0D |
| **子网掩码** | **255.255.255.0** | /24，代码中固定 | - | - |
| **网关** | **192.168.12.1** | 自动推导 (IP 最后字节 -> 1) | - | - |
| MAC 地址 | 从 STM32 UID 生成 | OUI: 00:08:DC，每板唯一 | - | - |
| Modbus TCP 端口 | 502 | 标准端口 | - | - |
| Modbus RTU Slave ID | 1 | - | `modbus/slave_id` | 0x09 |
| Modbus RTU 波特率 | 9600 | - | `modbus/rs485_bps` | 0x08 |
| DI 使能 | 0xFFFF (全使能) | 16 路 | `modbus/di/enable` | 0x01 |
| AI 使能 | 0x000F (全使能) | 4 路 | `modbus/ai/enable` | 0x02 |
| DI 采样间隔 | 200ms | - | `modbus/di/time` | 0x03 |
| AI 采样间隔 | 200ms | - | `modbus/ai/time` | 0x04 |
| 历史记录保存 | 关闭 (0) | - | `modbus/history` | 0x05 |
| CAN ID | 0x0111 | 标准帧 | `modbus/can/id` | 0x06 |
| CAN 波特率 | 250000 | 250kbps (与固件升级库一致) | `modbus/can/bps` | 0x07 |
| FTP 端口 | 21 | - | - | - |
| FTP 用户名 | admin | - | - | - |
| FTP 密码 | admin | - | - | - |
| **UDP 固件升级端口** | **8600** | `CONFIG_UDP_FW_CONFIG_PORT` | - | - |
| **CAN 固件升级帧 ID** | **0x101-0x105** | `can_fw_upgrade` 库定义 | - | - |
| 历史文件最大数 | 10 | 10 x 1MB = 10MB | - | - |
| 历史文件最大大小 | 1MB | 1024x1024 字节 | - | - |
| **时间戳** | **0** | 写 0x0E/0x0F 设置 RTC 时间 | - | 0x0E-0x0F |
| **参数保存** | **0** | 写 0x10 非零触发 settings_save | - | 0x10 |
| **系统重启** | **0** | 写 0x11 非零触发 sys_reboot | - | 0x11 |

> **v3.0 → v3.1 默认值变化**:
> - Settings 命名空间: `io/` → **`modbus/`** (直接映射 holding_reg, 复用已验证实现)
> - 寄存器数量: 15 → **21** (新增 TIMESTAMP/CFG_SAVE/REBOOT/HEART_EN/HEART_TIMEOUT/HEART)
> - Slave ID 寄存器: 0x0E → **0x09** (与已验证实现布局一致)
> - CFG_SAVE 寄存器: 0x09 → **0x10** (与已验证实现布局一致)
> - 新增: 心跳使能/超时 (0x12-0x13), 时间戳 (0x0E-0x0F), 重启 (0x11)

> **v3.0 默认值变化** (保留参考):
> - IP 地址: 192.168.1.101 -> **192.168.12.101** (参考 RT-Thread `init.c` 中的默认值)
> - CAN 波特率: 10000 -> **250000** (与 `CONFIG_CAN_FW_UPGRADE_BITRATE` 一致，提升固件升级速度)
> - UDP 配置端口: 9002 -> **8600** (由 `udp_fw_upgrade` 共享库统一管理)
> - UDP OTA 端口: 5000 -> **8600** (与 UDP 配置合并，共享库管理)
> - 参数存储: magic+CRC16 -> **Zephyr settings + FCB**
> - MCUboot 模式: Overwrite-only -> **SWAP_SCRATCH** (支持回滚)

## 附录 C: 历史数据文件格式

> 与 RT-Thread 完全兼容，PC 端 `data_parser.c` 可直接解析。

**文件命名**: `data_MMDD_HHMM.raw` (如 `data_0915_1120.raw` 表示 9月15日 11:20 创建)

**文件内容**: 连续的 `struct his_data` 记录流

```
+------------------------------------------------------+
| his_data (DI, 10 bytes)                              |
|   type=1 | timestamp(4B) | di_en(2B) | di_val(2B)   |
+------------------------------------------------------+
| his_data (AI, 16 bytes)                              |
|   type=2 | timestamp(4B) | ai_en(2B) | ai_val[4](8B) |
+------------------------------------------------------+
| his_data (DI, 10 bytes)                              |
|   ...                                                 |
+------------------------------------------------------+
```

**PC 端解析输出**:
```
2024-09-15-11:20:01 DI data: ( 0: on 1: off 2: on )
2024-09-15-11:20:01 AI data: ( 0:12.50mA 1:8.30mA 2:5.00V 3:3.30V )
```

---

## 附录 D: v2.0 -> v3.0 变更摘要

| 变更项 | v2.0 | v3.0 | 原因 |
|--------|------|------|------|
| 固件升级 | 自定义 UDP OTA (端口 5000, magic 0xAE) | 共享库 `udp_fw_upgrade` (端口 8600) + `can_fw_upgrade` | 复用已有库，支持双通道 |
| 参数持久化 | magic + CRC16 (`config_store`) | Zephyr settings + FCB 后端 | 使用 Zephyr 原生子系统，支持增量写入和磨损均衡 |
| MCUboot 模式 | Overwrite-only | SWAP_SCRATCH | 支持固件回滚，与 n2e-gw / angle-handler 一致 |
| Flash 布局 | slot1 (448KB) + storage (1MB) + lfs (~14.5MB) | slot1 (448KB) + scratch (448KB) + storage (64KB) + lfs (~15MB) | SWAP_SCRATCH 需要 scratch 分区；FCB 无需 1MB |
| 网络配置 | DHCP + 静态 IP | 仅静态 IP | 用户要求，简化配置 |
| 默认 IP | 192.168.1.101 | 192.168.12.101 | 参考 RT-Thread `init.c` 默认值 |
| MAC 地址 | 未明确 | 从 STM32 UID 生成 (OUI: 00:08:DC) | 参考 n2e-gw 实现 |
| UDP 配置协议 | 端口 9002/9001, 文本+二进制 | 端口 8600, app handler (0x10+) | 与 `udp_fw_upgrade` 共享端口 |
| CAN 波特率 | 10000 | 250000 | 与 `can_fw_upgrade` 库默认值一致 |
| 密钥哈希校验 | 无 | `fw_keyhash.h` (SHA-256, 32B) | 防止错误密钥签名的固件刷入 |
| 版本管理 | 手动版本号 | `fw_gitver.h` (git commit hash) | 自动生成，可追溯 |
| 项目结构 | 独立项目 | `applications/io-edge-hub/` (apps/ 仓库) | 与 n2e-gw / angle-handler 共享 libs/ |
| 线程模型 | 10 线程 (~18KB 栈) | 10 线程 (~17KB 栈) | 共享库线程替代自定义线程 |

---

## 附录 E: v3.0 -> v3.1 变更摘要

> **v3.1 核心变更**: 对比已验证实现，复用大量模块，降低开发风险。

| 变更项 | v3.0 | v3.1 | 原因 |
|--------|------|------|------|
| **Modbus TCP** | Zephyr 内置 `CONFIG_MODBUS_TCP_SERVER` | RAW ADU 模式 + 自定义 TCP Server (`select()` 多路复用) | 已验证实现；支持客户端管理/超时/link-down 安全 |
| **FTP 服务器** | 从 RT-Thread 迁移 (tcpserver→Socket) | 直接复用已验证实现 Zephyr 原生实现 | 已验证实现；k_mem_slab 无 malloc，支持全部 FTP 命令 |
| **参数持久化** | `g_params` 结构体 + `io/` 命名空间 + `persist.c` | Settings 直接映射 `holding_reg[]` + `modbus/` 命名空间 | 已验证实现；消除独立参数结构体和同步逻辑 |
| **Holding 寄存器** | 15 个 (SLAVE_ID@0x0E, CFG_SAVE@0x09) | 21 个 (SLAVE_ID@0x09, CFG_SAVE@0x10, 新增 TIMESTAMP/REBOOT/HEART) | 心跳+RTC 功能需要额外寄存器；布局与已验证实现一致 |
| **心跳看门狗** | 无 | `heart_poll` 线程 (k_sem, 超时清零 DO) | 工业安全：Modbus TCP 通信中断时自动断开输出 |
| **RTC 时间管理** | 仅 SNTP | RTC + `clock_settime` + `log_set_timestamp_func` + `set_timestamp()` | 已验证实现；日志使用 RTC 时间戳，Modbus/UDP 可设置时间 |
| **栈溢出保护** | 无 | `k_sys_fatal_error_handler` → warm reboot | 已验证实现；栈溢出自动重启 |
| **状态 LED** | 无 | `mcuboot_led0` 300ms on / 2700ms off + `CONFIG_MCUBOOT_INDICATION_LED` | 已验证实现；运行状态可视化 |
| **网络断连安全** | 无特殊处理 | `NET_EVENT_IF_DOWN` 清零 DO + `link_down` 拒绝新连接 | 已验证实现；网络断开时输出安全归零 |
| **POSIX API** | 未启用 | `CONFIG_POSIX_API=y` | Modbus RAW ADU TCP + FTP 服务器需要 `select()`/`FD_SET` |
| **系统时钟** | HSI (内部 16MHz RC) | HSE (外部 13MHz 晶振, PLL M=13/N=336/P=2 → 168MHz) | 板载外部晶振，HSE 精度高于 HSI，有利于 ADC 和通信 |
| **W5500 RST/INT** | PD0/PD1（误判为 HSE 引脚而省略） | RST=PD0 / INT=PD1 | HSE 实际走 `PH0`/`PH1`(OSC_IN/OSC_OUT)，PD0/PD1 与它无关，恢复 W5500 硬件复位/中断引脚 |
| **libc 配置** | NEWLIB_LIBC | `CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=0` | FTP 使用 k_mem_slab 替代 malloc，关闭 heap arena |
| **IO 采集模块** | 独立 `src/io/` 目录 (digital_input/analog_input/io_sampler) | 复用已验证实现 `modbus/dio.c` + `modbus/adc.c` | 已验证实现；DI/DO/LED/AI 紧耦合 Modbus 寄存器 |
| **历史记录** | `src/storage/history_store.c` (k_msgq) | 复用已验证实现 `modbus/history.c` (k_fifo + k_work) | 已验证实现；异步写入机制更高效 |
| **目录结构** | `src/io/` + `src/net/` + `src/storage/` + `src/sys/persist.c` | `src/modbus/` (统一) + `src/ftp_server/` + `src/storage/` + `src/sys/` | 模块化结构更清晰 |
| **开发周期** | 15 周 (Phase 1-10) | 13 周 (Phase 1-10, 大量模块复用) | 已验证模块直接复用，缩短开发周期 |

---

## 附录 F: v3.1 -> v3.2 变更摘要 (复核修正)

> **v3.2 变更**: 对照 `libs/` 共享库源码与 Zephyr v4.4.0 实际符号，修正会导致编译失败或功能静默失效的问题。

| 变更项 | v3.1 | v3.2 | 原因 |
|--------|------|------|------|
| **网络配置** | 缺 `CONFIG_NET_MGMT`/`CONFIG_NET_MGMT_EVENT` | §8.1 显式开启 | `udp_fw_upgrade` 库 `depends on NET_MGMT_EVENT`，缺失会被静默丢弃 |
| **镜像签名** | ECDSA-P256 | **RSA-2048** | `fw_keyhash`/`gen_keyhash.py` 仅支持 RSA (PKCS#1)，EC 密钥 configure 阶段报错；与 n2e-gw/angle-handler 一致 |
| **FW 线程优先级** | udp_fw_rx/can_fw_rx = 3 | = 8 (库默认) + prj.conf 显式固定 | 库默认 `CONFIG_*_RX_PRIORITY=8`，v3.1 未覆盖 |
| **UDP 回复** | 声称走 `udp_tx` 线程异步发送 | 澄清为 `udp_fw_reply()` 同步发送；补 64B 缓冲上限与跨子网广播回复端口 8601 | 与库实现一致 |
| **app handler 示例** | 签名带 `const struct sockaddr *src`，`udp_fw_reply(buf,len,src)` | 与 `udp_fw_app_cmd_cb_t`/`udp_fw_reply(cmd,data,len)` 一致 | 照抄无法编译 |
| **CAN 回调示例** | `const struct can_frame *` | 非 const `struct can_frame *` | 与 `can_fw_app_rx_cb_t` typedef 一致 |
| **MCUboot LED** | 应用 prj.conf 里 `CONFIG_MCUBOOT_INDICATION_LED=y` | sysbuild.conf `SB_CONFIG_MCUBOOT_INDICATION_LED=y` | 该符号属 MCUboot 镜像 Kconfig |
| **W5500 配置** | `CONFIG_ETH_W5500=y` + `CONFIG_ETH_W5500_SPI=y` | 删除（由设备树自动拉起；`ETH_W5500_SPI` 不存在） | 与 n2e-gw 一致 |
| **目录结构** | `src/modbus/udp.c` (JSON/多播 9002) | 删除该残留条目 | v3.1 已改 app handler，无 JSON/多播 |
| **看门狗** | 仅文字描述 | 补 `&iwdg` DTS 使能节点与超时配置说明 | `CONFIG_IWDG_STM32` 依赖设备树节点 |
| **板级定义** | 仅 overlay + pem | Phase 1 明确需完整板定义 `boards/io_edge_hub/`；构建命令去掉 `-DBOARD_ROOT` | 新 MCU 需 board.cmake/board.yml/*.dts/*_defconfig，且 board_root 在仓库根 |
| **内存/线程统计** | 10 线程 ~17KB / 合计 ~57KB | 11 线程 ~15KB / 合计 ~55KB | 与 §4.2 线程表对齐 |
| **HEAP 重复** | `CONFIG_HEAP_MEM_POOL_SIZE` 定义两次 (16384/2048) | 仅保留 16384 一次 | 消除重复与矛盾 |
| **JSON 残留** | `CONFIG_JSON_LIBRARY=y` | 移除 | v3.1 无 JSON 需求 |
| **IGMP** | `CONFIG_NET_IPV4_IGMP=y` | 移除 | 无多播需求 |
| **章节号** | 两个 §9.8 | 第二个改为 §9.9 | 编号唯一 |

---

## 附录 G: 实现备注 (代码落地与本文档的差异)

> 本附录记录 io-edge-hub 实际编码落地时, 因 Zephyr v4.4.0 实际 API / 符号 / 约束, 与上述设计章节的出入。**以实现代码为准**, 本文档其他章节为原始设计意图。

### G.1 板名

- 板名 `io_edge_hub` → **`io_edge_f407vet6`** (标识 io_edge + 芯片 F407VET6, 与 `nrf24_f103rct6` 命名风格一致); 项目名 `io-edge-hub` (应用目录) 不变。
- Kconfig 符号 `BOARD_IO_EDGE_HUB` → `BOARD_IO_EDGE_F407VET6`; 板 DTS model / yaml identifier / board.yml name 同步。

### G.2 "复用已验证实现" 实际为全新实现

§1 / §4 / §5 反复提到 "复用已验证实现" 的 Modbus RAW ADU TCP (tcp.c)、RTU (rtu.c)、FTP (ftp_server/)、DI/DO/AI (dio.c / adc.c)、历史记录 (history.c)、RTC (time.c)、栈溢出 / 状态 LED (main.c) 等模块 — **该源码并不在 iot-zephyr-app 仓库**, 全部按本文档规范全新实现。真正复用的是 `libs/udp_fw_upgrade` + `libs/can_fw_upgrade` 共享库。

### G.3 设备树 (§9)

- **ADC1** (§9.6): 用 `channel@a-d` 子节点 (含 `st,adc-clock-source = "SYNC"` + `st,adc-prescaler = <2>` 及 zephyr,gain/reference/acquisition-time/resolution)；通道号经 `/zephyr,user` 的 `io-channels` 引用，工程量转换系数放 `ai-coeffs`（自定义属性放 `&adc1` 子节点会触发 binding 校验报错）；代码用 `ADC_DT_SPEC_GET_BY_IDX` 生成 `adc_specs[]`（本 Zephyr 版本无 `ADC_DT_SPEC_ARRAY` 宏），系数用 `DT_PROP_BY_IDX` 读取。
- **CAN1** (§9.8): `bus-speed` → **`bitrate`** (bus-speed 在 v4.4 deprecated); CAN1 配置从应用 overlay 移到 **板 DTS** (`boards/io_edge_f407vet6/io_edge_f407vet6.dts`), 供 MCUboot 与应用共享 (未来 MCUboot 可能用 CAN)。
- **新增 chosen**: `zephyr,canbus = &can1` (can_fw_upgrade 库 `DT_CHOSEN(zephyr_canbus)` 必需); `zephyr,entropy = &rng` (硬件 RNG)。
- **RNG** (本文档未覆盖): 板 DTS `&rng { status = "okay"; }` (STM32F407 硬件 RNG, rng 节点 clock 已在 stm32f405.dtsi 定义 `AHB2,6`)。

### G.4 随机源 (本文档未覆盖)

- 用 **STM32 硬件 RNG** (`CONFIG_ENTROPY_GENERATOR` + `ENTROPY_STM32_RNG`, `RNG_GENERATOR_CHOICE` 默认 `ENTROPY_DEVICE_RANDOM_GENERATOR`), 替代 `CONFIG_TEST_RANDOM_GENERATOR` (测试用, 警告提示)。MCUboot 镜像 (`sysbuild/mcuboot.conf`) 同样配 `CONFIG_ENTROPY_GENERATOR=y` + `CONFIG_TIMER_RANDOM_GENERATOR=n` + `CONFIG_TEST_CSPRNG_GENERATOR=n`, 消除 mbedtls 的 test-random 警告。

### G.5 Kconfig 修正 (§8)

- 删除 `CONFIG_FLASH_JESD216` (无 prompt, 由 SPI_NOR 自动 select); 删除 `CONFIG_NET_SOCKETS_POSIX_NAMES` (v4.4 无此符号, POSIX_API 已覆盖); 删除 `CONFIG_FILE_SYSTEM_SHELL` (littlefs blk 模式需 disk driver); 删除 `CONFIG_FS_LITTLEFS_BLK_DEV` (改 flash-area 模式, 用 littlefs 分区)。
- `CONFIG_STACK_SENTINEL` → **`CONFIG_HW_STACK_PROTECTION`** (F407 有 MPU; STACK_SENTINEL 是 F1 无 MPU 的软件方案)。
- `SB_CONFIG_MCUBOOT_INDICATION_LED` sysbuild 符号不存在, 删除 (应用 main.c 的状态 LED 仍工作)。
- `FIXED_PARTITION_ID(label)` → **`PARTITION_ID(label)`** (v4.4 deprecation)。
- 新增 `CONFIG_FILE_SYSTEM_MKFS=y` (fs_mkfs, LittleFS 首次格式化兜底)。
- `west.yml` 加 `littlefs` 到 import name-allowlist (LittleFS 需外部 module)。

### G.6 模块实现差异 (§5)

- **FTP** (§5.3): 单文件 `ftpd.c` 实现 (非 ftpd.c / ftp_cmds.c / ftp_handler.c 三文件); **单线程 select 多路复用** (最多 3 个客户端命令交错, 数据传输时该会话独占, 第 4 个返回 421, per-session buffer); **PASV/EPSV + PORT/EPRT** 数据连接 (RFC 959 + 2428); **TYPE A (CR/LF 转换) + TYPE I**; LIST 标准 ls -l (历史文件名解析真实创建时间, Zephyr fs_dirent 不暴露 mtime)。
- **历史记录** (§5.6): **msgq + 系统工作队列** (`K_MSGQ_DEFINE` 16 槽累积 + `k_work_submit` 立即提交 (k_work 合并去重) + `fs_sync` 批量写, 减少 Flash 写次数), 替代 k_fifo + 独立 writer 线程; **单文件 1MB 大小轮转** (非按分钟切换, `data_MMDD_HHMMSS.raw`, 保留 10 个); 无独立线程。
- **DI / DO / LED** (§5.1): GPIO 通过 `/zephyr,user` 节点 `di-gpios` / `do-gpios` / `led-gpios` 列表定义, 代码 `DT_FOREACH_PROP_ELEM` 生成数组 (非独立 gpio-leds label 节点)。
- **Modbus TCP** (§5.2.1): RAW ADU iface 名 `RAW_0`; 每个请求独立 `struct modbus_adu`, 响应经全局槽 `g_resp` + 按 MBAP `trans_id` 匹配 (库 `modbus_server_handler` 回显 trans_id), 消除多客户端响应交叉污染; 客户端 socket 设 `SO_KEEPALIVE` + `SO_RCVTIMEO/SO_SNDTIMEO` (5s); 提交前提前校验 proto_id/length/unit_id, 避免无响应等待。
- **心跳废除**: 移除应用层心跳 (`heart_poll` 线程 / HEART 寄存器 0x12-0x14 / `heart_event_send`), holding 寄存器 21 → 18; Modbus TCP 连接存活改由 `SO_KEEPALIVE` 检测 (`CONFIG_NET_TCP_KEEPALIVE`, 空闲 30s / 探测 5s / 3 次)。
- **MCUboot** (§3.3): `sysbuild/mcuboot.conf` 加 `CONFIG_BOOT_MAX_IMG_SECTORS=256` (外部 Flash slot1/scratch 扇区数大) + `CONFIG_ENTROPY_GENERATOR` (硬件 RNG)。

### G.7 版本与构建

- 编译命令: `west build -b io_edge_f407vet6 applications/io-edge-hub --sysbuild` (建议 `--build-dir` 独立目录, 默认 `build/` 可能被其他应用占用)。
- debug 镜像 ~261KB, release (`-DCONF_FILE=prj_release.conf`) ~142KB (slot0 上限 448KB, 余量充足)。
- `LOG=n` (release) 时 `log_process()` / `log_set_timestamp_func()` 需 `#ifdef CONFIG_LOG` 保护。
- 项目文档: `applications/io-edge-hub/` 下新增 `README.md` / `USER_GUIDE.md` / `CLAUDE.md`。

---

## 附录 H: v3.2 -> v3.3 变更摘要 (code review 修复 + 心跳改 Keepalive + ADC 设备树化)

> **v3.3 变更**: 对已落地代码做一轮 code review 修复，并将主站连接保活从应用层心跳改为 TCP Keepalive，ADC 通道/系数设备树化。

| 变更项 | v3.2 | v3.3 | 原因 |
|--------|------|------|------|
| **主站保活** | 应用层心跳 (`heart_poll` 线程 + HEART 寄存器 0x12-0x14 + `heart_event_send`) | **TCP Keepalive** (`SO_KEEPALIVE`, `CONFIG_NET_TCP_KEEPALIVE`, 空闲30s/探测5s/3次) | Modbus 寄存器层无需心跳；keepalive 由协议栈检测连接存活，无线程开销；holding 21 → 18 |
| **Modbus TCP 响应** | 全局 `tmp_adu` 单槽复用 | 每请求独立 `modbus_adu` + `trans_id` 匹配响应 | `modbus_raw_submit_rx` 异步处理 (系统工作队列)，单槽会多客户端交叉污染 |
| **TCP 超时** | 无 recv/send 超时 | `SO_RCVTIMEO`/`SO_SNDTIMEO` 5s + 帧长/unit_id 提前校验 | 恶意/慢客户端可挂死单线程服务 |
| **历史使能恢复** | settings 加载后 `history_enabled` 未同步 | `modbus_settings_init` 加载后调 `history_enable_write()` | 重启后已使能的历史实际不写入 |
| **settings 键名** | `strncmp(name, "x", name_len)` 前缀匹配 | `NAME_IS` 宏长度精确匹配 | 防止 `di_enx` 之类误匹配 |
| **采样间隔** | 仅下限 10ms | 上下限钳制 [10ms, 5s] | 远程调大两个采样间隔会导致 IWDG 复位 |
| **FTP STOR** | `FS_O_CREATE\|FS_O_WRITE` | 追加 `FS_O_TRUNC` (REST 续传除外) | 覆盖上传残留旧文件尾部 |
| **历史 fs_tell** | `(uint32_t)fs_tell()` 失败转大数 | 显式错误检查 | 失败时反复轮转新建小文件 |
| **ADC 设备树化** | 通道/系数硬编码在 adc.c | `io-channels` + `channel@` 子节点 + `ai-coeffs` | 改通道/系数只动 overlay |
| **死配置清理** | `MODBUS_COLS_*`、`IO_NET_INIT_PRIORITY`、`CONFIG_SNTP` | 全部删除 | 从未被引用/未使用 |

---

## 附录 I: v3.3 -> v3.4 变更摘要 (UDP 协议精简 + 看门狗策略调整 + 寄存器重命名 + IP 校验统一)

> **v3.4 变更**: 对已落地代码做一轮协议精简与命名规范化，统一 IP 校验逻辑，调整看门狗喂狗策略。

| 变更项 | v3.3 | v3.4 | 原因 |
|--------|------|------|------|
| **UDP 应用协议** | 12 条命令 (SET/GET IP/MODBUS/SAMPLE/CAN/HIS, DISCOVER, FACTORY_RESET) | **6 条** (SET_IP / GET_IP / SET_MODBUS / GET_MODBUS / SET_TIME / FACTORY_RESET) | DI/AI/CAN/历史参数完全可经 Modbus holding 寄存器配，UDP 通道无需重复；协议面收敛降低误操作与文档维护成本 |
| **UDP 设备发现** | `DISCOVER` (0x18) 返回字符串 "io-edge-hub \<ip\> v\<ver\>" | `GET_IP` (0x11) 返回 4B 纯 IP，注册为广播允许命令 | DISCOVER 与 GET_NET 信息重叠；合并后协议更紧凑，对端解析简单 |
| **UDP 时间设置** | 无 (仅 Modbus 0x0E/0x0F) | 新增 `SET_TIME` (0x14)，4B 大端 Unix 时间戳 | UDP 通道补齐时间设置能力，与 Modbus 通道对等 |
| **SET_IP 行为** | 持久化后自动 `set_reboot_status(true)` 延迟重启 | 持久化后**不自动重启**，需客户端发 holding 0x11=1 或重新上电 | 自动重启会让上位机丢失连接难以确认结果；改由客户端显式控制生效时机 |
| **IP 合法性校验** | `udp.c::ip_valid` 与 `function.c::ip_is_valid_for_export` 两份重复实现，规则仅拦末字节 0/0xFF 与组播 224-239 | 统一公共函数 `ip_addr_valid(a,b,c,d)` (声明在 `init.h`，实现在 `function.c`)，扩展规则：拒绝 0.x / 127.x / 224+ / 末字节 0/0xFF | 消除代码重复；扩展拦截本网络/环回/保留段/限定广播等非法地址 |
| **看门狗超时** | 10s | **30s** | 与代码事实对齐 (`WDG_TIMEOUT_MS = 30000`)，文档原写 10s 为错误 |
| **看门狗喂狗** | `dio` / `adc_io` 采样线程周期喂狗 | `main` 主循环 (~3s 周期) 喂狗；`fs_littlefs.c` mkfs 前后事件型喂狗 (mkfs 跨 30s 窗口且此时 main 未运行) | 简化喂狗路径，避免采样间隔被远程调大导致误触发复位；保留 mkfs 事件喂狗防文件系统半损坏。代价：丢失"采样线程冻结则复位"的语义 |
| **HOLDING 寄存器数量** | Kconfig 默认 21 (实际枚举仅 18) | Kconfig 默认 **18** (与枚举一致) | 修正历史遗留过配，节省 6B RAM + 收敛 Modbus 可访问地址范围 |
| **HOLDING 寄存器命名** | 缩写晦涩 (`_EN` / `_SI` / `_HIS` / `_BPS` / `_ADDR_1` / `TIMESTAMPH` / `_CFG`) | 全称清晰 (`_ENABLE` / `_SAMPLE_MS` / `_HISTORY_ENABLE` / `_BAUDRATE` / `_OCTET1` / `TIMESTAMP_HI` / `_CONFIG`) | 提升源码可读性；settings 键名字符串不变，已部署设备无影响 |
| **TIMESTAMP 寄存器读取** | `holding_reg_rd_cb` 返回数组里的陈旧值 (仅写入时刷新) | 读 `0x0E/0x0F` 时实时调用 `time(NULL)` 拆分返回 | 修复读到上次写入值而非当前系统时间的 bug；副作用：FC03 成对读跨秒边界存在 < 1e-6 概率的时间撕裂，可重读恢复 |

---

*文档结束 - io-edge-hub 方案规划 v3.4 (已验证模块复用 + RAW ADU Modbus + Settings 直接映射 + TCP Keepalive 保活 + RTC 时间管理; 附录 G/H/I 记录实现差异)