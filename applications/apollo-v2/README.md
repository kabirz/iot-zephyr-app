# apollo-v2 — 阿波罗 V2 STM32H743 应用工程

按《Apollo_V2_STM32H743_Application_Plan.md》从零重写的 apollo-v2 应用
（第一版代码已删除，开发实录与必保留修复见计划 §10）。当前实现 **P0 阶段**
（双变体出镜像 + console/shell/基础 IO），并带入已实测的 P1 BSP 底座
（QSPI / SDRAM / 以太网 / TF 卡 / RTC / FDCAN1）。

## 双变体构建

USB OTG FS 与 FDCAN1 共用 PA11/PA12（底板 P9 二选一），一套代码两个变体：

```shell
source ~/.venv/zephyr/bin/activate
cd /home/zhp/code/zephyr/app

# CAN 变体 (默认, P9 跳线置 CAN):
west build -b apollo_v2_h743iit6 apps/applications/apollo-v2 -d build/apollo_v2

# USB 变体 (P9 跳线置 USB, CDC ACM 枚举; FDCAN1 不使能):
west build -b apollo_v2_h743iit6 apps/applications/apollo-v2 -d build/apollo_v2_usb \
    -DEXTRA_CONF_FILE=usb.conf -DDTC_OVERLAY_FILE=usb.overlay
```

## 烧录 / 串口

```shell
west flash -d build/apollo_v2
st-flash --connect-under-reset reset     # west flash 后芯片处于 halt, 需复位运行
# 串口 (打开前清 RTS/DTR, 避免拉住复位线):
python -m serial.tools.miniterm /dev/ttyUSB0 115200 --rts 0 --dtr 0
```

## shell 命令

```
apollo info                         # 板/版本/运行状态汇总
apollo beep [ms]                    # 蜂鸣器 (PCF8574 P0, 低=响)
apollo led <0|1> <on|off|toggle>    # DS0 红 PB1 / DS1 绿 PB0 (低电平点亮)
apollo adc                          # ADC1 PA5 (mV)
apollo time                         # 本地时间 (RTC 存 UTC, 显示 UTC+8)
apollo timeset YYYY-MM-DD HH:MM:SS  # 设 RTC (本地时间)
apollo cansend <hex-id> <hex字节>   # CAN 变体: 发送标准帧
apollo reboot                       # 热复位
```

按键: WK_UP 蜂鸣一声; KEY0/1/2 翻转绿色 LED。DS0 红灯 1Hz 心跳。
每 `CONFIG_APOLLO_STATUS_PERIOD` 秒输出一行状态 (uptime/时间/IP/存储/CAN)。

## 实测状态 (2026-09-05, CAN 变体上板)

| 项目 | 结果 |
|---|---|
| 双变体编译 | ✅ CAN 355KB / USB 373KB (内部 Flash 2MB, RAM ~185KB) |
| banner / shell / apollo info | ✅ fw 0.1.0-<git6位> |
| LED / 按键 / 蜂鸣器静音 | ✅ (上电第一时间静音 PCF8574, 跨复位保持) |
| QSPI W25Q256 32MB 探测 | ✅ SFDP/JEDEC, indirect 模式不写 |
| FMC SDRAM 32MB 走样 | ✅ |
| RTC (LSE) + UTC+8 | ✅ 掉电保持, 时间正确 |
| 以太网 YT8512C 100M 全双工 | ✅ DHCP 无服务器时 20s 回退静态 192.168.12.200 |
| ping 192.168.12.200 | ✅ 0% 丢包, RTT ~0.2ms (直连 enp3s0) |
| FDCAN1 500k 正常模式 | ✅ 启动正常 (总线无帧, 计数 0) |
| ADC1 PA5 | ✅ ~2.5V (P10 跳线悬浮读数) |
| TF 卡识别 | ✅ 16GB 卡识别 + 扇区读 |
| TF 挂载 | ⚠️ 当前卡无 FAT/exFAT 文件系统 (扇区 0 无签名), 格式化后可挂载 |

已知无害噪音: 上电第一条 `pcf857x: Pin(s) is/are configured as input...`
是内核 pcf857x 驱动对首次 `GPIO_OUTPUT` 配置的误报 (内部 check 用错
value/mask), 副作用仍把引脚标为输出, 后续写入正常。

## 与计划路线图的对应

| 阶段 | 状态 |
|---|---|
| P0 west 环境/骨架/console/shell | ✅ 本次完成 |
| P1 BSP (QSPI/SDRAM/ETH/SDMMC) | ✅ 底座完成; MCUboot+XIP 属头号攻关, 未开始 |
| P2 三通道升级 (MCUmgr-UDP/Web/CANopen) | 未开始 (依赖 MCUboot) |
| P3 CANopen / P4 Web 面板 / P5 LVGL / P6 音频 / P7 扩展 | 未开始 |

计划 §10.2 "重新实现时必须保留的修复" 落实情况:
- #1 SDMMC `idma` → 板 DTS ✅
- #3 LED 低有效 → 板 DTS ✅
- #4/#6 FATFS LFN/中文/REENTRANT → prj.conf ✅ (+exFAT)
- #8/#9 不启用 `CONFIG_STM32_MEMMAP`、不加 QSPI MPU region → QSPI 仅 indirect 探测 ✅
- #10 RTC 存 UTC、显示 UTC+8 → `src/app_time.c` ✅
- #11 按键上拉 → 板 DTS ✅
- #13 TF 挂载周期重试 → `src/storage.c` ✅

## 目录

```
├── CMakeLists.txt          # 含 fw_gitver.h 生成 (git 短哈希)
├── Kconfig                 # APOLLO_* 应用配置
├── prj.conf                # 公共配置 (IO/存储/CAN/网络)
├── VERSION                 # 0.1.0 (sysbuild/MCUboot 镜像版本用)
├── boards/apollo_v2_h743iit6.overlay   # CAN 变体 (默认)
├── usb.overlay + usb.conf  # USB 变体 (构建参数引入)
└── src/
    ├── main.c              # 启动流程 + 心跳/状态行
    ├── board_io.c          # LED/按键(input)/蜂鸣器/ADC
    ├── app_time.c          # RTC + UTC/本地时间
    ├── storage.c           # QSPI/SDRAM 探测 + TF 带重试挂载
    ├── net.c               # DHCP→静态回退
    ├── can.c               # FDCAN1 收发/计数
    ├── usb.c               # CDC ACM (USB 变体)
    ├── shell.c             # apollo 根命令
    └── version.h
```
