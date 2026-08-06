# Angle Handler — 激光测距手持控制器

基于 Zephyr RTOS 的嵌入式手持控制器模块，运行在 STM32F103RCT6 上，用于激光测距设备的远程操控。采集操纵杆角度和电池状态，通过 CAN 总线或 2.4G 无线 (nRF24L01+) 与激光设备通信，提供 OLED 显示、系统休眠和 OTA 固件升级能力。

**版本**: v0.1.4_<6hex> (格式 `v<M>.<m>.<p>_<6位git commit>`) | **许可证**: Apache-2.0

## 功能特性

- **操纵杆控制** — 双通道 ADC 采集 X/Y 轴角度，变化时即时上报 + 显示刷新
- **双通道通信** — CAN 总线 (250Kbps) + 2.4G 无线 (nRF24L01+)，互斥冗余链路
- **CAN/2.4G 手动切换** — 通过 link_switch 按键 (PA10) 手动切换，切换时关闭对方电源并重新初始化，启动默认 CAN
- **CAN 手柄状态上报** — 帧 ID 0x1E3，8 字节 payload (X/Y 角度 BE + 按键 + 保留)，心跳成功时周期发送，角度/按键变化时即时发送
- **CAN 扫描仪数据接收** — 帧 ID 0x263/0x363/0x463，接收超欠挖、激光测距、X/Y/Z 坐标，即时显示
- **2.4G 遥测** — 帧格式 `[CAN ID 2B BE][payload]`，复用 CAN 帧内容，角度/按键变化时即时发送 (仅 CAN 断开时)
- **2.4G 数据接收** — 网关下发的扫描仪数据按 CAN ID 分发，复用 `mod_can_parse_scanner` 解析
- **中断驱动无线收发** — nRF24L01+ 驱动内置 IRQ 线程，收到数据自动投递到 msgq；发送等待硬件 ACK 返回结果 (成功/失败 + 耗时 + 重传次数)
- **心跳保活** — CAN 心跳帧 (800ms 周期)，连续 3 次失败停止心跳循环
- **电池管理** — 充电状态 GPIO 检测、电源电压 ADC 采集，毫伏阈值映射电量图标
- **OLED 显示** — SH1106 128x64 I2C 屏幕，事件驱动刷新，Row 0 使用图标（信号/电池/连接类型），变化时即时更新对应行
- **系统休眠/唤醒** — 电源键触发或 10 分钟无操作自动休眠，关闭所有外设电源；唤醒时重新上电并刷新显示
- **OTA 升级** — 通过 CAN 总线接收固件，写入外部 SPI Flash，MCUBoot 安全切换
- **外设电源管理** — CAN / 2.4G (nRF24) / 显示 / 手柄 四路独立 GPIO 电源开关，整合在 gpio.c 统一管理

## 硬件要求

- MCU: STM32F103RCT6 (ARM Cortex-M3, 72MHz, 256KB Flash, 48KB RAM)
- 外部 SPI Flash: GD25Q80 (存放 OTA 升级镜像)
- 无线模块: Nordic nRF24L01+ (SPI 接口，2.4GHz)
- OLED: SH1106 128x64 (I2C 地址 0x3C)
- CAN 收发器: 250Kbps

## 构建

```shell
# 标准构建（含 MCUBoot sysbuild）
west build -b nrf24_f103rct6 . --sysbuild

# 带 shell 和 imgmgr 调试工具
west build -b nrf24_f103rct6 . --sysbuild -Dangle-handler_SNIPPET=imgmgr-shell

# 清理重建
west build -b nrf24_f103rct6 . --sysbuild --pristine
```

## 烧录

sysbuild 构建包含 mcuboot + app，在 build 根目录执行 west flash 会按 flash_order 依次烧录全部镜像。默认使用 pyocd：

```shell
# 烧录全部 (mcuboot + app)，pyocd (默认)
west flash

# openocd
west flash --runner openocd

# probe-rs
west flash --runner probe-rs

# 只烧录 application (不含 mcuboot)
west flash --domain angle-handler
```

## 系统架构

```
main() + SYS_INIT
  |
  +-- 主循环 (main thread)
  |     +-- 启动时初始全屏刷新, 事件驱动休眠/唤醒
  |     +-- 10 分钟无操作自动休眠, 关闭所有外设电源
  |
  +-- CAN 总线 (priority 8 收发 / 11 心跳)
  |     +-- 消息收发与协议分发
  |     +-- 固件升级状态机
  |     +-- 心跳保活 (800ms) + 手柄状态上报 (0x1E3, 大端序)
  |     +-- 扫描仪数据接收 (0x263/0x363/0x463, 即时解析+显示)
  |
  +-- 2.4G 无线 nRF24L01+ (rf24.c + 驱动内置 IRQ 线程)
  |     +-- 中断驱动接收: IRQ→驱动 IRQ 线程排空 FIFO→投递 msgq
  |     +-- RX 线程 (priority 8): 从 msgq 取帧, 按 CAN ID 分发到扫描仪解析器
  |     +-- 遥测发送: nrf24_send 等待 ACK, 返回 {acked, elapsed_ms, retransmits}
  |     +-- 发送后自动切回 PRX 接收模式
  |
  +-- ADC 采集 (priority 7 角度 / 8 电压)
  |     +-- 操纵杆 X/Y 角度 (周期采集, 变化时即时显示+发送)
  |     +-- 电源电压 (200ms 周期, 变化时即时显示)
  |
  +-- GPIO 按键 + 电源管理 (PRE_KERNEL_2 初始化)
        +-- 按键防抖: ISR → k_work_delayable, 延时读 GPIO 电平确认
        +-- 操纵手柄按键 (ISR → k_work → 即时显示+发送)
        +-- 电源键 (ISR → k_work → 休眠/唤醒)
        +-- link_switch (ISR → k_work → CAN/2.4G 切换 + 对方电源关闭)
        +-- 充电状态 GPIO 检测
        +-- 4 路 GPIO 电源开关 (CAN/nRF24/显示/手柄)
```

### 线程清单

| 线程 | 函数 | 栈大小 | 优先级 | 说明 |
|------|------|--------|--------|------|
| CAN 收发 | `mod_can_process_thread` | 2048 | 8 | CAN 消息接收与协议处理 |
| CAN 心跳 | `can_heart_thread` | 1024 | 11 | 800ms 周期心跳 + 手柄状态上报，3 次失败停止 |
| 2.4G 接收 | `rf24_rx_thread` | 1024 | 8 | 从 rf24_rx_msgq 取帧，按 CAN ID 分发到扫描仪解析器 |
| ADC 角度 | `adc_read_thread` | 1024 | 7 | 周期采集操纵杆角度，变化时即时显示+发送 |
| ADC 电压 | `adc_power_thread` | 1024 | 8 | 200ms 周期采集电源电压，变化时即时显示 |
| nRF24 IRQ | `nrf24_irq_thread` (驱动内) | 1024 | 2 | IRQ 底半部：排空 RX FIFO→msgq，处理 TX 完成 |

### 线程间通信

- **全局状态**: `global_params_t` 结构体（操纵杆角度、电池状态、扫描仪数据、连接类型等）
- **事件通知**: `k_event` — WAKE_EVENT(休眠唤醒), CAN_EVENT/CAN_RX_EVENT(CAN 激活), RF24_EVENT(2.4G 激活)
- **消息队列**: CAN 接收使用 `k_msgq`；nRF24 驱动内部 IRQ 线程→应用 RX 线程使用 `k_msgq` (投递 `nrf24_frame`)
- **信号量**: nRF24 驱动 `irq_sem`(ISR→IRQ 线程) + `tx_done_sem`(IRQ 线程→send 线程)
- **工作队列**: `k_work` / `k_work_delayable` — 按键防抖、显示+发送
- **互斥锁**: `rf24_tx_mutex` (2.4G TX 序列化), `display_mutex` (显示), 驱动内部 `lock` (SPI 总线)

## OLED 显示布局

SH1106 128x64 像素，使用 8x16 ASCII 字体 + 独立位图图标：

```
Row 0: [2.4G/CAN图标][信号图标]            [电池图标]  ← 连接类型 + 信号 + 电池
Row 1: OB:+12345      Dis:12345                         ← 超欠挖 + 激光距离 (来自扫描仪)
Row 2: X:+15.0        Y:-3.5                            ← X/Y 坐标 (来自扫描仪)
Row 3: Z:+1.20                                          ← Z 坐标 (来自扫描仪)
```

Row 0 图标说明:
- **连接类型** (8x16): "2.4G" 或 "CAN" 文本 (ROW0_ASCII 模式) / 标签图标 (label_rf24/label_can)
- **信号图标** (16x16): 0-4 级柱状图 (基于 nRF24 TX 的 ACK 成功与重传次数做 EMA 估算，分段映射到 5 档)
- **电池图标** (24x16): 毫伏阈值映射 4 级电量图标，充电时显示闪电标记

启动时全屏刷新一次，之后各模块事件驱动更新对应行。所有 `display_write` 通过 `display_mutex` 保护，`display_str_pad` 用空格填充行尾清除旧数据残留。

## CAN 通信协议

帧 ID 定义在 `mod-can.h`:

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x101 | 平台 → 手柄 | 控制命令（升级启动/确认/版本查询/重启） |
| 0x102 | 手柄 → 平台 | 响应帧 |
| 0x103 | 平台 → 手柄 | 固件数据传输 |
| 0x104 | 平台 → 手柄 | 升级 keyhash (5 帧: `[seq 1B][chunk 7B]`, 共 32B) |
| 0x105 | 手柄 → 平台 | 版本字符串分帧 (VERSION 查询后: `[seq 1B][text 7B]`, 末帧 '\0' 填充) |
| 0x110 | 平台 → 手柄 | RF24 配置命令（设置/查询信道与地址） |
| 0x111 | 手柄 → 平台 | RF24 配置响应 |
| 0x763 | 手柄 → 平台 | 心跳保活 (800ms) |
| 0x1E3 | 手柄 → 平台 | 手柄状态 (X/Y 角度 BE + 按键 + 保留) |
| 0x263 | 平台 → 手柄 | 超欠挖 + 激光测距数据 |
| 0x363 | 平台 → 手柄 | X/Y 坐标数据 |
| 0x463 | 平台 → 手柄 | Z 坐标数据 |

### CAN 手柄状态帧格式 (ID 0x1E3, DLC 8, 大端序)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-1 | 2 | coord_x | int16_t BE, 单位 1° |
| 2-3 | 2 | coord_y | int16_t BE, 单位 1° |
| 4 | 1 | btn flags | bit0: btnHandler |
| 5-7 | 3 | reserved | 固定 0xFF |

### CAN 超欠挖+激光测距帧格式 (ID 0x263, DLC 8, 大端序)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | flags | bit0: overbreak_valid, bit1: laser_valid |
| 1 | 1 | reserved | — |
| 2-3 | 2 | overbreak_value | int16_t BE, 超欠挖值 |
| 4-7 | 4 | laser_value | uint32_t BE, 激光测距值 |

### CAN X/Y 坐标帧格式 (ID 0x363, DLC 8, 大端序)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-3 | 4 | coordX | int32_t BE, X 坐标 |
| 4-7 | 4 | coordY | int32_t BE, Y 坐标 |

### CAN Z 坐标帧格式 (ID 0x463, DLC 8, 大端序)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-3 | 4 | coordZ | int32_t BE, Z 坐标 |
| 4 | 1 | flags | bit0: coordz_valid (同时设置 X/Y/Z 有效) |
| 5-7 | 3 | reserved | — |

### CAN/2.4G 连接切换

系统默认 `connect_type = CAN_TYPE`，通过 link_switch 按键 (PA10) 手动切换：

1. 按下 link_switch → ISR → k_work → CAN ↔ 2.4G 切换
2. 切换时关闭对方外设电源 (rf24_deinit/can_power_enable)
3. 切换时即时刷新 OLED Row 0
4. CAN 模式：角度/按键变化时通过 CAN 发送，心跳线程周期发送手柄状态帧
5. 2.4G 模式：角度/按键变化时通过 nRF24 发送遥测帧，RX 线程接收扫描仪数据

## 2.4G 无线帧格式 (nRF24L01+)

单包 ≤ 32 字节，帧格式遵循 CAN ID + 数据模式：

```
[CAN ID 2B BE][payload 0~30B]
```

- **发送** (手柄→网关): ID = 0x1E3 (HANDLER_STATE)，payload = `[x 2B BE][y 2B BE][btn][0xFF 3B]`，与 CAN 帧内容一致
- **接收** (网关→手柄): 按 CAN ID 分发，支持 0x263 / 0x363 / 0x463，剥离 ID 后构造临时 `can_frame` 复用 `mod_can_parse_scanner()` 解析

### nRF24L01+ 中断驱动收发

驱动 (`drivers/nrf24l01p/`) 采用中断驱动模型（参考 Zephyr MCP2515 CAN 驱动）：

- **IRQ 引脚**下降沿 → ISR (`k_sem_give`，不碰 SPI) → 驱动内置 `nrf24_irq` 线程
- **接收**: IRQ 线程读 STATUS，循环排空 RX FIFO，每帧以 `K_NO_WAIT` 投递到用户注册的 msgq (`nrf24_add_rx_msgq`) 或回调 (`nrf24_add_rx_callback`)
- **发送**: `nrf24_send()` 切 PTX → CE 脉冲触发 → 等待 IRQ 线程处理 TX_DS(ACK)/MAX_RT(重传耗尽) → 返回 `nrf24_tx_result {acked, elapsed_ms, retransmits}` → 自动切回 PRX

## Shell 命令

```
link can       -- 切换到 CAN 模式
link rf24      -- 切换到 2.4G (nRF24L01+) 模式
```

## OTA 升级流程

使用 `libs/can_fw_upgrade` 共享库，协议与 n2e-gw 一致。

> **升级 keyhash 校验（默认开启）**：编译期由 `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE` 派生 32B keyhash（SHA-256 of RSA 公钥 PKCS#1 DER，即镜像 `IMG_TLV_KEYHASH`）。新上位机在发送 0x101 升级启动命令前，先发 5 帧 0x104 keyhash（每帧 1B seq + 7B 数据）凑齐 32B；启动时校验不一致回 `KEYHASH_ERROR` 且不擦写 Flash。老上位机不发 keyhash 帧仍按原流程放行，兼容旧协议。

1. 平台通过 CAN 帧 0x104 分 5 帧发送 32B keyhash（新上位机，可跳过兼容）
2. 平台通过 CAN 帧 0x101 发送升级启动命令
3. 手柄初始化外部 SPI Flash 写入上下文
4. 平台通过 CAN 帧 0x103 分片传输固件数据
5. 传输完成后验证镜像，确认写入成功
6. MCUBoot 以 swap-with-scratch 模式完成安全切换

## 引脚分配

| 功能 | 引脚 | 说明 |
|------|------|------|
| CAN TX/RX | PA12 / PA11 | CAN1, 250Kbps |
| nRF24 SPI | PB13 / PB14 / PB15 | SPI2 (SCK/MISO/MOSI) |
| nRF24 CS | PB12 | SPI2 片选 |
| nRF24 CE | PA9 | 片使能 |
| nRF24 IRQ | PC6 | 中断输入 (下降沿) |
| 调试串口 | PB7 / PB6 | USART1 (remap), 115200bps |
| OLED I2C | PB11 / PB10 | I2C2, SH1106 @ 0x3C |
| SPI Flash | PA5/PA6/PA7/PA4 | SPI1, GD25Q80 |
| CAN 电源 | PC7 | GPIO 输出 |
| 显示电源 | PC8 | GPIO 输出 |
| nRF24 电源 | PC9 | GPIO 输出 |
| 5V 使能 | PA8 | GPIO 输出 |
| ADC-X | PC4 (ADC1_CH14) | 操纵杆 X 轴 |
| ADC-Y | PC5 (ADC1_CH15) | 操纵杆 Y 轴 |
| ADC-VCC | PB1 (ADC1_CH9) | 电源电压采样 |
| 操纵手柄按键 | PB0 | GPIO 输入 |
| 充电满 | PA3 | GPIO 输入上拉，低有效 |
| 充电中 | PA2 | GPIO 输入上拉，低有效 |
| 电源键 | PA1 | GPIO 输入上拉 |
| Link Switch | PA10 | GPIO 输入上拉 (CAN/2.4G 手动切换) |

## 目录结构

```
include/
  common.h          -- 全局状态类型 (global_params_t, scanner_data_t, 事件位定义)
  mod-can.h         -- CAN 协议定义
  rf24.h            -- 2.4G 无线接口 (nRF24L01+ 封装: telemetry/data_send/init/deinit)
  display.h         -- 显示模块接口
  mod-gpio.h        -- GPIO 电源控制 + 按键 + 电池状态 + CAN/2.4G 切换
  persist.h         -- Settings 持久化 (connect_type)
  battery_icons.h   -- 电池图标位图 (24x16, 4 级电量 + 充电动画)
  label_icons.h     -- 连接类型标签图标位图 (8x16, label_can/label_rf24)
  signal_icons.h    -- 信号强度图标位图 (16x16, 0-4 级)
src/
  main.c            -- 入口 + 主循环 (休眠/唤醒事件驱动)
  can.c             -- CAN 收发 + 心跳线程 + 手柄状态上报 + 扫描仪数据解析 + 固件升级
  rf24.c            -- 2.4G 无线 (nRF24L01+) + msgq 驱动 RX 线程 + 遥测发送
  font_8x16.c       -- 8x16 ASCII 字体位图 (95 字符)
  font_5x8.c        -- 5x8 ASCII 字体位图
  adc.c             -- ADC 操纵杆角度采集 + 电源电压采样
  gpio.c            -- GPIO 按键防抖 + 电源管理 + CAN/2.4G 切换 + 休眠/唤醒 + Shell
  display.c         -- SH1106 OLED 显示 (8x16 文本 + 位图图标, display_str_pad 防残留)
  persist.c         -- Settings 持久化 (connect_type)
boards/
  nrf24_f103rct6.overlay  -- 板级 Devicetree 覆盖
sysbuild.conf            -- MCUBoot sysbuild 配置
sysbuild/mcuboot.conf    -- MCUBoot 编译参数
VERSION                  -- 版本号定义 (0.1.4-release)
```
