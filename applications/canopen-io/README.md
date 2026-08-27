# canopen-io

基于 Zephyr RTOS 的 CANopen IO 计测节点,运行于 STM32F407VET6 (io-edge-hub 的
CANopen 版)。复刻 io-edge-hub 的计测器寄存器模型并全部经 CANopen 暴露:
SDO 读写、TPDO 周期/事件上报、RPDO 控制;固件下载走 CiA 302-2 (0x1F50/0x1F51)。

同时移植了 io-edge-hub 除 CAN 业务外的全部功能:**Modbus TCP/RTU**、
**Web 管理 (HTTP + WebSocket)**、**FTP + LittleFS 历史记录**、**UDP 配置与固件升级**、
**RTC 时间管理**、**IWDG 看门狗** 等完整运维能力。

| 项目 | 说明 |
|------|------|
| MCU | STM32F407VET6 (Cortex-M4 @ 168MHz, 512KB Flash, 192KB RAM = 128KB SRAM + 64KB CCM) |
| 板 | `io_edge_f407vet6` (LCKFB STM32F407VET6) |
| 网络 | W5500 (SPI2, 硬件 TCP/IP 协议栈) |
| 外部存储 | W25Q128 SPI Flash (SPI1, 16MB) |
| RS485 | USART2 + MAX485 (Modbus RTU) |
| CANopen | CAN1 (PA11/PA12), 默认节点号 10, 250 kbit/s |
| Bootloader | MCUboot (SWAP_SCRATCH + RSA-2048, 支持回滚) |

## 功能

- **CANopen 节点**:CANopenNode v4 (NMT/EMCY/SYNC/PDO/SDO),CiA 303-3 LED 指示,
  OD 参数持久化 (settings/FCB);DI/AI 实时数据经 TPDO1/TPDO2 自动上报,
  DO 经 RPDO1/SDO 写 0x2002 即时驱动 (并同步 Modbus 寄存器)
- **IO 采集**:16 路 DI(光耦隔离)+ 8 路 DO(驱动 + LED 联动)+ 4 路 AI(电流 4-20mA / 电压 0-10V)
- **Modbus TCP Server**:端口 502,RAW ADU + select() 多路复用,无客户端数量限制,
  链路断连安全清零 DO,TCP Keepalive 检测主站掉线
- **Modbus RTU Slave**:RS485,波特率/Slave ID 可配(默认 9600/1)
- **FTP Server**:端口 21,LittleFS 历史文件管理(admin/admin + anonymous 只读,PASV/EPSV)
- **Web 管理**:HTTP 页面 + REST API (/api/*) + WebSocket 实时推送 (/ws),
  浏览器内完成 IO 监控 / 参数配置 / 历史下载 / WebSocket 固件升级
- **双通道固件升级**:UDP(端口 8600)+ CANopen SDO(CiA 302-2)+
  Web WebSocket 三通道互斥;MCUboot SWAP_SCRATCH,验签失败自动回滚
- **历史记录**:LittleFS,`data_MMDD_HHMMSS.raw`,单文件 1MB 轮转(保留 10 个),
  系统工作队列批量写
- **参数持久化**:Zephyr settings + FCB,直接映射 Modbus holding 寄存器(`modbus/` 命名空间);
  OD 0x2004 应用参数为寄存器的桥接视图
- **时间管理**:STM32 内部 RTC(LSE),日志使用 RTC 时间戳,Modbus/UDP/Web 可设时间
- **安全机制**:网络断连 DO 安全清零、栈溢出保护(自动重启)、IWDG(30s)

## 寄存器模型(CANopen 与 Modbus 共享)

`holding_reg[]` 是唯一参数源,Modbus FC03/06/16、Web REST/WS、shell 全部共用同一读写路径:

| 地址 | 名称 | 说明 |
|------|------|------|
| 0x00 | DO1-8 输出控制 | 与 OD 0x2002 双向同步 |
| 0x01-0x02 | DI/AI 使能位图 | 对应 OD 0x2004:1/:2 |
| 0x03-0x04 | DI/AI 采样间隔 ms | 钳位 10-5000,对应 OD 0x2004:3/:4 |
| 0x05 | 历史保存使能 | |
| 0x06-0x07 | RS485 波特率 / Slave ID | 重启生效 |
| 0x08-0x0B | IP 四段 | 静态 IP,重启生效 |
| 0x0C-0x0D | 时间戳高/低 16 位 | 读返回实时 RTC 时间 |
| 0x0E | 参数保存触发 | 写非 0 → FCB 全量保存 |
| 0x0F | 重启触发 | 写非 0 → 延迟冷重启 |

Input registers: `0x00` 版本(主<<12 \| 次<<8 \| 补丁), `0x01-0x04` AI 工程量, `0x05` DI 位图。

## 编译

需 Zephyr SDK + west + Python venv(含 littlefs module,见仓库 `west.yml`)。

```shell
west update                  # 首次:拉取 littlefs / canopennode 等模块
west build -b io_edge_f407vet6 applications/canopen-io --sysbuild
west flash                   # 烧录 MCUboot + 应用 (一次两镜像)

# 发布模式 (关 LOG/SHELL/Web, 减小体积)
west build -b io_edge_f407vet6 applications/canopen-io --sysbuild -DCONF_FILE=prj_release.conf
```

> `build/` 目录默认可能被其他应用占用,可用 `--build-dir build/canopen-io` 指定独立目录。

## 目录结构

```
canopen-io/
  CMakeLists.txt            -- 源文件列表
  Kconfig                   -- 线程栈/优先级 + 寄存器数量 + Web 配置
  prj.conf / prj_release.conf -- 调试/发布配置
  sysbuild.conf             -- MCUboot (SWAP_SCRATCH + 签名密钥)
  sysbuild/mcuboot.conf     -- MCUboot 参数
  VERSION                   -- 版本号
  objdict/                  -- CANopenNode 对象字典 (OD.c/OD.h)
  boards/
    io_edge_f407vet6.overlay -- W5500/ADC/RS485/DI/DO/LED/IWDG 外设
    io_edge_f407vet6.pem     -- MCUboot RSA-2048 签名密钥 (独立)
  include/
    init.h                  -- 寄存器枚举 / his_data / 全局函数声明
    fw_upgrade_state.h      -- 升级通道互斥接口
  src/
    main.c                  -- 网络(MAC/IP/事件) + housekeeping(喂狗/延迟重启) + CANopen 主循环
    app_od.c                -- OD 扩展回调 (0x2004 ↔ 寄存器桥接)
    fw_download.c           -- CiA 302-2 固件下载 (0x1F50/0x1F51 → slot1)
    fw_upgrade_state.c      -- UDP/WS/SDO 三通道升级互斥
    modbus/                 -- function.c(寄存器+settings)/ tcp.c(RAW ADU TCP)/ rtu.c(RS485)
    io/                     -- dio.c(16DI+8DO+8LED+OD 镜像)/ adc.c(4 路 AI+OD 镜像)
    history/history.c       -- 历史记录 (msgq + 批量落盘)
    settings/init.c         -- settings 加载 (FCB → holding_reg)
    storage/fs_littlefs.c   -- LittleFS 挂载
    sys/                    -- time.c(RTC) / watchdog.c(IWDG)
    ftp_server/ftpd.c       -- FTP server (PASV/EPSV)
    udp.c / udp.h           -- UDP app handler (0x10+ 业务命令)
    shell.c                 -- 调试 shell ('io' 根命令)
    web/                    -- httpd.c / ws_io.c / index.html (gzip 内嵌)
```

板定义在仓库根 `boards/io_edge_f407vet6/`(含 clock/flash 分区/CAN1/RNG,供 MCUboot 共享)。

## SYS_INIT 优先级

| 优先级 | 配置项 | 模块 | 说明 |
|--------|--------|------|------|
| 3 | `CONFIG_CANOPEN_IO_INIT_PRIORITY_FW_UPGRADE` | fw_upgrade_state | 升级通道互斥锁注册 |
| 5 | `CONFIG_CANOPEN_IO_INIT_PRIORITY_HIST_WORKQ` | history | 历史记录工作队列 |
| 8 | `CONFIG_CANOPEN_IO_INIT_PRIORITY_SETTINGS` | settings | 加载持久化参数 (FCB → holding_reg) |
| 12 | `CONFIG_CANOPEN_IO_INIT_PRIORITY_IO` | dio/adc | GPIO + ADC 初始化 |
| 13 | `CONFIG_CANOPEN_IO_INIT_PRIORITY_RTU` | rtu | Modbus RTU Slave |
| 41 | `CONFIG_CANOPEN_IO_INIT_PRIORITY_CLOCK` | time | RTC → 系统时钟 |
| 50 | `CONFIG_CANOPEN_IO_INIT_PRIORITY_WATCHDOG` | watchdog | IWDG (30s) |
| 60 | `CONFIG_CANOPEN_IO_INIT_PRIORITY_WS` | ws_io | WebSocket 槽位初始化 |
| 80 | `CONFIG_CANOPEN_IO_INIT_PRIORITY_UDP` | udp | UDP 应用回调注册 |
| 99 | `CONFIG_CANOPEN_IO_INIT_PRIORITY_LITTLEFS` | fs_littlefs | LittleFS 挂载 |

## 线程

| 优先级 | 线程名 | 说明 | 栈 |
|--------|--------|------|----|
| -1 | sys_workq | 系统工作队列 (Modbus RAW server 处理) | 2048 |
| 0 | main | 网络启动 + CANopen 1ms 主循环 | 2048 |
| 2 | di | DI 采样 (`CANOPEN_IO_DI_*`) | 512 |
| 2 | adc_io | AI 采样 (`CANOPEN_IO_ADC_*`) | 512 |
| 8 | ws_N | WebSocket 会话 (`CANOPEN_IO_WEB_WS_*`) | 2048 |
| 10 | housekeeping | IWDG 喂狗 + 延迟重启处理 | 2048 |
| 13 | mb_tcp | Modbus TCP Server (`CANOPEN_IO_MODBUS_TCP_*`) | 2048 |
| 13 | ftp | FTP Server (`CANOPEN_IO_FTP_*`) | 4096 |
| 1 | udp_fw_rx | UDP 固件升级 RX (libs/) | 2048 |
| HTTP 服务线程 | httpd | Zephyr HTTP server (`HTTP_SERVER_*`) | 4096 |

> 数值越小优先级越高。CANopenNode 无独立线程:由 main 循环驱动。

详细对象字典/PDO 映射/升级流程见 [USER_GUIDE.md](USER_GUIDE.md);
io-edge-hub 功能对照见其仓库文档。

## 许可证

Apache-2.0
