# io-edge-hub

基于 Zephyr RTOS 的工业 IO 数据采集边缘节点,运行于 STM32F407VET6。

## 概述

io-edge-hub 提供 **16 路数字输入**、**8 路数字输出**、**4 路模拟输入**采集,通过 **Modbus TCP/RTU** 向上位机提供实时数据;具备 **LittleFS 历史数据存储**、**FTP 文件访问**、**UDP + CAN 双通道远程固件升级**、远程参数配置等完整运维能力。从 RT-Thread 实现迁移,复用已验证的硬件设计。

作为 `iot-zephyr-app` 仓库下的一个应用,与 `n2e-gw` / `angle-handler` 并列,共享 `libs/` 下的固件升级库。

| 项目 | 说明 |
|------|------|
| MCU | STM32F407VET6 (Cortex-M4 @ 168MHz, 512KB Flash, 192KB RAM = 128KB SRAM + 64KB CCM) |
| 板 | `io_edge_f407vet6` (LCKFB STM32F407VET6) |
| 网络 | W5500 (SPI2, 硬件 TCP/IP 协议栈) |
| 外部存储 | W25Q128 SPI Flash (SPI1, 16MB) |
| RS485 | USART2 + MAX485 (Modbus RTU) |
| CAN | CAN1 (PA11/PA12) |
| Bootloader | MCUboot (SWAP_SCRATCH + RSA-2048, 支持回滚) |
| 版本 | v0.1.0_\<6hex\> (`FW_GIT_VERSION`) |

## 功能

- **IO 采集**:16 路 DI(光耦隔离)+ 8 路 DO(驱动 + LED 联动)+ 4 路 AI(电流 4-20mA / 电压 0-10V,12-bit)
- **Modbus TCP Server**:端口 502,RAW ADU + select() 多路复用,无客户端数量限制,链路断连安全清零 DO,TCP Keepalive 检测主站掉线
- **Modbus RTU Slave**:RS485,波特率/Slave ID 可配(默认 9600/1)
- **FTP Server**:端口 21,LittleFS 历史文件管理(admin/admin + anonymous 只读,PASV 模式)
- **双通道固件升级**:UDP(端口 8600)+ CAN(帧 0x101-0x105),共享库自管,应用仅注册业务回调
- **历史记录**:LittleFS,`data_MMDD_HHMMSS.raw`,单文件 1MB 轮转(保留 10 个),系统工作队列批量写
- **参数持久化**:Zephyr settings + FCB,直接映射 Modbus holding 寄存器(`modbus/` 命名空间)
- **时间管理**:STM32 内部 RTC(LSI),日志使用 RTC 时间戳,Modbus/UDP 可设时间
- **安全机制**:网络断连 DO 安全、栈溢出保护(自动重启)、IWDG(30s, main 主循环喂狗 + mkfs 事件喂狗);TCP Keepalive 检测主站连接存活

## 编译

需 Zephyr SDK + west + Python venv(含 littlefs module,见仓库 `west.yml`)。

```shell
# 激活 venv
. C:\Users\<user>\venv\zephyr\Scripts\Activate.ps1   # Windows PowerShell
# source ~/venv/zephyr/bin/activate                   # Linux

west update                  # 首次:拉取 littlefs 等模块
west build -b io_edge_f407vet6 applications/io-edge-hub --sysbuild
west flash                   # 烧录 MCUboot + 应用 (一次两镜像)

# 发布模式 (关 LOG/SHELL, ~134KB)
west build -b io_edge_f407vet6 applications/io-edge-hub --sysbuild -DCONF_FILE=prj_release.conf

# MCUboot 内置 CAN 固件升级 (bootloader 探测等待, 见 libs/can_fw_upgrade)
# 注意: mcuboot_EXTRA_CONF_FILE 会顶替 sysbuild 自动注入的 sysbuild/mcuboot.conf,
# 两个片段必须一起列出 (分号列表, 引号防 shell 展开)
west build -b io_edge_f407vet6 applications/io-edge-hub --sysbuild \
  "-Dmcuboot_EXTRA_CONF_FILE=$PWD/applications/io-edge-hub/sysbuild/mcuboot.conf;$PWD/libs/can_fw_upgrade/mcuboot_can.conf"
```

> `build/` 目录默认可能被其他应用占用,可用 `--build-dir build/io-edge-hub` 指定独立目录。

## 目录结构

```
io-edge-hub/
  CMakeLists.txt            -- 源文件列表
  Kconfig                   -- 线程栈/优先级 + 寄存器数量
  prj.conf / prj_release.conf -- 调试/发布配置
  sysbuild.conf             -- MCUboot (SWAP_SCRATCH + 签名密钥)
  sysbuild/mcuboot.conf     -- MCUboot 参数
  VERSION                   -- 版本号
  boards/
    io_edge_f407vet6.overlay -- 应用外设 (W5500/ADC/RS485/DI/DO/LED/RTC/IWDG)
    io_edge_f407vet6.pem     -- MCUboot RSA-2048 签名密钥 (独立)
  include/init.h            -- 寄存器枚举 / his_data / 全局函数声明
  src/
    main.c                  -- 网络(MAC/IP/事件) + 状态LED + 栈溢出保护
    udp.c / udp.h           -- UDP app handler (0x10+ 业务命令)
    can.c / can.h           -- CAN app handler (业务帧)
    modbus/                 -- function.c(寄存器+settings)/ tcp.c(RAW ADU TCP + Keepalive)/ rtu.c
    io/                     -- dio.c(16DI+8DO+8LED)/ adc.c(4 路 AI)
    history/history.c       -- 历史记录 (msgq + 批量落盘)
    settings/init.c         -- settings 加载 (FCB → holding_reg)
    storage/fs_littlefs.c   -- LittleFS 挂载
    sys/                    -- time.c(RTC) / watchdog.c(IWDG)
    ftp_server/ftpd.c       -- FTP server (PASV)
```

板定义在仓库根 `boards/io_edge_f407vet6/`(含 clock/flash 分区/CAN1/RNG,供 MCUboot 共享)。

## SYS_INIT 优先级

| 优先级 | 配置项 | 模块 | 说明 |
|--------|--------|------|------|
| 3 | `CONFIG_IO_INIT_PRIORITY_FW_UPGRADE` | fw_upgrade_state | 固件升级互斥锁 + hook 注册 |
| 5 | `CONFIG_IO_INIT_PRIORITY_HIST_WORKQ` | history | 历史记录工作队列 |
| 8 | `CONFIG_IO_INIT_PRIORITY_SETTINGS` | settings | 加载持久化参数 (FCB → holding_reg) |
| 10 | `CONFIG_IO_INIT_PRIORITY_CAN` | can | CAN 应用回调注册 |
| 12 | `CONFIG_IO_INIT_PRIORITY_DIO` | dio | 16DI + 8DO + 8LED GPIO |
| 12 | `CONFIG_IO_INIT_PRIORITY_ADC` | adc | 4 路 AI 采样 |
| 13 | `CONFIG_IO_INIT_PRIORITY_RTU` | rtu | Modbus RTU Slave |
| 41 | `CONFIG_IO_INIT_PRIORITY_CLOCK` | time | RTC → 系统时钟 |
| 50 | `CONFIG_IO_INIT_PRIORITY_WATCHDOG` | watchdog | IWDG (30s) |
| 60 | `CONFIG_IO_INIT_PRIORITY_WS` | ws_io | WebSocket 槽位初始化 |
| 80 | `CONFIG_IO_INIT_PRIORITY_UDP` | udp | UDP 应用回调注册 |
| 99 | `CONFIG_IO_INIT_PRIORITY_LITTLEFS` | fs_littlefs | LittleFS 挂载 |

## 线程优先级

### Zephyr 系统线程

| 优先级 | 线程名 | 配置项 | 栈大小 | 说明 |
|--------|--------|--------|--------|------|
| -1 | sys_workq | `CONFIG_SYSTEM_WORKQUEUE_PRIORITY` | `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE` (2048) | 系统工作队列 |
| 0 | main | `CONFIG_MAIN_THREAD_PRIORITY` | `CONFIG_MAIN_STACK_SIZE` (2048) | 主线程 |
| 0 | log | `CONFIG_LOG_PROCESS_THREAD_PRIORITY` | `CONFIG_LOG_PROCESS_THREAD_STACK_SIZE` (768) | 日志处理 |
| 最低 | idle | - | `CONFIG_IDLE_STACK_SIZE` (320) | 空闲线程 |

### 应用线程

| 优先级 | 线程名 | 配置项 | 栈大小 | 说明 |
|--------|--------|--------|--------|------|
| 2 | di | `CONFIG_IO_DI_PRIORITY` | `CONFIG_IO_DI_STACK_SIZE` (512) | DI 采样 |
| 2 | adc_io | `CONFIG_IO_ADC_PRIORITY` | `CONFIG_IO_ADC_STACK_SIZE` (512) | AI 采样 |
| 13 | mb_tcp | `CONFIG_IO_MODBUS_TCP_PRIORITY` | `CONFIG_IO_MODBUS_TCP_STACK` (2048) | Modbus TCP Server |
| 13 | ftp | `CONFIG_IO_FTP_PRIORITY` | `CONFIG_IO_FTP_STACK_SIZE` (4096) | FTP Server |
| 15 | bw_test | - | 4096 | TCP 带宽测试 (临时) |

### 库线程 (libs/)

| 优先级 | 线程名 | 配置项 | 栈大小 | 说明 |
|--------|--------|--------|--------|------|
| 1 | udp_fw_rx | `CONFIG_UDP_FW_RX_PRIORITY` | `CONFIG_UDP_FW_RX_STACK_SIZE` (2048) | UDP 固件升级 RX |
| 1 | can_fw_rx | `CONFIG_CAN_FW_UPGRADE_RX_PRIORITY` | `CONFIG_CAN_FW_UPGRADE_RX_STACK_SIZE` (2048) | CAN 固件升级 RX |

> 数值越小优先级越高。固件升级线程优先级最高 (1),DI/AI 采样次之 (2)。

## 与设计文档

详细方案见 [`docs/io-edge-hub.md`](../../docs/io-edge-hub.md)。实现与设计的差异在该文档「附录 G:实现备注」章节记录。

## 许可证

Apache-2.0
