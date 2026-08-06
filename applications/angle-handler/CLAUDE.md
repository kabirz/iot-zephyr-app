# CLAUDE.md -- angle-handler

## 项目愿景

angle-handler 是一个运行在 STM32F103RCT6 (ARM Cortex-M3, 72MHz, 256KB Flash, 48KB RAM) 上的 Zephyr RTOS 嵌入式应用。它作为激光测距系统的**手持控制器模块**，负责采集操纵杆角度、电池状态，并通过 CAN 总线或 2.4G 无线 (nRF24L01+) 与激光设备通信，同时提供 OLED 显示、系统休眠和 OTA 固件升级能力。

- **版本**: v0.1.4_<6hex> (格式 `v<M>.<m>.<p>_<6位git commit>`)
- **硬件平台**: nrf24_f103rct6 (自定义板)
- **Bootloader**: MCUBoot (swap-with-scratch 模式)
- **无线模块**: Nordic nRF24L01+ (SPI 接口，2.4GHz，中断驱动)
- **许可证**: Apache-2.0

---

## 架构总览

系统采用 Zephyr 多线程架构，通过 `global_params_t` 全局结构体在模块间共享状态，使用 `k_event` 进行跨线程事件通知。外围设备电源由 GPIO 独立控制，整合在 `gpio.c` 统一管理，支持系统休眠/唤醒。

```
main() + SYS_INIT
  |
  +-- 主循环 (main thread)
  |     +-- 启动时初始全屏刷新, 事件驱动休眠/唤醒
  |     +-- 10 分钟无操作自动休眠, 关闭所有外设电源
  |
  +-- CAN 总线 (mod_can_process_thread, priority 8)
  |     +-- 固件升级协议 (fw_update)
  |     +-- 心跳发送 (can_heart_thread, priority 11)
  |     +-- 手柄状态上报: 心跳成功时发送 0x1E3 帧 (X/Y BE + 按键 + 0xFF)
  |     +-- 扫描仪数据接收: 0x263/0x363/0x463 (即时解析+显示)
  |     +-- 心跳失败 3 次 → 停止心跳循环
  |
  +-- 2.4G 无线 nRF24L01+ (rf24.c + 驱动内置 nrf24_irq 线程)
  |     +-- 中断驱动接收: IRQ 引脚→驱动 IRQ 线程排空 RX FIFO→投递 msgq
  |     +-- RX 线程 (rf24_rx_thread, priority 8): 从 rf24_rx_msgq 取帧, 按 CAN ID 分发
  |     +-- 遥测发送: nrf24_send 等待 ACK, 返回 {acked, elapsed_ms, retransmits}
  |     +-- 发送后自动切回 PRX 接收模式
  |
  +-- ADC 操纵杆 (adc_read_thread, priority 7)
  |     +-- X/Y 角度采集, 变化时即时显示 + CAN/2.4G 发送
  |     +-- 电源电压采集 (adc_power_thread, priority 8), 变化时即时显示
  |
  +-- GPIO 按键 + 电源管理 (power_init, PRE_KERNEL_2; gpio_init, APPLICATION)
        +-- 按键防抖: ISR → k_work_delayable, 延时读 GPIO 电平确认
        +-- 操纵手柄按键 (ISR → k_work_delayable → 即时显示+发送)
        +-- 电源键 (ISR → k_work_delayable → 休眠/唤醒)
        +-- link_switch (ISR → k_work → CAN/2.4G 切换 + 关闭对方电源)
        +-- 充电状态 GPIO 检测 (read_battery_status)
        +-- 4 路 GPIO 电源开关 (CAN/nRF24/显示/手柄)
```

### 关键设计决策

- **全局状态共享**: `global_params_t global_params` (定义在 `display.c`) 是所有模块共享的中心状态结构，包含操纵杆角度、电池状态、扫描仪数据 (`scanner_data_t`)、事件对象等。
- **事件系统**: `k_event` 定义 4 个事件位 — `WAKE_EVENT BIT(0)` (休眠唤醒), `CAN_RX_EVENT BIT(1)` (CAN 数据接收), `CAN_EVENT BIT(2)` (CAN 激活), `RF24_EVENT BIT(3)` (2.4G 激活)。
- **事件驱动显示**: 启动时全屏刷新一次，之后各模块在数据变化时即时更新对应 OLED 行。`display_str_pad` 渲染字符串后用空格填充行尾，防止旧数据残留。
- **心跳保活机制**: CAN 心跳线程周期发送心跳帧，失败 3 次后停止循环。心跳成功时发送手柄状态帧 (0x1E3, 大端序)。
- **连接类型切换**: 通过 link_switch 按键 (PA10) 手动切换 CAN/2.4G。ISR 中提交 k_work，工作队列中调用 `connect_switch()` 关闭对方电源 + 重新初始化 + 即时刷新 OLED Row 0。启动默认 CAN_TYPE。
- **系统休眠/唤醒**: 电源键触发或 10 分钟无操作自动休眠，关闭所有外设电源。唤醒时重新上电、rf24_init (若 2.4G 模式)、等待 200ms 后 reinit 显示并全屏刷新。
- **外设独立供电**: CAN、nRF24、显示屏、手柄电源各有独立 GPIO 使能引脚，整合在 `gpio.c` 统一管理 (`power_init` PRE_KERNEL_2 初始化)。
- **按键防抖**: 所有按键 (电源键、操纵手柄键) 使用 `k_work_delayable` + `k_work_reschedule` 实现延时防抖。ISR 中提交延时 work (10ms)，work handler 中读取 GPIO 电平确认后执行业务逻辑。
- **OTA 通过 CAN**: 固件升级完全通过 CAN 总线传输，分两阶段 -- 控制帧走 `PLATFORM_RX` (0x101)，数据帧走 `FW_DATA_RX` (0x103)。使用 MCUBoot swap-with-scratch 确保掉电安全。
- **2.4G 无线帧格式**: 单包 ≤32 字节，`[CAN ID 2B BE][payload 0~30B]`，完全复用 CAN 帧内容布局。发送 ID=`HANDLER_STATE`(0x1E3) + `[x 2B][y 2B][btn][0xFF 3B]`；接收按 ID 分发到 `mod_can_parse_scanner()`，零重复代码。
- **nRF24 中断驱动收发**: nRF24L01+ 驱动 (`drivers/nrf24l01p/`) 采用中断驱动模型，参考 Zephyr MCP2515 CAN 驱动。IRQ 引脚下降沿 → ISR (`k_sem_give`，不碰 SPI) → 驱动内置 `nrf24_irq` 线程排空 RX FIFO、处理 TX 完成。应用通过 `nrf24_add_rx_msgq()` 注册 msgq 接收帧（参考 `can_add_rx_filter_msgq`）。
- **TX ACK 结果**: `nrf24_send()` 增加 `struct nrf24_tx_result *result` 出参，发送后等待 IRQ 线程处理 TX_DS/MAX_RT，返回 `{acked, elapsed_ms, retransmits}`（retransmits 来自 OBSERVE_TX.ARC_CNT）。发送完成自动切回 PRX。
- **TX/RX 协调**: send 切 PTX 时 PRIM_RX=0 不会收 RX，IRQ 只为 TX_DS/MAX_RT；发完自动切回 PRX 恢复接收。RX 线程从 msgq 取帧（`k_msgq_get`），不再轮询 `nrf24_recv`。
- **CAN 手柄状态协议**: 帧 ID 0x1E3 (`HANDLER_STATE`)，8 字节 payload (X/Y int16 BE + 按键 + 0xFF 保留)。所有多字节数据使用大端序 (网络字节序)。
- **CAN 扫描仪数据接收**: 帧 ID 0x263 (`OVERBREAK_LASER`), 0x363 (`COORD_XY`), 0x463 (`COORD_Z`)，接收扫描仪下发的超欠挖+激光测距、X/Y/Z 坐标数据。解析后存入 `global_params.scanner` 并即时触发显示刷新。
- **OLED 显示**: 事件驱动刷新。启动时 `mod_display_all` 全屏刷新，之后各模块在数据变化时调用对应行刷新函数。Row 0 使用独立位图图标：连接类型 (`label_icons.h`, 8x16, CAN/2.4G)、信号强度 (`signal_icons.h`, 16x16, 0-4级)、电池图标 (`battery_icons.h`, 24x16, 4级电量+充电动画)。
- **连接类型**: `connect_type` 字段区分 `CAN_TYPE (1)` 和 `RF24_TYPE (2)`，默认 CAN。通过 link_switch (PA10) 手动切换，切换时即时刷新 OLED Row 0。持久化到 settings (FCB 后端)。
- **Shell 调试**: `link can` / `link rf24` 切换连接模式。

---

## 构建与开发

### 构建

```shell
# 标准构建（带 MCUBoot sysbuild）
west build -b nrf24_f103rct6 . --sysbuild

# 带 shell 和 imgmgr snippet
west build -b nrf24_f103rct6 . --sysbuild -Dangle-handler_SNIPPET=imgmgr-shell
```

### 烧录

```shell
west flash
```

### 清理重建

```shell
west build -b nrf24_f103rct6 . --sysbuild --pristine
```

### 硬件连接 (devicetree overlay)

板级 overlay 定义在 `boards/nrf24_f103rct6.overlay`，核心引脚分配:

| 功能 | 引脚 | 说明 |
|------|------|------|
| CAN 电源 | PC7 | GPIO 输出 |
| 显示电源 | PC8 | GPIO 输出 |
| nRF24 电源 | PC9 | 驱动 `power-gpios` 管理（init 上电 + POR 延时） |
| 5V 使能 | PA8 | GPIO 输出 |
| 操纵手柄按键 | PB0 | GPIO 输入 |
| 充电满 | PA3 | GPIO 输入上拉 (低有效) |
| 充电中 | PA2 | GPIO 输入上拉 (低有效) |
| 电源键 | PA1 | GPIO 输入上拉 |
| Link Switch | PA10 | GPIO 输入上拉 (CAN/2.4G 手动切换) |
| ADC-X | PC4 (ADC1_CH14) | 操纵杆 X 轴 |
| ADC-Y | PC5 (ADC1_CH15) | 操纵杆 Y 轴 |
| ADC-VCC | PB1 (ADC1_CH9) | 电源电压采样 |
| CAN | PA11/PA12 | CAN1, 250Kbps |
| nRF24 SPI | PB13/PB14/PB15 | SPI2 (SCK/MISO/MOSI) |
| nRF24 CS | PB12 | SPI2 片选 |
| nRF24 CE | PA9 | 片使能 |
| nRF24 IRQ | PC6 | 中断输入 (下降沿) |
| 调试串口 | PB6/PB7 | USART1 (remap), 115200bps |
| OLED I2C | PB10/PB11 | I2C2, SH1106 @ 0x3C |
| SPI Flash | PA5/PA6/PA7/PA4 | SPI1, GD25Q80 (存放升级镜像) |

---

## CAN 通信协议

帧 ID 定义在 `mod-can.h`:

| 帧 ID | 方向 | 用途 |
|-------|------|------|
| 0x101 (`PLATFORM_RX`) | 平台->手柄 | 控制命令 (升级/确认/版本/重启) |
| 0x102 (`PLATFORM_TX`) | 手柄->平台 | 响应帧 |
| 0x103 (`FW_DATA_RX`) | 平台->手柄 | 固件数据传输 |
| 0x763 (`COBID_HEATBEAT`) | 手柄->平台 | 心跳 (800ms 周期) |
| 0x1E3 (`HANDLER_STATE`) | 手柄->平台 | 手柄状态 (X/Y BE + 按键) |
| 0x263 (`OVERBREAK_LASER`) | 平台->手柄 | 超欠挖 + 激光测距 (BE) |
| 0x363 (`COORD_XY`) | 平台->手柄 | X/Y 坐标 (BE) |
| 0x463 (`COORD_Z`) | 平台->手柄 | Z 坐标 (BE) |

### CAN 手柄状态帧格式 (ID 0x1E3, DLC 8, 大端序)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0-1 | 2 | coord_x | int16_t BE, 单位 0.1° |
| 2-3 | 2 | coord_y | int16_t BE, 单位 0.1° |
| 4 | 1 | btn flags | bit0: btnHandler |
| 5-7 | 3 | reserved | 固定 0xFF |

---

## 2.4G 无线帧格式 (nRF24L01+)

单包 ≤ 32 字节：`[CAN ID 2B BE][payload 0~30B]`

| 帧类型 | 方向 | 内容 |
|--------|------|------|
| 遥测 | 手柄→网关 | ID=0x1E3 + `[x 2B][y 2B][btn][0xFF 3B]` |
| 扫描仪数据 | 网关→手柄 | `[CAN ID 2B][CAN data NB]`，按 ID 分发到 `mod_can_parse_scanner()` |

### nRF24L01+ 驱动中断驱动模型

驱动位于 `drivers/nrf24l01p/`，采用中断驱动收发（参考 Zephyr MCP2515 CAN 驱动 `can_mcp2515.c`）：

- **接收**: IRQ→ISR→`nrf24_irq` 线程排空 RX FIFO→投递 `struct nrf24_frame {len, data[32]}` 到用户注册的 msgq (`nrf24_add_rx_msgq`) 或回调 (`nrf24_add_rx_callback`)。未注册时投递到驱动内部兜底 msgq 供 `nrf24_recv` 兼容使用。
- **发送**: `nrf24_send(dev, buf, len, timeout, &result)` 切 PTX→CE 脉冲→等 IRQ 线程处理 TX_DS/MAX_RT→返回 `nrf24_tx_result {acked, elapsed_ms, retransmits}`→自动切回 PRX。
- **API**: `nrf24_send`, `nrf24_recv` (poll 兼容), `nrf24_add_rx_msgq`, `nrf24_add_rx_callback`, `nrf24_start_rx`, `nrf24_set_mode`, `nrf24_configure`。详见 `drivers/nrf24l01p/nrf24l01p.h`。

---

## 源码结构

```
include/
  common.h          -- 全局状态类型定义 (global_params_t, scanner_data_t, 事件位, CAN_TYPE/RF24_TYPE)
  mod-can.h         -- CAN 协议定义 + OTA 接口
  rf24.h            -- 2.4G 无线接口 (nRF24L01+ 封装: telemetry/data_send/init/deinit/link_status)
  display.h         -- 显示模块接口
  mod-gpio.h        -- GPIO 电源控制 + 按键 + 电池状态 + CAN/2.4G 切换 (connect_switch)
  persist.h         -- Settings 持久化 (connect_type)
  battery_icons.h   -- 电池图标位图 (24x16, 4级电量 + 充电动画)
  label_icons.h     -- 连接类型标签图标位图 (8x16, label_can/label_rf24)
  signal_icons.h    -- 信号强度图标位图 (16x16, signal_levels 0-4级)
src/
  main.c            -- 入口 + 主循环 (休眠/唤醒事件驱动, 10 分钟无操作自动休眠)
  can.c             -- CAN 收发 + 心跳线程 + 手柄状态上报 (0x1E3 BE) + 扫描仪数据解析
  firmware.c        -- OTA 固件升级状态机
  rf24.c            -- 2.4G 无线 (nRF24L01+) + msgq 驱动 RX 线程 + 遥测发送 + 按 CAN ID 分发
  font_8x16.c       -- 8x16 ASCII 字体位图 (95 字符)
  font_5x8.c        -- 5x8 ASCII 字体位图
  adc.c             -- ADC 操纵杆角度采集 + 电源电压采样 (变化时即时显示+发送)
  gpio.c            -- GPIO 按键防抖 + 电源管理 + CAN/2.4G 切换 + 休眠/唤醒 + Shell (link can/rf24)
  display.c         -- SH1106 OLED 显示 (8x16 文本 + 位图图标, display_str_pad 防残留, global_params 定义)
  persist.c         -- Settings 持久化 (connect_type, FCB 后端)
boards/
  nrf24_f103rct6.overlay  -- 板级 devicetree 覆盖
  nrf24_f103rct6.pem    -- 独立签名密钥（本应用专用，与 n2e-gw 不同）
sysbuild.conf            -- MCUBoot 配置（签名密钥 ${APP_DIR}/boards/nrf24_f103rct6.pem）
sysbuild/mcuboot.conf    -- MCUBoot 参数
VERSION                  -- 版本号 (0.1.4-release)
```

### MCUboot 签名密钥（每应用独立）

`sysbuild.conf` 用 `${APP_DIR}/boards/nrf24_f103rct6.pem` 指向**本应用目录**下的密钥（不是板子目录的共享密钥）。这样 angle-handler 和 n2e-gw（共用同一块板 `nrf24_f103rct6`）**各自一把独立 RSA-2048 密钥**，互不通用：

- 一个 mcuboot 镜像只能验签它自己嵌入公钥对应的应用，**不能跨应用引导**。
- 密钥重新生成：`python bootloader/mcuboot/scripts/imgtool.py keygen -k <app>/boards/nrf24_f103rct6.pem -t rsa-2048`（改密钥后 mcuboot 和 app 必须重新烧写）。

---

## 线程清单

| 线程名 | 函数 | 栈大小 | 优先级 | 说明 |
|--------|------|--------|--------|------|
| `thread_can_rx` | `mod_can_process_thread` | 2048 | 8 | CAN 消息接收与分发 |
| `thread_can_heart` | `can_heart_thread` | 1024 | 11 | CAN 心跳保活 + 手柄状态周期上报 |
| `thread_rf24_rx` | `rf24_rx_thread` | 1024 | 8 | 2.4G 接收：从 rf24_rx_msgq 取帧，按 CAN ID 分发 |
| `thread_adc` | `adc_read_thread` | 1024 | 7 | ADC 周期采集 (X/Y 角度，变化时即时发送) |
| `thread_adc_power` | `adc_power_thread` | 1024 | 8 | ADC 电源电压采样 (200ms 周期，变化时即时显示) |
| `nrf24_irq` | `nrf24_irq_thread` (驱动内) | 1024 | 2 | nRF24 IRQ 底半部：排空 RX FIFO→msgq，处理 TX 完成 |

---

## 编码规范

- 使用 Zephyr Log 模块 (`LOG_MODULE_REGISTER`, `LOG_INF/ERR/DBG/WRN`)
- 设备获取使用 devicetree 宏 (`DEVICE_DT_GET`, `GPIO_DT_SPEC_GET`)
- 线程使用 `K_THREAD_DEFINE` 静态定义
- 初始化使用 `SYS_INIT` 分级初始化
- GPIO 引脚全部通过 `zephyr,user` devicetree 节点定义
- 头文件保护使用 `#ifndef _MOD_XXX_H__` 或 `#ifndef __MOD_XXX_H__` 模式
- 显示为事件驱动：各模块数据变化时直接调用对应行刷新函数，不在主循环轮询刷新
- 所有 display_write 调用通过 `display_mutex` 保护
- 显示函数使用 `display_str_pad` 替代 `display_str`，空格填充行尾防止旧数据残留
- ISR 中通过 `k_work_reschedule` 提交延时任务，不在中断上下文做显示/发送操作
- 按键防抖使用 `k_work_delayable`，ISR 中 reschedule，work handler 中读 GPIO 电平确认

---

## AI 使用指引

- 修改 CAN 协议时，同步更新 `mod-can.h` 中的枚举定义
- 添加新外设时，先在 `boards/nrf24_f103rct6.overlay` 的 `zephyr,user` 节点中定义引脚，然后在对应驱动中用 `GPIO_DT_SPEC_GET` / `ADC_DT_SPEC_GET_BY_IDX` 获取
- `global_params_t` 中 `connect_type` 使用 `CAN_TYPE (1)` / `RF24_TYPE (2)` 区分连接类型，通过 link_switch (PA10) 手动切换
- `global_params` 定义在 `display.c`，声明在 `common.h`
- 事件位定义: `WAKE_EVENT BIT(0)`, `CAN_RX_EVENT BIT(1)`, `CAN_EVENT BIT(2)`, `RF24_EVENT BIT(3)`
- Flash 分区: 内部 Flash 仅存放 mcuboot(64KB) + image-0(192KB)，image-1 和 scratch 在外部 SPI Flash (GD25Q80)
- `main.c` 中的 `#ifndef CONFIG_FLASH_SIZE` 保护是针对 native_sim 构建的兼容处理
- 字体/图标位图取模方式: Column-major, page-interleaved, LSB=top pixel (与 SH1106 原生格式一致)。图标数据按 page0 cols[0..N-1] 然后 page1 cols[0..N-1] 排列
- 图标位图定义在独立头文件: `battery_icons.h` (24x16, 电池)、`label_icons.h` (8x16, 连接类型标签)、`signal_icons.h` (16x16, 信号强度)
- Row 0 显示布局: 连接类型 (8x16) + 信号图标 (16x16) + 电池图标 (24x16)
- 显示接口: `mod_display_rf24(rssi)`, `mod_display_can()`, `mod_display_battery(power_mv, status)`, `mod_display_scanner(s)`, `mod_display_all(params)`
- `mod_display_init` 中通过 `handler_get_btn()` 读取初始按键状态写入 `global_params.h_button`
- `mod_display_battery` 参数为 `(uint32_t power_mv, battery_status_t status)`，电源电压使用 ADC 毫伏值，毫伏阈值映射电量图标 (≥3850mV=4级, ≥3750mV=3级, ≥3550mV=2级, ≥3400mV=1级, <3400mV=0级)
- CAN 帧使用大端序 (网络字节序): 发送用 `sys_put_be16`/`sys_put_be32`，接收用 `sys_get_be16`/`sys_get_be32`
- CAN/2.4G 切换通过 `global_params.connect_type` 控制: link_switch 按键 ISR→k_work→`connect_switch()` 关闭对方电源 + 重新初始化
- 扫描仪数据通过 `global_params.scanner` (scanner_data_t) 存储，CAN 接收线程或 2.4G RX 线程写入并即时触发显示刷新
- 2.4G 无线: `rf24.c` 封装 nRF24 驱动。`rf24_init()` 注册 msgq + 进 PRX；`rf24_data_send(can_id, data, len)` 组帧 `[CAN ID 2B BE][data]` 后 `nrf24_send`；`rf24_send_telemetry(params)` 发 0x1E3 遥测帧；RX 线程从 `rf24_rx_msgq` 取 `nrf24_frame`，按 CAN ID 构造临时 `can_frame` 调 `mod_can_parse_scanner()`
- 2.4G TX 线程安全: `rf24_tx_mutex` 保护 `rf24_data_send()`，防止 ADC 线程、按键 k_work 并发 TX 竞争
- nRF24 驱动 (`drivers/nrf24l01p/`) 是独立的 Zephyr 驱动模块，通过 `apps/zephyr/module.yml` 自动编入。公共头 `nrf24l01p.h` 由驱动 CMake 通过 `zephyr_library_include_directories` 全局导出，app 设 `CONFIG_NRF24L01P=y` 即可 `#include <nrf24l01p.h>`
- nRF24 驱动中断模型: ISR 仅 `k_sem_give(&irq_sem)`；`nrf24_irq` 线程读 STATUS 分发 TX_DS/MAX_RT→give `tx_done_sem` (通知 send)，RX_DR→循环读 FIFO→投递 `nrf24_frame` 到用户 msgq/callback/兜底 msgq
- `btn_display_work`、`sleep_work`、`linksw_work` 为 `gpio.c` 内部 static 变量，ISR 中 `k_work_reschedule`/`k_work_submit`，工作队列线程执行
- 按键防抖: `btn_display_work` 和 `sleep_work` 使用 `k_work_delayable`，ISR 中 reschedule，work handler 中 `gpio_pin_get_dt()` 确认
- 电源控制函数定义在 `gpio.c`，声明在 `mod-gpio.h`: `can_power_enable()`, `dis_power_enable()`, `handler_power_enable()`, `handler_get_btn()`（nRF24 电源由驱动 `nrf24_power_enable()` 按 `power-gpios` 管理）
- 电池状态读取: `read_battery_status()` 定义在 `gpio.c`，声明在 `mod-gpio.h`，返回 `battery_status_t` 枚举
- 系统休眠: `system_sleep()` 关闭所有外设电源 (can/dis/handler_power_enable + rf24_deinit)，设置 `sleeping = true`，clear WAKE_EVENT（rf24_deinit 内部 `nrf24_power_enable(false)` 断电 nRF24）
- 系统唤醒: `system_wake()` 重新上电所有外设，2.4G 模式下调用 `rf24_init()`，等待 200ms 后 `mod_display_reinit()` + `mod_display_all()`，设置 `sleeping = false`
- nRF24 电源时序封装在 `rf24_init/deinit` 内: init = `nrf24_power_enable(true)`（上电 + POR + 重配 + 进 PRX）后应用持久化配置；deinit = `nrf24_power_enable(false)`（软关机 + 断电）。电源由驱动按 `power-gpios` 管理，POR 延时 `CONFIG_NRF24L01P_POWER_ON_DELAY_MS`
- 命名说明: 板名 `nrf24_f103rct6` 取自 nRF24L01+ 射频芯片；代码中无线相关符号统一为 RF24 前缀 (RF24_TYPE/RF24_EVENT/mod_display_rf24/label_rf24/connect_switch/link rf24)
