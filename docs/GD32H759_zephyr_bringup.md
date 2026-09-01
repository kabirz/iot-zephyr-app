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
  测试应用 `apps/examples/can-loopback`（经典回环 + FD 64 字节回环自测 + normal + CAN_SHELL）
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

## 7.6 OSPI NOR Flash ✅ 可用并已板上验证（GD25Q64 8MB，间接模式 1-1-1）

- 板载 U7 **GD25Q64ESIGR（64Mbit=8MB）挂 OSPI0**（OCTOSPI 类控制器，AHB3），
  不是普通 SPI——OSPI0=0x52005000、OSPI1=0x5200A000、OSPIM=0x5200B400；
  时钟 AHB3EN bit4(OSPIM)/5(OSPI0)/6(OSPI1)
- **引脚 AF 号（数据手册 AF 表 + EVAL 例程锚点校准）**：
  SCK=PA3 **AF12**、CSN=PB10 **AF9**、IO0=PF8 **AF10**、IO1=PF9 **AF10**、
  IO2=PE2 **AF9**、IO3=PA6 **AF6**。每个引脚的 OSPI AF 都不一样！
  （校准法：EVAL 例程已证实 PB2=SCK@AF9、PB6=CSN@AF10，与目标引脚同页
  的 AF 表用这两个作列锚点定列边缘）
- **必须配置 OSPIM 互联**：时钟开启后要 `ospim_port_sck/csn/io3_0_config()`
  使能端口 0 信号 + `*_source_select()` 选 OSPI0 源，否则引脚完全不通
- 新驱动 `drivers/flash/flash_gd32_ospi.c`（`gd,gd32-ospi` 绑定 +
  `CONFIG_FLASH_GD32_OSPI`，select USE_GD32_OSPI）：间接模式标准 SPI 命令
  （WREN 06 / READ 03 / PP 02 / SE 20 / RDSR 05 / RDID 9F），借厂商
  `ospi_command_config`+`ospi_transmit/receive`；节点属性 size + jedec-id；
  init 读 JEDEC 校验（无响应→ENODEV，不匹配→WARN 继续）；4KB 扇区布局
- prescaler=9（内核时钟/10，EVAL 同款保守值），GD25Q64 远未跑满
- 验证：`jedec id: c8 40 17` ✓；4KB 擦除+4096 字节写入+回读全对 ✓
  （测试应用 `apps/examples/ospi-flash-test`，含 `flashid`/`flashtest`/
  `mmtest`/`mpudump` shell 命令）

### Memory-Mapped（XIP 窗口）✅ 可用，但必须非缓存属性

- OSPI0 MM 窗口 = **0x90000000**（OSPI1=0x70000000），驱动 API
  `flash_gd32_ospi_mm_enable()/mm_disable()`（include/zephyr/drivers/flash/gd32_ospi.h）：
  首次使能会置 flash QE 位（SR2 bit1，0x35/0x31）并把 MM 读命令配成
  **quad fast read 0x6B**（1-1-4+8 dummy）；擦/写等 flash API 调用会自动切回
  间接模式，之后可再切回 MM。板级 dts 在 reserved-memory 声明窗口
  （`ospi0-mm@90000000`，含 linker region OSPI0_MM）
- **⚠️ 大坑一：D-Cache 行填充会挂死总线**。窗口若为可缓存属性（默认映射
  0x80000000-0x9FFFFFFF 就是 WT-cacheable），M7 读未命中会产生 **WRAP 突发
  行填充**，OSPI 的 AHB 从机不应答 → 单字节读都能把系统挂死（lockup，SWD
  halt 都停不住，只能 reset halt）。**必须** 用 `zephyr,memory-attr =
  <(DT_MEM_ARM(ATTR_MPU_RAM_NOCACHE) | DT_MEM_NON_VOLATILE)>` 把窗口编成
  非缓存 MPU 区域（需 `CONFIG_MEM_ATTR=y`；注意**没有 compatible 的节点
  不会生成属性宏**，必须 `compatible = "zephyr,memory-region"` + 必需的
  `zephyr,memory-region = "OSPI0_MM"` 字符串属性）
- **⚠️ 怪癖二：`CONFIG_CACHE_MANAGEMENT=y`（哪怕从不启用 cache）影响间接
  读速度 8 倍**：有=y → 3582 KB/s，=n → 438 KB/s（同代码路径，A/B 三轮
  复现；cache 驱动 init 为空，疑为代码布局/取指对齐次生效应，待查）。
  测试应用保持 =y
- **⚠️ 怪癖三：开启 D-Cache 后间接 FIFO 轮询路径也慢 8 倍**（439 KB/s），
  机理未明——所以当前配置 dcache 保持关闭
- 实测（64KB，SCK=内核/10）：**间接读 3582 KB/s**（≈1-1-1 30MHz 线速上限），
  **MM 指针读 590 KB/s**（非缓存单拍，每笔访问都带完整命令+地址+dummy 开销），
  数据逐字节一致。**结论：大批量读用 flash_read()（间接流式）；MM 适合随机
  小读/零拷贝/不用驱动直接寻址**。要把 MM 提速需要 cache 行填充突发支持
  （本 IP 似不支持，见坑一）
- 后续优化：间接读换成 0x6B quad（QE 已置位）可到 ~12MB/s；prescaler 降到
  1/2 可再翻倍（GD25Q64 支持 104MHz+ SCK）

- 注意：普通 SPI（SPI0-2，`spi_gd32.c`）是另一组控制器，板上无外部器件；
  SPI2 可从 BTB 排针引出（PC10/PC11/PC12/PA15），待需要时再启用验证

## 7.7 以太网（ENET0 + LAN8720A RMII）⚠️ 驱动可用、连通已验证，吞吐/延迟待调

新增文件：`drivers/ethernet/eth_gd32.c`（寄存器级驱动，不用厂商库全局描述符）、
`drivers/ethernet/Kconfig.gd32`、`dts/bindings/ethernet/gd,gd32-eth.yaml`；
配套：`drivers/entropy/entropy_gd32_h7.c`（H7 TRNG，网络栈随机数必需）、
dtsi 新增 `eth0@40028000`/`eth1@4002a000`/`trng@48021800` 节点、
clock/reset ID（ENET0/TX/RX、TRNG）、pinctrl 引脚（RMII 全套 AF11 + CK_OUT0_PA8 AF0）、
HAL shim `gd32_enet.h`。

- **硬件**：LAN8720A PHY（MDIO 地址 0，ID 0x0007c0f1），MCU PA8 输出 50MHz
  （RCU_CFG2：CKOUT0SEL=PLL0P、CKOUT0DIV=12 → 600/12=50MHz）→ PHY CLKIN，
  REF_CLK 回 PA1；PF6 复位 PHY（驱动先开 CKOUT0 再复位，保证 strap 锁存正确）；
  RMII 接口选择在 **SYSCFG_PMCFG bit23**（不在 MAC_CFG！）
- **已验证**：PHY 自动扫描（0-31 扫 ID）、自协商、链路 100M 全双工、
  ARP/ping/TCP echo 端到端全通（主机 enp3s0=192.168.12.100/24，
  板子静态 192.168.12.200/24，示例 `apps/examples/eth-echo`，TCP 4242 回显）
- **踩过的坑（都已修复进驱动）**：
  1. **TX 描述符必须带 TERM（end of ring）**，且每次重武装都要带——否则 DMA
     消费完一帧就走出环停在哪算哪（CTDADDR=TDTADDR+0x10 的野地址），后续全超时
  2. **ISR 里 TBU 不要无条件 TPEN**——空链表时 poll demand 会立刻再挂 TBU，
     ISR 死循环饿死整个系统（现象：shell 无响应、IRQ 一直 pending）
  3. **这颗 MAC 的 RX 不剥 FCS**（APCD 对 RX 无效），驱动软件剥尾 4 字节
  4. **ET/异常中断源要全清**，漏清一个（如 ET）就会中断风暴
  5. **网络栈需要熵**：`CONFIG_ENTROPY_GENERATOR=y` + chosen `zephyr,entropy` +
     H7 TRNG 驱动（F4 版驱动 API 不通用）
- **诊断技巧**：RSTSTS（RCU+0x74）看复位原因；DEMCR=0x400 开 hard-fault 向量捕获；
  attach 模式 pyocd 会话可不停机读内存；`verify` 必做——**HW-Link_LITE 烧写
  静默失败概率不低，未 verify 的"成功"烧录会浪费一轮测量**
- **⚠️ 延迟根因（两阶段定位，均已修复）**：
  - **第一阶段（SRAM0 + 轮询规避）**：初版驱动 ping RTT 3-6s 且随包间隔缩放。
    厂商 ENET_LWIP 例程（cmake+gcc 交叉编译烧同板）RTT 0.13-0.5ms → 硬件无问题。
    对齐厂商配置（描述符/缓冲放 **SRAM0 0x30000000**、chain 描述符、存储转发）后
    仍滞后，当时改为 `INTEN=0` 纯轮询规避，RTT 4-10ms。
  - **第二阶段（风暴根因 = TBU）**：借 `gdeth` 诊断 shell（`CONFIG_ETH_GD32_IRQ_TEST`，
    运行时改 INTEN + ISR 按位计数）+ pyocd 挂起采样 **IPSR=77（IRQ61）实锤 CPU 卡死在
    ENET ISR 内循环**；挂起态读 `DMA_STAT=0x00670404`：置位的只有 **TBU(bit2)+ET**，
    **NI/AI 汇总位都没置**。结论：这颗 IP 的中断线是 **(STAT & INTEN) 逐位相或**，
    汇总位只是摆设不是闸门；且 **TBU 属于 NORMAL 汇总**（STM32F4 语义里是 abnormal，
    惯性思维坑）。单描述符 TX 链表每发完一帧 DMA 必报一次 TBU（本 IP 的正常行为），
    旧 ISR 只在 AI 分支清 TBU → AI 永不置位 → **TBU 永远清不掉 → 中断线永久挂起 →
    ISR 无限重入**。修复：ISR 对每个已使能源**无条件清除**（TBU 只清不发 TPEN），
    MSC/WUM/TST 镜像位去 MAC/MSC 源寄存器清。修复后中断驱动全开：
    200 ping 全收 0 丢包、每 ping 恰好 RS+TS/TBU/ET（计数完全对称）、residual=0。
  - **附加发现**：MSC 统计中断源复位默认不屏蔽（RINTMSK 复位值 0x20060），
    驱动 start 时用 RMW 全部屏蔽；MSC/MAC 标志寄存器疑似只认单 bit 写清除
    （厂商库 `enet_interrupt_flag_clear` 也是单 bit 写）。
- **SRAM0 放置（修正）**：**描述符**必须在 0x30000000 外设 SRAM（`section("SRAM0")`），
  但**帧缓冲不必**——当年"缓冲也必须 SRAM0"的结论实为 TBU 风暴的误诊：D-cache 开启后
  只要做缓存维护（`CONFIG_CACHE_MANAGEMENT=y`，驱动 RX invalidate/TX flush），
  缓冲放普通 RAM 即可，16KB SRAM0 限制解除后 RX 环可扩到 32 描述符
- **Zephyr 网络栈 TCP 调优（与驱动无关但很坑）**：
  1. `NET_BUF_DATA_SIZE` 默认 128B → 1460B 段要 12 个 buf，RX 池 36 个只够 3 段
     在途，**≥4KB 传输必卡**（表象与驱动丢包一模一样！）。调 512B/1536B + 池扩容
  2. **echo 类应用板端要关 Nagle**（`TCP_NODELAY`），否则逐段等 ACK 串行化（~20ms/段）
  3. **收发双向大流量会把 Zephyr 栈缓冲池耗尽**（板端 send 返回 ENOBUFS=105 主动断连，
     主机表象为固定字节处断开），且半死连接会把单线程 echo 服务卡住——测试客户端
     一定要干净关闭、测前复位板子
  4. RX 线程优先级必须高于栈线程（否则环溢出丢 ACK），但**不能只高不睡**——
     突发时逐帧循环永不阻塞会把栈饿死（0.06Mbps）。驱动加了 `ETH_GD32_RX_BATCH`
     （默认 8）限批排水。**注意 `k_yield()` 救不了饿死**：它只让给同优先级线程，
     prio 4 的排水循环期间 prio 5 的栈依然拿不到 CPU；必须靠阻塞（信号量）让出
- **⚠️ 坑六（性能根因）：从未使能 I-cache，全系统慢 ~10 倍**。初期实测
  ping RTT 4-5ms、TCP 只有 ~2Mbit/s，一度误判为"Zephyr 栈每包 ~4ms 成本的
  天花板"——**错误结论**。自包含分段计时插装（`gdeth stat` 的
  alloc/copy/recv 字段）发现 `net_pkt` 池分配要 368µs、98B 拷贝 3.4µs/字节、
  ICMP echo 构造 1.35ms——全是代码密集路径；pyocd 采样 78% 落在
  `arch_cpu_idle`（CPU 闲着）+ 分段总和超过 RTT（TC 线程内联抢占）→ 定位到
  **600MHz M7 无缓存跑 flash 取指全在吃 wait states**（此前 CCR 读出 cache
  一直是关的；厂商 BSP 明确使能 I/D-cache）。驱动 init 里使能 I-cache 后
  （raw PPB 写 `ICIALLU`/`CCR.IC`，CMSIS 头在该 TU 不可靠）：
  **ping 4.7ms→0.6ms、TCP 下行 2.0→17.3Mbit/s、UDP 上行 2.0→15.2Mbit/s，
  全部 ~8 倍提升**，进入厂商 lwIP 同区间
- **D-cache 实验失败 x3（已永久关闭）**：① 逐描述符缓存维护（RX 读前
  invalidate/写回 flush、TX 武装后 flush、start() 全环 flush）→ 链路建立后
  致命复位循环；② 驱动手写 MPU 非缓存区域（扫描空闲区域号 + D-cache）→
  同样复位循环；③ **正规方案**（`soc_mpu_regions.c` 经
  `CPU_HAS_CUSTOM_FIXED_SOC_MPU_REGIONS` 走 arm_mpu 框架声明 0x30000000
  32KB 非缓存静态区域 + `SYS_INIT(POST_KERNEL)` 在 MPU 编程后开 I/D-cache）
  → **直接硬砖**：SWD 无法连接（WAIT ACK），须 BOOT0 恢复。
  ③ 的失败推翻了"MPU 区域被运行时覆盖"假设——框架区域确认被编程后仍砖。
  ④ `CONFIG_MPU_STACK_GUARD=n` + 运行时开关（`gdeth dcache on`）：
  **空闲完美存活，ping 0.231ms**（达到厂商延迟水平！），但突发流量下
  渐进性死亡（先串口/printk 冻结，后网络）——证明栈保护的动态 MPU 改写
  是 lockup 的一个根因，但不是全部；⑤ 厂商内存模型完整移植（描述符+TX
  缓冲 SRAM0、RX 缓冲 SRAM1、全 DMA 流量进非缓存窗口、零缓存维护、
  双缓存启动使能）→ 启动后 2 秒内同样静默死亡。
  **八轮实验结论：D-cache 只要系统有真实活动（初始化风暴/网络突发）就会
  在数秒内静默死亡，与内存模型/MPU 配置/使能时机/维护策略无关；空闲时
  完美（0.23ms ping 证明潜力）。根因在本驱动配置之外**（硅 errata /
  fork 架构层 / 未知前置条件），需专门调查（J-Link 跟踪、逐优先级心跳
  转储），不再以换配置方式尝试。**以 I-cache-only（ping 0.63ms /
  TCP 20.1Mbit/s / 全零丢包）为交付基线**
- **TCP 窗口结论**：`NET_TCP_MAX_*_WINDOW_SIZE` 用默认 0（按缓冲池自动
  计算）最优——实测 18.8 Mbit/s；写死 16K=17.3，写死 64K 反而崩到 6.7
  （0 丢包 0 重传，纯窗口抖动病理，无 window scale 时 64K 上限也无意义）
- **缓存 A/B/C 定标矩阵（厂商固件同协议实测）**：
  | 配置 | ping 56B | TCP 下行 |
  |---|---|---|
  | I+D 全开 | 0.204ms | 63.88 Mbit/s |
  | 仅 I-cache | 0.204ms | 63.88 Mbit/s |
  | 双关 | 1.17ms | 14.71 Mbit/s |
  结论：**I-cache 是 600MHz 下必需品**（无它 lwIP 也掉 4.3 倍，证实 Zephyr
  侧 8-10 倍惩罚为真且方向一致）；**D-cache 对网络吞吐贡献为零**（推翻
  "Zephyr 差距主因是 D-cache"的归因）。**Zephyr(20.1) vs 厂商(63.9) 的
  3.2 倍差距在两者都有 I-cache 的公平对比下 = 100% 协议栈/驱动架构成本**
  （lwIP raw 轮询 vs Zephyr net_pkt/线程/socket 层）。Zephyr 侧常规优化
  （零拷贝 RX、硬件校验和卸载、缓冲精调）预期 30-40 Mbit/s；63.8 需要
  lwIP raw 级栈架构。
- **同硬件厂商固件对比（定标）**：厂商 ENET_LWIP 例程（gcc 交叉编译 + 我加的
  sink 端点，其余保持原版：I/D-cache + MPU 非缓存 SRAM + lwIP 原版参数——
  窗口仅 2×MSS、堆 15KB 在非缓存 SRAM1）：**ping 0.13ms、TCP 下行 63.8 Mbit/s**
  （8MB 零丢失，板端精确对账）。即本板硬件上限 ~64 Mbit/s，Zephyr 当前 20.1
  为其 1/3；差距主因 **D-cache 未开**（厂商靠 MPU 分区后全速开），次因
  Zephyr 栈逐包成本（线程/锁/net_pkt vs lwIP raw 轮询）。驱动 DMA 层不是
  瓶颈（同一套 MAC+描述符，厂商能到 63.8）。
  ⚠️ 厂商 lwIP 堆必须待在 0x30004000 非缓存 SRAM1——挪到缓存 .bss 立即
  DMA 一致性崩溃（教科书案例：靠内存分区而非缓存维护解决一致性）
- **带宽实测汇总**（I-cache 使能后；bench 工具：`apps/examples/eth-echo/
  src/bwtest.c` TCP 双模式端点 + `src/bench_shell.c` UDP 基准 shell，
  分别移植自 io-edge-hub 和 n2e-gw）：
  - **ICMP**：56B RTT 0.65-1.5ms / 1400B 0.9-1.7ms，0 丢包
  - **TCP 下行（sink 模式，板只收）**：**18.8 Mbit/s**（2.24 MB/s），8MB 零断连
  - **TCP 双向 echo**：**7.1 Mbit/s 单向**，8MB 零丢失 0 重传
    （⚠️ echo 应用线程必须低于栈优先级：prio 0 的 main 线程收发死循环会把栈
    饿死、TX 池耗尽 send 返回 ENOBUFS 断连；bwtest 用 prio 15 跑 8MB 稳定）
  - **UDP 上行（`bench tx`）**：1024B **1872 pps（15.3 Mbit/s）**，1000 包
    0 丢包 0 乱序（seq 校验）
  - **UDP 下行（`bench rx` 限速）**：1000pps 输入实收 787pps（6.4 Mbit/s）；
    线速 94Mbps 洪泛仍接近全丢（栈 UDP 投递层饱和，非驱动）
  - 结论：驱动层（0 丢包 0 重传）不是瓶颈；当前上限是栈逐包处理 + 单核
    软中断模型，D-cache/MPU 是下一个提升点
- **⚠️ NAPI 式 RX 中断屏蔽实验（已否决，数据留档）**：曾把 ISR 改成 RS 时屏蔽
  RIE/RBUIE、RX 线程排空后重挂（每批一次中断而非每帧一次）。洪泛时中断数确实
  大降（RBU 4.2万→607），但 **大帧延迟恒定 +10ms**（1400B ping 4.9→14.9ms，
  线性 ~7.4µs/字节，与帧长成正比）、TCP echo 吞吐减半（0.84→0.35Mbps）。
  A/B 复测确认元凶就是 **RS 时写 INTEN 的 RMW**（本 IP 上该写法会拖慢 DMA 管线，
  机理未深究）。最终驱动**不做 RS 屏蔽**，保留每帧一次 RS + **RBU 连发退避**
  （连续 16 次 RBU 无 RS 则临时屏蔽 RBUIE，排水后重挂）：洪泛中断 4.2万→5千，
  延迟/吞吐与无退避版持平。`gdeth stat`（`CONFIG_ETH_GD32_IRQ_TEST`）可查
  `isr->thread` 唤醒延迟和 `copy_cyc` 每字节拷贝成本（实测 ~55 cyc/byte）

## 7.8 USB（USBHS0 CDC ACM）✅ 可用并已板上验证（设备枚举 + 回环）

### 硬件与资源

- USBHS0 @ `0x40040000`，IRQ 77（global）；DWC2 OTG 核（GLOBAL/HOST/DEVICE/PWRCLK 四段布局，
  同 STM32H7 OTG_HS）。LQFP144 上 `USBHS0_DM/DP` = pin 130/131（数据手册 Rev1.6，
  USB 专用脚不需要 GPIO AF 配置）；板上 OTG 座接 USBHS0（已实测确认）
- **GD32 没有实现 GHWCFG1/2/4 只读 ID 寄存器（读出恒 0，SVD 也不列）**——Zephyr 的
  `usb_dc_dw` 驱动依赖 GHWCFG4 判专用 FIFO 模式、GHWCFG2 数端点，为此新增
  `gd,gd32-usbhs` 绑定 + DTS 静态端点参数（6 IN / 4 OUT，SVD 的 DIEP0..5TFLEN 佐证），
  驱动内按 compat 回退到 DT 值

### 上电顺序（照 GigaDevice V1.6.0 USBHS 库，落在 `usb_dc_dw_gd32.h` quirk）

1. RCU 开 PMU 时钟，PMU_CTL2 置 `USBSEN|VUSB33DEN`，**等 USB33RF**（PMU @
   `0x58005800`，注意 GD32 的 APB4 基址是 `0x58000000` 而非 STM32H7 的 `0x48000000`——
   写错总线段=总线错误整板挂死，实测踩坑）
2. IRC48M 起振（ADDCTL0.16/17），USBCLKCTL 的 USBHS048MSEL(bits5-6)=3 选 IRC48M
3. AHB1EN bit14 开 USBHS0 总线时钟
4. GUSBCS.6（EMBPHY_FS）选片上 FS PHY，**必须在核软复位之前**（复位握手需要核时钟）
5. pwr_on 钩子：清 GOTGINTF 挂起、PWRCLKCTL(0xE00)=0 重启 PHY 时钟、
   GOTGCS.6/7（GD32 的 BVOE/BVOV 位，**不是 STM32 的位位置**）强置 B-session valid、
   GCCFG(0x38).16 PWRON 给收发器上电

### 关键坑（都已修复在提交 87f6a63c94e）

- **DevSpd 必须写 1 不是 3**：这是带片上 FS PHY 的 HS 核，厂商库 FS 模式用
  `DCFG_DS=1`；Zephyr 通用驱动写 3（FS 核语义）会导致 EP0 对主机请求完全无响应
  （主机 `device descriptor read error -110`）
- **总线复位时必须冲刷 RX/TX FIFO**：主机放弃控制传输后会复位总线，此时 IN FIFO
  残留数据永远不会排空；下一次 SETUP 到来时 `usb_dw_tx()` 在 ISR 里等 FIFO 清空，
  `k_yield()` 在 ISR 中不能阻塞 → CPU 活锁 → console/整机冻死（可复现，与
  ENET TBU 风暴同症状）。修复：`usb_dw_handle_reset()` 里 RXFFLSH + TXFFLSH
  （TXFNUM=0x10 全部），另给 `usb_dw_tx()` 的 FIFO 等待加了 10000 次护栏返回 -EIO
- 设备域寄存器是标准 DCFG@0x800/DCTL@0x804/DSTAT@0x808（与厂商结构体一致），
  调试工具用错偏移（STM32 旧式 0x80C/0x810）会得出"寄存器写不进"的错误结论

### 实测结果（device_next / USBD 栈，当前方案）

> legacy `usb/device` 栈（`usb_dc_dw` + 提交 87f6a63c94e 的适配）曾先行打通并验证，
> 但该栈已废弃、Zephyr 4.5 将移除；现行方案为 `usb/device_next`（USBD/UDC），
> 见提交 2abd830a435。legacy 驱动适配保留在树中供其他板使用。

- 主机 xhci 枚举：`28e9:0575` "EmbedFire GD32H759 BTB CDC ACM"（序列号来自 hwinfo UID），
  `cdc_acm` 绑定出 `/dev/ttyACM1`
- DTR 握手 + 回环：16 × 1040B 往返零丢失
- 示例：`apps/examples/usb-cdc-acm`（USBD_DEVICE_DEFINE + FS 配置 + 中断驱动回环）

### device_next（UDC DWC2 驱动）额外踩的坑（都已修复在提交 2abd830a435）

- **GHWCFGn 硬件读零时回退 DT 值**：UDC 驱动运行时大量读 GHWCFG1-4（端点方向/DMA/
  FIFO 模式/深度），全零会直接 -ENOTSUP；绑定加了 `ghwcfg3` 可选属性 + 驱动回退逻辑，
  dtsi 写入推导值（0x500 / 0x1506 / 0x04000024 / 0x16200000）
- **GHWCFG2 位算错一位 = 无上拉**：0x1566 里 bit6 使 HSPHYTYPE=UTMI+，驱动走 HS 分支
  时 `PHYSEL_USB20` 会主动清 bit6（EMBPHY_FS）→ FS PHY 关闭 → 主机完全看不到设备。
  正确值 0x1506（FSPHYTYPE=1、HSPHYTYPE=0）
- **PWRCLKCTL 必须在控制器 init 前清零**（quirk pre_enable）：否则端点激活路径里的
  FIFO 冲刷会永久挂起（等 RXFFLSH 自清）
- **RXFFLSH/TXFFLSH 不自清**：两个冲刷等待都加了 1000µs 有界等待（FIFO 本来为空，
  冲刷未完成无碍，legacy 同样处理）
- **TRDT(USBTNR) 被通用 init 清零**：`udc_dwc2_init_controller` 整体重写 GUSBCS，
  厂商库用 `|=` 保留复位默认值 TRDT=5；quirk post_enable 恢复，否则主机报
  `error -71`（EPROTO）
- **INEPNAKEFF 是电平黏滞位**：bus reset 时驱动把 DIEPINT_INEPNAKEFF 开进 diepmsk，
  而 EP0 空闲 NAK 生效是常态 → GINTSTS.IEPINT 永久置位 → ISR 活锁（整机冻死，
  GDB 抓到 PC 卡在 `udc_dwc2_isr_handler` 读 DAINT 循环）。修复：diepmsk 去掉
  INEPNAKEFF（厂商库只开 XFERCOMPL），IEPINT/OEPINT 处理改为清全部挂起位
- **调试工具**：probe-rs 的 gdb stub 只能读 RAM/Flash 不能读外设；pyocd 停机后可以
  （`pyocd cmd -t gd32h759im -c halt -c "read32 <addr>"`）。SWD 楔死（WAIT response）
  时用 `pyocd cmd -t gd32h759im --connect under-reset -c quit` 恢复


## 8. 已知待办 / 后续

- [x] 以太网：ENET 中断风暴根因已定位（TBU 属 NORMAL 汇总且未被 ISR 清除，
      中断线 = STAT & INTEN 逐位相或，汇总位不是闸门），已修复并恢复中断驱动
- [x] 以太网：终版驱动实测（中断驱动 + 每帧 RS + RBU 退避 + 批处理排水 prio 4
      + 32 描述符 + 缓冲出 SRAM0 + CACHE_MANAGEMENT + 1536B net_buf）：
      ping 56B 4.2ms / 1400B 5.2ms 均 0 丢包；TCP echo 64KB 0.84Mbps；
      洪泛中断 5k/5s。NAPI 屏蔽方案已否决（见上文）
- [ ] 以太网：TCP echo 256KB+ 在板端 TX 池耗尽处（49 个未 ACK 段）板端 send
      返回 ENOBUFS 主动断连——Zephyr 栈 send 不阻塞的问题，待应用层重试或
      `NET_PKT_TX_COUNT` 加大验证
- [x] Cache：`CONFIG_CACHE_MANAGEMENT` 已开（ENET DMA 缓冲维护用）；整体
      I/D-cache 性能收益待测
- [ ] flash 驱动：H7 FMC 需新的 `flash_gd32_v4` 变体（当前 flash 节点为普通 `soc-nv-flash`，
      Zephyr 的 GD32 flash 驱动未启用）
- [ ] GPIO J/K 端口（H7 最多到 PK，当前设备树只挂了 A-H，本板 BGA176 用不到 J/K）
- [ ] 其余外设驱动逐个启用：SPI/I2C/TIMER/PWM/DMA/ADC/TRNG（时钟 ID 已在 `gd32h7xx-clocks.h`
      预置了 TIMER0-6/SPI0/I2C0/1）
- [ ] SWD 偶发掉线：HW-Link_LITE 探针 + pyocd 0.45-dev 组合下烧录偶发 `index out of range`
      崩溃，重试即可；必要时降低 `--frequency`
