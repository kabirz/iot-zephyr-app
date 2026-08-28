# GD32H759 (gd32h759_btb) Zephyr 移植与点亮记录

> 板卡：野火 GD32H759IMK6 BTB 开发板（核心板 EBF410304 + 底板 EBF410305）
> 状态：最小系统已点亮（hello_world @ 600MHz，USART0 115200 正常输出）
> 日期：2026-08-28

## 1. 移植文件清单

### Zephyr 树内（`zephyr/`，需随 fork 维护）

| 文件 | 说明 |
|------|------|
| `soc/gd/gd32/gd32h7xx/` | SoC 层：Kconfig（CPU_CORTEX_M7/双精度 FPU/MPU/Cache）、soc.c（调 `SystemInit()`）、`gd32_regs.h`（RCU 寄存器偏移） |
| `soc/gd/gd32/soc.yml` | 注册 gd32h7xx/gd32h759 |
| `include/zephyr/dt-bindings/clock/gd32h7xx-clocks.h` | RCU 时钟 ID 编码（AHB4 上 GPIO、APB1/2 串口等） |
| `include/zephyr/dt-bindings/reset/gd32h7xx.h` | RCU 复位 ID 编码 |
| `dts/arm/gd/gd32h7xx/gd32h7xx.dtsi` | 公共设备树：RCU/FMC/GPIOA-H/USART0-7/EXTI/SYSCFG，NVIC 优先级 4 位，EXTI num-lines=19 |
| `dts/arm/gd/gd32h7xx/gd32h759xx.dtsi` | 部件级：sram0=0x24000000 832KB AXI SRAM，flash0=0x08000000 3840KB，cpu clock-frequency=600M |
| `drivers/clock_control/clock_control_gd32.c` | H7 分支：AHB1-4/APB1-4/ADDAPB2 时钟频率计算，TIMERSEL 同 F4 |
| `drivers/pinctrl/pinctrl_gd32_af.c` | H7 速度档位断言（12/60/85/100-220MHz ↔ 2/25/50/MAX） |
| `drivers/interrupt_controller/intc_gd32_exti.c` | H7 分组 EXTI 寄存器别名（INTEN0/PD0 等，组 0 覆盖 line 0-21） |
| `modules/hal_gigadevice/{Kconfig,CMakeLists.txt}` | 树内胶水：H7 加 `GD32H73X_75X` 设备宏、misc/pmu 源文件、HXTAL 默认 |

### HAL 模块（`modules/hal/gigadevice/`，独立本地 git 仓库）

官方 `GD32H73x_75x_Firmware_Library_V1.6.0` 整体重命名打包（`gd32h73x_75x_*` → `gd32h7xx_*`），按上游
hal_gigadevice 布局：

```
modules/hal/gigadevice/
├── zephyr/module.yml          # name: hal_gigadevice, cmake-ext/kconfig-ext（用树内胶水）
├── common_include/gd32_*.h    # 统一垫片头（树内驱动 #include <gd32_usart.h> 等）
├── include/dt-bindings/pinctrl/
│   ├── gd32-af.h              # 引脚编码（port[3:0] pin[7:4] af[12:8]）
│   └── gd32h759xx-pinctrl.h   # 本板引脚功能（USART0/UART3/UART4）
└── gd32h7xx/
    ├── cmsis/gd/gd32h7xx/{include,source}/   # gd32h7xx.h、system_gd32h7xx.c、libopt
    └── standard_peripheral/{include,source}/ # 全部外设驱动源
```

### 板级（`apps/boards/gd32h759_btb/`）

board.yml / dts / defconfig / board.cmake / yaml。调试串口 USART0（PA9/PA10，AF7，J13 座），
LED1-3 = PD11/PF7/PD4（低电平点亮），KEY1/2 = PA0/PD7。

## 2. 构建与烧录

```shell
export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-1.0.1
export ZEPHYR_EXTRA_MODULES=$PWD/modules/hal/gigadevice   # 本工作区 HAL 模块（west 未托管）
export PROBE_RS_CHIP_DESCRIPTION_PATH=$HOME/.config/probe-rs/GD32H7xx_Series.yaml  # probe-rs 芯片描述（来自 CMSIS Pack 1.2.0）

west build -b gd32h759_btb zephyr/samples/hello_world -p
probe-rs download --chip GD32H759IM build/zephyr/zephyr.elf   # 烧录（推荐，稳定）
probe-rs verify   --chip GD32H759IM build/zephyr/zephyr.elf   # 校验
probe-rs reset    --chip GD32H759IM                           # 复位运行
# 备选：pyocd flash --target gd32h759im ...（0.45-dev 对本探针偶发 uninit 崩溃，重试即可）
```

调试串口：`/dev/ttyACM0` @ 115200（DAPLink 桥接）。预期输出：

```
*** Booting Zephyr OS build v4.4.0-4-g81723ec3c4d4 ***
Hello World! gd32h759_btb/gd32h759
```

pyocd 探针目标名来自 CMSIS Pack（小写 `gd32h759im`，不带封装后缀）。

## 3. ⚠️ 关键坑：不要写 PMU 供电配置（SWD 会死）

`SystemInit()`（vendor `system_gd32h7xx.c`）里 `#if defined(SEL_PMU_SMPS_MODE)` 会调用
`pmu_smps_ldo_supply_config()` 写 `PMU_CTL2`。**在本板上任何供电模式（SMPS-only / SMPS+LDO
级联）都会把芯片打入调试口无应答状态**：

- 现象：SWD "No ACK"，串口无输出；NRST 拉低无法恢复（PMU_CTL2 在常供电域，跨复位保持），
  **只有彻底断电（SW1 断电重上电）才能恢复**
- 保守验证过程：SMPS-only 和 DVSEN|LDOEN 级联两种模式均复现；调试器挂二分确认死亡发生在
  SystemInit 阶段
- 解决：**不定义 `SEL_PMU_SMPS_MODE`**（`gd32h7xx_libopt.h` 内有注释说明），SystemInit 跳过
  PMU 配置，600MHz 直接用复位默认 LDO 运行，与厂商库模板/例程默认一致

## 4. 时钟树（已实测寄存器验证）

来源：HXTAL 25MHz → PLL0（PSC=5, N=120, P=1 → VCO 600MHz）→ CK_SYS=PLL0P=600MHz
（RCU: SCS=SCSS=PLL0P，PLL0STB=1，HXTALSTB=1）

| 时钟 | 配置 | 频率 |
|------|------|------|
| CK_SYS (PLL0P) | — | 600 MHz |
| CK_AHB | AHBPSC=/2 | 300 MHz |
| CK_APB1 | /2 | 150 MHz |
| CK_APB2 | /1 | 300 MHz（USART0 挂此） |
| CK_APB3/APB4 | /2 | 150 MHz |

端到端佐证：`usart_baudrate_set()` 通过 `rcu_clock_freq_get(CK_USART0)` 实时读 RCU 计算波特率，
115200 干净输出 ⇒ 分频树与实际硬件一致。

## 5. ⚠️ 坑二：shell/console 收数后串口假死（ORE 中断风暴，已修复）

**现象**：shell 输入正常，一旦有突发输入（粘贴、快速敲入）导致 RX 瞬时拥堵，之后串口
彻底无响应（TX/RX 全部冻结），但芯片没死（SysTick 仍在跑）。

**根因**：H7 的 ORE（溢出错误）标志与 RBNE 共享同一个 RX 中断（RBNEIE）。溢出发生后：

1. ORE 置位 → USART0 中断触发；
2. 驱动 ISR 只调用上层回调，shell 后端只检查 `RBNE`（此时=0）不读数据；
3. **ORE 无人清除 → 中断线持续挂起 → ISR 无限重入 → 全系统饿死**。

调试现场：halt 时 IPSR=37（USART0），STAT 的 ORE=1，NVIC 无其他挂起，SysTick 活跃。

**修复**（`zephyr/drivers/serial/usart_gd32.c` 的 `usart_gd32_isr`）：ISR 入口读 STAT，
对 `ORERR/PERR/FERR/NERR` 任一置位时经 `usart_flag_clear()`（写 INTC）清除。

**注意编码陷阱**：`USART_FLAG_*` 是 `(寄存器偏移<<6)|位号` 的编码值（如 ORERR=0x703），
**不能**直接当 STAT 位掩码参与 `&` 运算；判断要用 `USART_STAT_*`（原生 BIT 宏），
清除仍走 `usart_flag_clear()`（内部解码）。

**验证**（`/tmp/shell_repro3.py`）：256 字节突发×20、4096 长行、`help`×50 轰炸、120s 空闲
后输入，四个阶段全部存活（修复前突发阶段即死）。

## 6. ⚠️ 坑三：方向键概率性显示 "["（RX 丢字节，已修复）

**现象**：shell 里按上下方向键翻历史，概率性打印出 "["，历史翻页无效。

**根因**（两层叠加）：

1. **H7 USART 的 FIFO 默认关闭**（`FCS.FEN` bit8=0），RX 只有一级缓冲。方向键的转义序列
   `ESC [ A` 三个字节背靠背到达（87µs/字节 @115200），RX 中断延迟稍大（SysTick、其他临界区）
   就溢出丢字节——丢掉 ESC 后 "[" 被当普通字符显示。
2. **shell 后端环形缓冲默认太小**（TX ring=8B、RX ring=64B）：shell 线程回显长行时被 8 字节
   TX ring 堵住 → RX ring 灌满 → 后续字节直接丢弃，还偶发 shell 线程永久死锁（回显恰好停在
   64 字节处冻结）。

**修复**：

- 驱动 `usart_gd32_init()` 里调用 `usart_fifo_enable()` 启用 H7 FIFO（在 `usart_enable()`
  前，vendor API 会先关 UEN）。驱动使用的 RBNE/TBE/TC 位在 FIFO 模式下语义兼容（同位双义）。
- 应用侧加大环形缓冲（`hello_world/prj.conf`）：
  `CONFIG_SHELL_BACKEND_SERIAL_TX_RING_BUFFER_SIZE=512`、
  `CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE=256`。

**验证**（回显完整性 + 方向键序列，`/tmp/rx_loss_test3.py`）：修复前 125 字符行 30/30 全部
丢字节、方向键 30/60 损坏；修复后 **0/30、0/20**。回显对比时注意剥离 shell 回显中的 ANSI
控制序列和行宽换行。

## 7. 常用外设状态（第二阶段）

| 外设 | 状态 | 验证方式 |
|------|------|----------|
| GPIO | ✅ 可用 | shell `gpio set/get` 控制 LED1（PD11） |
| Flash | ✅ 可用 | 新增 `flash_gd32_v4.c`（H7 FMC：按地址擦除 4KB 扇区、按字编程），shell `flash erase/write/read` 全流程验证 |
| PWM | ✅ 可用 | TIMER0_CH0 → PA8（AF1，扩展排针），shell `pwm usec pwm 0 1000 500` 设置后 gpio get 采样翻转 |
| DMA | ✅ 就绪 | dma0（8 通道）节点使能，驱动 READY；等 SPI/ADC 等消费者接入后做传输验证 |
| SPI | ⚠️ 节点就绪 | dtsi 定义 spi0/1/2；BTB 板引脚 AF 未获权威数据（例程无 SPI2@PC10-12 覆盖），板级使能待引脚分配表 |
| I2C | ⚠️ 节点就绪 | dtsi 定义 i2c0/1；同上，板上无硬件 I2C 焊盘（野火触摸为软件 I2C PH7/PH8） |

第二阶段新增/改动：

- `drivers/flash/flash_gd32_v4.c` + `dts/bindings/mtd/gd,gd32-nv-flash-v4.yaml`：
  H7 FMC 后端（960 × 4KB 均匀扇区，`fmc_sector_erase` 按地址擦除，字编程带 DSb/ISb 屏障），
  Kconfig/CMakeLists 注册 `GD32_NV_FLASH_V4`
- `dts/arm/gd/gd32h7xx/gd32h7xx.dtsi`：dma0、timer0-2（含 pwm 子节点）、spi0-2、i2c0-1 节点；
  flash0 切换到 `gd,gd32-nv-flash-v4`
- `drivers/dma/dma_gd32.c`：H7 分支（INTF0/INTF1 双标志寄存器兜底宏、SDEIE/TAEIE 错误中断映射、
  DMA_CHMADDR → DMA_CHM0ADDR 别名）
- 时钟/复位绑定头：补 TIMER0-6、SPI0-2、I2C0-3、DMA0/1 的 RCU 位
- 板级：dma0/timer0-pwm 使能（PWM 输出 PA8），defconfig 打开 FLASH/DMA/PWM 子系统，
  prj.conf 打开 `PWM_SHELL`/`FLASH_SHELL`

> **FIFO 说明**：H7 USART 的 RX/TX FIFO 已在驱动 init 中使能（`usart_fifo_enable`），
> 否则单字节缓冲扛不住 87µs/字节的连续输入（方向键转义序列丢字节）。

**PWM 占空比极性**：驱动使用 TIMER_OC_MODE_PWM1，占空比 100% 时引脚为低、0% 为高
（与上游行为一致）；需要常规极性可在 DT 设 `PWM_POLARITY_INVERTED`。

## 7.5 ADC 与 CAN（第三阶段）

### ADC ✅ 可用并已板上验证

- H7 的 ADC 为新一代 IP：采样时间编码进 RSQ 序列条目（10 位、无独立 SAMPT 寄存器）、
  通道配置函数改名 `adc_routine_channel_config`、时钟分频在 `ADC_SYNCCTL.ADCSCK`
  （同步模式，0xB = HCLK/8 = 37.5MHz @600MHz，ADC0/1 上限 72MHz）、转换前需校准
  （RSTCLB/CLB 流程，驱动已有实现兼容）
- `adc_gd32.c` 增加分支：SYNCCTL 分频初始化、采样表（原始周期数）、RSQ 条目携带采样时间、
  H7 输入走专用 `_C` 模拟焊盘无需 pinctrl（绑定中 pinctrl 改为可选，init 跳过引脚配置——
  否则 `pinctrl_lookup_state` 返回负值导致设备 DISABLED）
- dtsi：adc0/1/2（0x40012400/2800/2C00，IRQ 18/18/127，APB2EN/RST 位 8/9/10）
- 板上验证：`adc adc@40012400 resolution 12` + `read 0` 读 PA0_C 电位器（注意先设分辨率，
  shell 默认 resolution=0 会被驱动 -EINVAL 拒绝且无输出）

### CAN ✅ 驱动可用并已回环验证（经典 CAN + CAN-FD 64 字节，@1Mbps/5Mbps）

- 新驱动 `drivers/can/can_gd32fd.c`（`gd,gd32-can` 绑定 + `CONFIG_CAN_GD32FD`，select
  `CAN_FD_MODE`）：
  MB0=TX、MB1=RX（全收）、Message 中断驱动（IRQ 180/187/194）、软件过滤（最多
  `CONFIG_CAN_GD32FD_MAX_FILTER`=8 条）、normal/loopback/listenonly/**fd** 四模式、
  start/stop/set_mode/set_timing/**set_timing_data** 完整 Zephyr CAN API
- **CAN-FD 支持**（`can mode <dev> fd` + `can send <dev> -f -b ...`）：
  - `can_fd_config()` 在 start() 的 INACTIVE 窗口里配置：ISO FD、BRSEN、
    **MDSZ=64 字节邮箱**、TDC（offset=数据段采样点位置 tq 数）、FDBT 数据段时序
  - FDEN 置位后**消息 RAM 邮箱步长变为 payload+8**（厂商 `can_ram_address_get` 自动算，
    但自己手搓地址时要小心）；非 FD 模式显式清 FDEN
  - DLC 转换：TX 用 `data_bytes`（厂商库自动算 DLC 码）；RX 描述符 `data_bytes` 已是
    字节数，Zephyr 帧要用 `can_bytes_to_dlc()` 回填 dlc 码（DLC 9-15 ↔ 12/16/20/24/32/48/64B）
  - 数据段时序上下限（CAN_FDBT）：sjw 1-8、DPTS 0-31、DPBS1 1-8、DPBS2 2-8、
    DBAUDPSC 1-1024；5Mbps@300MHz 可解（如 prescaler=2、30tq）
  - shell 发 64 字节帧需 `CONFIG_SHELL_ARGC_MAX=80`（69 个参数，默认 12 不够）；
    参数顺序：`can send <dev> -f -b <id> b0 ... b63`（选项在设备名后）
- 板级：can2 okay（PD13 TX/PD12 RX AF5，接 SIT1042AQT 收发器 + J8 端子）、
  bitrate 1M + bitrate-data 5M + can-transceiver max-bitrate 5M；
  测试应用 `apps/applications/can-loopback`（经典回环 + FD 64 字节回环自测 + normal + CAN_SHELL）
- 验证：
  - FD 回环：shell `can send ... 456 <64字节>` → `B- 456 [64] 00..3f` 全字节回环一致
    （FDF+BRS、DLC 15）；启动自测 last_rx（调试器读）id=0x456/dlc=0xf/flags=0xc 同样正确
  - 经典回归：板→Linux（candump 收 `123 [8] DE AD...`）、Linux→板（`cansend` 后 shell
    过滤器打印 `123 [8] 11 22...`）双向 OK
  - **注意**：当前 Linux 侧 PCAN-USB（0c72:000c，`pcan_usb` 驱动，maxmtu 16）为经典版，
    **不支持 CAN-FD**；FD 与外部互通需 PCAN-USB FD / Pro FD 等适配器
    （`ip link set can0 type can fd on` 能成功即为支持）

- **CAN 基地址**：APB2 基准！CAN0=0x4001A000、CAN1=0x4001B000、CAN2=0x4001C000
  （CAN_BASE=APB2+0xA000；别按 APB1 算成 0x4000A000——寄存器读全 0 的元凶）
- **时序**：CAN 时钟源 CK_APB2=300MHz；1Mbps 默认参数 prescaler=30、prop=2、seg1=5、
  seg2=2（10tq、样本点 80%，厂商例程同款）；位域均"实际值-1"写入
- **全收过滤的关键坑**（该 IP 特有）：
  1. 过滤掩码寄存器（公有 RMPUBF/私有 MPFx）复位值**随机**（RAM 区），位=1 参与 ID 比较 →
     不配置就是随机精确匹配，回环只偶合例程 id
  2. 这些寄存器**仅暂停（INACTIVE/HALT）模式可写**，`can_init` 的 mb_public_filter 参数
     实际写不进去 → start() 里先进 INACTIVE、显式 `RMPUBF=0`（全不关心）+ `MPF1=0` 再进目标模式
- **驱动流程要点**：start() 每次完整重跑 can_init→(INACTIVE 写过滤+FD 配置)→进模式→
  装 RX 邮箱→开中断（厂商 IP 要求一次性序列，分阶段配置不生效）；ISR/过滤锁必须用
  spinlock（中断上下文）；RX 邮箱描述符的 data 指针必须指向真实缓冲（NULL 解引用→BusFault）

- H7 的 CAN 是**新一代 CAN-FD 控制器**（非 bxCAN）：32 邮箱消息 RAM、6 级 RX FIFO
  （支持 DMA）、公有+私有过滤器（104 扩展 ID/208 标准 ID）、CAN FD 数据段最高 8Mbps、
  四种模式（正常/暂停/回环静默/监听），FlexCAN 类架构
- 已完成：dtsi can0/1/2 节点（7 个命名中断：wkup/message/busoff/error/fasterror/tec/rec，
  CAN0=179-185、CAN1=186-192、CAN2=193-199）、`gd,gd32-can` 绑定、时钟/复位 ID
  （ADDAPB2EN/ADDAPB2RST bit 0-2，CAN 时钟源 CFG1 CANxSEL：HXTAL/APB2/...）、
  板级 pinctrl 宏（CAN2 TX=PD13/RX=PD12，AF5）

## 8. 已知待办 / 后续

- [ ] flash 驱动：H7 FMC 需新的 `flash_gd32_v4` 变体（当前 flash 节点为普通 `soc-nv-flash`，
      Zephyr 的 GD32 flash 驱动未启用）
- [ ] Cache（I/D-Cache）与 DWT 实测：当前 caches 未启用（`CONFIG_CACHE_MANAGEMENT` 未开）
- [ ] GPIO J/K 端口（H7 最多到 PK，当前设备树只挂了 A-H，本板 BGA176 用不到 J/K）
- [ ] 其余外设驱动逐个启用：SPI/I2C/TIMER/PWM/DMA/ADC/TRNG（时钟 ID 已在 `gd32h7xx-clocks.h`
      预置了 TIMER0-6/SPI0/I2C0/1）
- [ ] SWD 偶发掉线：HW-Link_LITE 探针 + pyocd 0.45-dev 组合下烧录偶发 `index out of range`
      崩溃，重试即可；必要时降低 `--frequency`
