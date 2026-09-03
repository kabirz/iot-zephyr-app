# hw-bringup — 阿波罗 V2 STM32H743 全硬件自检

针对 `apollo_v2_h743iit6` 板的板载外设驱动 + 自检例程。上电自动跑一遍
所有可自检外设并打印 PASS/FAIL/SKIP；shell 命令 `hwtest` 重跑,
`beep [ms]` 手动测蜂鸣器 (自检不响蜂鸣器)。

以太网 (YT8512C) 自检见 `eth-bwtest` 例程。

## 实测结果 (2026-09-04, 最终: 14 PASS / 0 FAIL / 0 SKIP)

| # | 硬件 | 测试内容 | 结果 |
|---|---|---|---|
| 1 | LED DS0(红 PB1)/DS1(绿 PB0) | 交替闪烁 3 次 | PASS (肉眼) |
| 2 | 按键 WK_UP/KEY0/KEY1/KEY2 | 静止电平 4/4 正确 | PASS |
| 3 | I2C2 总线扫描 (PH4/PH5) | 发现 0x10/0x1E/0x20/0x36/0x50 共 5 个器件 | PASS |
| 4 | AP3216C 光距传感 @0x1E | 地址应答 (无 ID 寄存器) | PASS |
| 5 | ES8388 音频 codec @0x10 | 地址应答 | PASS |
| 6 | 24C02 EEPROM @0x50 | 0xF0 处写读 8 字节一致 (写后恢复) | PASS |
| 7 | QSPI W25Q256 32MB (四线) | 末页擦-写-读一致, JEDEC EF 40 19, 4 字节地址模式 | PASS |
| 8 | FMC SDRAM W9825G6KH 32MB | 全片 838 万字走样写读 **0 错误** | PASS |
| 9 | ADC1 PA5 (INP19) | 8 次采样 | PASS |
| 10 | DAC1_OUT1 PA4 | 输出 2048/4095 ≈ 1.65V | PASS (API) |
| 11 | PWM PA3 = TIM2_CH4 | 1kHz 50% | PASS (API) |
| 12 | FDCAN1 (PA11/PA12, 500k) | 回环收发 ID 0x123 x8 一致 | PASS |
| 13 | TF 卡 (SDMMC1 4 位) | 16GB 卡识别 + MBR 读取 | PASS (需插卡) |
| 14 | RTC (LSE 32.768k) | 3 秒走时 +3s | PASS |
| - | 蜂鸣器 (PCF8574 P0) | 手动 `beep [ms]` | 低电平=响 |
| 15 | TFTLCD 2.8寸 ILI9341 (FMC 8080) | 手动 `lcd`: ID 读回 0x9341, 初始化 + 8 色彩条 | PASS (实测) |

## 调试过程中踩的坑 (重要)

1. **QSPI 写静默失效**: 驱动 quad 模式默认用 1-4-4 页编程 (0x38),
   W25Q256 只支持 1-1-4 (0x32) —— 芯片忽略不认识的指令, `flash_write`
   返回成功但数据全 FF。修复: DTS 加 `writeoc = "PP_1_1_4"`。
2. **SDRAM 只有高 8MB 可用**: 板上 W9825 的 **BA0/BA1 bank 选择线接在
   PG4/PG5 (FMC_A14/A15)**, pinctrl 漏配时 BA 浮空恒为 11, 只有 bank3
   (地址最高 8MB) 能读写。修复: pinctrl 补 `fmc_a14_pg4 &fmc_a15_pg5`。
   注意本板 SDNE0/SDCKE0 也走非常规的 PC2/PC3 (`_c` 复用)。
3. **SDRAM 时序**: sdclk=HCLK/3=80MHz + CL2 (mode-register 0x220) +
   放松时序后全片 0 错误。120MHz/CL3 (-6 标称支持) 实测 44% 错误,
   板级信号完整性不够, 不用。
4. **PCF8574 状态跨复位保持**: 蜂鸣器一旦被程序置响, SWD 重烧固件
   后仍持续响 (只有断电能清)。固件 main() 第一件事强制 P0 拉高静音;
   极性为 **P0 低 = 响**。
5. 24C02 无 ID 寄存器可查, AP3216C 也没有 (0x03 是 IR 数据, 别当 ID 读)。
6. `sys_cache_data_flush_all()` 在本 fork 上调用即挂死系统 (H7 cache
   驱动问题), 避免使用。
7. **FMC LCD (8080 并口屏) 必须配 MPU 非缓存区**: Cortex-M7 默认把
   0x60000000 区当可缓存内存, LCD 写会滞留 D-Cache (现象: 读回自己
   刚写的值"回显"), 屏幕收不到任何数据。正确做法是板 DTS 声明
   `lcd_mem` 节点 (reg 0x60000000/64MB, `zephyr,memory-attr =
   <DT_MEM_ARM_MPU_IO>`), ARM MPU 驱动启动时**静态**建立 region
   (参见 zephyr/doc/services/mem_mgmt/index.rst)。
   注意: 不能在运行时手动写 MPU 寄存器 —— Zephyr 栈保护
   (MPU_STACK_GUARD) 在每次上下文切换时重写 MPU 动态区, 会把手动
   配的 region 清掉。静态 DT 方案与栈保护共存, 无需任何取舍。
   另: BCR1 bit31 置位是正常现象 (ATK 工作配置同样置位), 非写保护。

## 无法自动测试的硬件 (需外接模块/与以太网冲突)

| 硬件 | 原因 |
|---|---|
| USB OTG FS (PA11/PA12) | 与 FDCAN1 共用引脚, 底板 P9 二选一; 本例选 CAN。测 USB 时去掉 overlay 里 fdcan1, 加 `zephyr_udc0: &usbotg_fs` |
| RS232 COM2 (USART2 PA2/PA3) | PA2=ETH_MDIO, PA3=PWM — 与以太网冲突 |
| RS232 COM3 / ATK 模块 (USART3 PB10/PB11) | PB11=ETH_TX_EN — 与以太网冲突 |
| RS485 (USART2 + PCF8574 P6) | 同 USART2 冲突 |
| 无线模块座 NRF24L01 (SPI2 + PG10/PG12/PI11) | 需插模块; 驱动见本仓库 `apps/drivers/nrf24l01p` |
| SH3001 IMU / ST480MC 磁力计 | I2C 扫描见 0x36 在位, 但无上游驱动, 地址归属待原理图确认 |
| DHT11/DS18B20 (PB12 单总线) | 需外接传感器; PB12 还与 PCF8574 中断共享 |
| RGB LCD (J3 排针) / OV2640 摄像头 (DCMI) | 需外接屏幕/摄像头模块 (TFTLCD 8080 已支持, 见 `lcd` 命令) |
| ES8388 音频 (SAI1 PE2~PE6) | codec 在位已确认, SAI 录放音需应用层开发 |
| 红外接收 LF0038 (PA8) / 触摸按键 TPAD (PA5) | 需应用层解码/校准 |
| I2C 地址 0x36 器件 | 待确认 (SH3001 或 ST480MC) |

## 使用

```shell
source ~/.venv/zephyr/bin/activate
west build -b apollo_v2_h743iit6 apps/examples/hw-bringup -d build/apollo_hwtest
west flash -d build/apollo_hwtest
# 串口 (注意 RTS/DTR, 打开后用 st-flash --connect-under-reset reset 复位):
python -m serial.tools.miniterm /dev/ttyUSB0 115200 --rts 0 --dtr 0
```

shell 命令: `hwtest` 重跑自检; `beep [ms]` 手动蜂鸣器; `lcd` TFTLCD 测试 (探测/初始化/彩条)。

## 文件

- 板载固定外设 (QSPI/SDRAM/24C02/ADC/DAC/PWM/SDMMC1/RTC, 含本页所有
  踩坑修正) 已固化在 **板 DTS**
  `apps/boards/apollo_v2_h743iit6/apollo_v2_h743iit6.dts`
- `boards/apollo_v2_h743iit6.overlay` — 应用差异: 关以太网 +
  使能 FDCAN1 (PA11/12 与 USB 冲突, 板级不默认开)
- `main.c` — 自检套件 (每项独立 PASS/FAIL/SKIP, 失败不中断)
- RAM 占用 20KB / FLASH 113KB, 与以太网应用互不干扰
