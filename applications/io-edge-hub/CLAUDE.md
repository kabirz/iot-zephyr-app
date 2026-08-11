# CLAUDE.md -- io-edge-hub

## 项目愿景

io-edge-hub 是运行在 STM32F407VET6(Cortex-M4, 168MHz, 512KB Flash, 192KB RAM)上的 Zephyr RTOS 嵌入式应用,作为**工业 IO 数据采集边缘节点**:16 路 DI / 8 路 DO / 4 路 AI 采集,通过 W5500 以太网提供 Modbus TCP/RTU 实时数据,具备 LittleFS 历史存储、FTP 文件访问、UDP+CAN 双通道远程固件升级、远程参数配置等运维能力。

- **版本**:v0.1.0_\<6hex\>(格式 `v<M>.<m>.<p>_<6位git commit>`)
- **板**:`io_edge_f407vet6`(LCKFB STM32F407VET6),仓库第二个板(独立于 `nrf24_f103rct6`)
- **Bootloader**:MCUboot(SWAP_SCRATCH,支持回滚)
- **网卡**:W5500(SPI2,硬件 TCP/IP)
- **外部存储**:W25Q128(SPI1,16MB)
- **许可证**:Apache-2.0

> 项目名 `io-edge-hub`(应用目录,连字符);板名 `io_edge_f407vet6`(下划线,含芯片 F407VET6,与 `nrf24_f103rct6` 风格一致)。设计文档见 `docs/io-edge-hub.md`。

---

## 架构总览

应用层各模块通过 **SYS_INIT**(静态初始化)或 **K_THREAD_DEFINE**(静态线程)自启动,`main()` 仅负责网络初始化与状态指示。模块间通过 `holding_reg[]` / `input_reg[]` 数组(经 `init.h` 的 get/update 函数访问)与 settings 解耦。

```
main() + SYS_INIT 链 (priority):
  settings_subsys_init + load (init.c, 11)    ← 从 FCB 恢复 holding_reg[]
  dio_init / adc_init (12)                     ← DI/DO/LED/AI GPIO 配置
  rtu_init (13)                                ← Modbus RTU slave (读 baud/slave_id)
  can_app_init / udp_app_init (10 / 80)        ← 注册固件升级库业务回调
  littlefs_init (99)                           ← LittleFS 挂载 /lfs1
  clock_init (time.c, POST_KERNEL 41)          ← RTC → 系统时钟
  watchdog_init (50)                           ← IWDG
  udp_fw_upgrade / can_fw_upgrade (库, 90/70)  ← SYS_INIT 自管配置端口/CAN

main() 线程:
  MAC from UID → SET_MAC → 静态IP(从 holding_reg)→ net_if_up → 等 IF_UP
  → 状态 LED 心跳 (300ms/2700ms) + 延迟重启
```

线程:`di`(DI 采样,512/1)、`adc_io`(AI 采样,512/1)、`mb_tcp`(Modbus TCP,2048/13)、`ftp`(FTP,4096/13)。**历史记录无独立线程**,复用系统工作队列(`k_work_delayable`,1s 批量 flush)。共享库线程:`udp_fw_rx`/`can_fw_rx`(1024/8)。**无应用层心跳**:主站连接存活由 Modbus TCP socket 的 `SO_KEEPALIVE`(TCP Keepalive)检测,探测参数见 `prj.conf` 的 `NET_TCP_KEEPIDLE/INTVL/CNT`。

### 关键设计决策

- **settings 直接映射 holding_reg**:`modbus/` 命名空间,settings handler(`function.c`)直接读写 `holding_reg[]` 元素,**无独立参数结构体**,避免双份数据同步。写 holding 0x10 触发全量 `settings_save()`。
- **Modbus RAW ADU**(非 Zephyr 内置 TCP Server):`MODBUS_MODE_RAW` + 自定义 TCP socket(`tcp.c`,select() 多路复用,3 客户端,30s 超时,链路断连拒绝新连接 + 清零 DO)。比内置 Server 更灵活可控。
- **主站连接存活用 TCP Keepalive**(非应用层心跳):Modbus TCP 客户端 socket 启用 `SO_KEEPALIVE`(参数 `NET_TCP_KEEPIDLE/INTVL/CNT`,见 prj.conf),主站异常掉线时协议栈自动断开连接;`heart` 线程与 HEART 寄存器(0x12-0x14)已移除,不占 Modbus 寄存器。
- **双通道固件升级**:`udp_fw_upgrade`(端口 8600)+ `can_fw_upgrade`(帧 0x101-0x105)共享库,SYS_INIT 自管 RX 线程,应用仅 `udp_fw_set_app_handler` / `can_fw_set_app_handler` 注册业务回调。仓库首个双通道应用。
- **CAN1 在板 DTS**(非应用 overlay):`boards/io_edge_f407vet6/io_edge_f407vet6.dts` 含 `&can1` + `zephyr,canbus` chosen,供 MCUboot 与应用共享(未来 MCUboot 可能用 CAN)。
- **MCUboot 跨 flash**:Primary Slot 内部 Flash(448KB),Secondary Slot + Scratch 外部 W25Q128(SWAP_SCRATCH)。slot1/scratch 必须在板 DTS(bootloader 独立编译,不含应用 overlay)。
- **硬件 RNG**:`ENTROPY_STM32_RNG` + 板 DTS `&rng` status okay(MCUboot 与 app 共享),替代 `TEST_RANDOM_GENERATOR`。
- **历史记录(msgq + 系统工作队列)**:`K_MSGQ_DEFINE`(16 槽)累积采样数据,采样线程 `send_history_data` 入队 + `k_work_submit`(立即提交,k_work 合并去重 → 机会性批量);系统工作队列 handler 批量取 msgq + 写当前文件(保持打开 + `fs_sync`),**减少 Flash 写次数**;单文件 1MB 轮转(`data_MMDD_HHMMSS.raw`,保留 10 个);无独立线程。
- **FTP 单线程多客户端**:`ftpd.c` 单线程 select 多路复用(最多 3 个客户端命令交错,RETR/STOR/LIST 传输时该会话独占),per-session buffer;**PASV/EPSV + PORT/EPRT** 数据连接(RFC 959 + 2428);TYPE A(ASCII CR/LF 转换)与 TYPE I;LIST 标准 `ls -l`(历史文件 `data_MMDD_HHMMSS.raw` 从文件名解析真实创建时间,其他文件用当前 RTC——Zephyr `fs_dirent` 不暴露 mtime)。
- **栈溢出保护**:`k_sys_fatal_error_handler` 捕获 `K_ERR_STACK_CHK_FAIL` → warm reboot。F4 有 MPU,板 defconfig 用 `HW_STACK_PROTECTION`(非 F1 的 `STACK_SENTINEL`)。
- **网络断连 DO 安全**:`NET_EVENT_IF_DOWN` 回调立即清零所有 DO 输出(工业安全)+ 设 `link_down` 拒绝新 Modbus TCP 连接。
- **MAC 从 UID 派生**:hwinfo 读 STM32 96-bit UID,前 3B = Wiznet OUI `00:08:DC`,后 3B 折叠,`net_if_up` 前 `SET_MAC_ADDRESS`。需 `CONFIG_ETH_NET_IF_NO_AUTO_START=y`。

---

## 构建与开发

```shell
. C:\Users\<user>\venv\zephyr\Scripts\Activate.ps1   # venv (含 littlefs module)
west build -b io_edge_f407vet6 applications/io-edge-hub --sysbuild --build-dir build/io-edge-hub
west flash
# 发布: --sysbuild -DCONF_FILE=prj_release.conf  (debug ~256KB / release ~134KB, slot0 上限 448KB)
```

> `west.yml` 已 import `littlefs`(name-allowlist);改后须 `west update`。`fw_keyhash.h` 仅 sysbuild 生成(依赖 `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE`,由 SB_CONFIG 传入),非 sysbuild 编译会因 `can_fw_upgrade.c` include 它而失败。

---

## 寄存器与协议

- **寄存器枚举**:`include/init.h`(`enum holding_reg_idx` / `enum input_reg_idx`),18 个 holding + 6 个 input。改寄存器布局同步改此处 + settings handler(`function.c` 的 `mb_handle_set`/`mb_handle_export`)。
- **Modbus 寄存器映射、UDP 命令、CAN 帧、默认参数**:详见 `USER_GUIDE.md`。
- **UDP 命令码**:`src/udp.h`(`enum udp_app_cmd`,0x10+)。CAN 业务帧 ID = `holding_reg[CAN_ID]`(默认 0x0111)。

---

## 源码结构

```
include/init.h             -- 寄存器枚举 / his_data / 全局函数声明 (唯一公共头)
src/
  main.c                   -- 网络(MAC/IP/事件) + 状态LED + 延迟重启 + k_sys_fatal_error_handler
  udp.c / udp.h            -- UDP app handler (0x10+ 命令, 操作 holding_reg)
  can.c / can.h            -- CAN app handler (业务帧, can_fw_set_app_handler)
  modbus/
    function.c             -- holding_reg/input_reg 数组 + get/update + Modbus user callbacks
                              (io_modbus_cbs) + settings handler (modbus/ 命名空间) + factory reset
    init.c                 -- settings_subsys_init + load (SYS_INIT 11)
    tcp.c                  -- Modbus TCP RAW ADU Server (select 多路复用, RAW_0 iface)
    rtu.c                  -- Modbus RTU Slave (modbus0 节点, baud/slave_id 从 holding_reg)
    adc.c                  -- 4 路 AI (adc_channel_setup + 工程量转换 7.414x/3.7037x)
    dio.c                  -- 16 DI + 8 DO + 8 LED (zephyr,user gpio 列表, DT_FOREACH_PROP_ELEM)
    history.c              -- msgq + 系统工作队列批量写 + 1MB 轮转 (LittleFS /lfs1)
  storage/
    fs_littlefs.c/.h       -- LittleFS 挂载 (flash-area 模式, /lfs1) + 就绪信号量
  sys/
    time.c                 -- RTC (clock_init POST_KERNEL 41) + set_timestamp + 日志时间戳
    watchdog.c/.h          -- IWDG (30s) + watchdog_feed (main 主循环 + mkfs 事件)
  ftp_server/
    ftpd.c                 -- FTP server (单线程 select, PASV/EPSV+PORT/EPRT, 3 客户端)
    ftp.h                  -- FTP 配置 (端口/用户/根目录)
boards/
  io_edge_f407vet6.overlay -- 应用外设 (W5500/ADC/RS485+modbus/DI/DO/LED/RTC/IWDG, entropy chosen)
  io_edge_f407vet6.pem     -- MCUboot RSA-2048 签名密钥 (本应用独立, 与 n2e-gw/angle-handler 不通用)
```

板定义 `boards/io_edge_f407vet6/`(仓库根):clock(HSE 13MHz→PLL 168MHz)、USART1 控制台、内部 Flash 分区(slot0)、外部 W25Q128 分区(slot1/scratch/storage/littlefs)、`&can1`、`&rng`。

---

## MCUboot 签名密钥(每应用独立)

`sysbuild.conf` 用 `${APP_DIR}/boards/${BOARD}.pem` 指向**本应用目录**密钥。io-edge-hub 与 n2e-gw / angle-handler 各自独立 RSA-2048 密钥,**镜像互不通用**(一个 bootloader 只能验签自己密钥的应用)。

重新生成:`python bootloader/mcuboot/scripts/imgtool.py keygen -k applications/io-edge-hub/boards/io_edge_f407vet6.pem -t rsa-2048`(改密钥后 mcuboot 与 app 必须重新烧写)。

---

## 编码规范

- C:tab 缩进、K&R;中文注释 OK;**所有文件 LF 换行**
- 设备获取用 devicetree 宏(`DEVICE_DT_GET`、`GPIO_DT_SPEC_GET`、`DT_FOREACH_PROP_ELEM`)
- 线程用 `K_THREAD_DEFINE` 静态定义;初始化用 `SYS_INIT` 分级
- GPIO 引脚通过 `/zephyr,user` 节点 `di-gpios`/`do-gpios`/`led-gpios` 列表定义(代码 `DT_FOREACH_PROP_ELEM` 生成数组)
- 日志用 `LOG_MODULE_REGISTER` + `LOG_INF/ERR/DBG/WRN`
- CONFIG 符号前缀:无(用通用 Zephyr 符号);应用 Kconfig 用 `IO_*`(线程栈/优先级)、`MODBUS_*_REGISTER_NUMBERS`(寄存器数量)
- 命名:目录/文档连字符(`io-edge-hub`),C/CMake/CONFIG 下划线;板名 `io_edge_f407vet6`

## AI 使用指引

- **改寄存器布局**:`include/init.h` 枚举 + `function.c`(默认值 `holding_reg[]` 初始化 + `mb_handle_set`/`mb_handle_export` + `holding_reg_wr_cb` 副作用)+ `USER_GUIDE.md` 寄存器表。
- **改引脚**:`boards/io_edge_f407vet6.overlay`(`/zephyr,user` 节点 gpio 列表);CAN/clock/flash 分区在板 DTS `boards/io_edge_f407vet6/io_edge_f407vet6.dts`。
- **改网络配置**:直接改 `prj.conf`;静态 IP 逻辑在 `main.c::net_init`(IP 从 `holding_reg[IP_ADDR_1..4]`)。
- **加 UDP 命令**:`udp.h` 加枚举 + `udp.c::app_cmd_handler` 加 case(操作 holding_reg + `udp_fw_reply`)。
- **`holding_reg[]` / `input_reg[]` 是唯一数据源**:DI/AI 采样写 input_reg,DO/参数读 holding_reg,settings 直接映射。不要引入独立参数结构体。
- **Modbus RAW ADU 流程**(`tcp.c`):recv 8B(MBAP+FC)→ `modbus_raw_get_header` → recv data → `modbus_raw_submit_rx`(server 调 user_cb 处理)→ `raw_tx_cb` 回填响应 → `k_sem_take` → `modbus_raw_put_header` + send。iface 名 `RAW_0`。
- **固件升级**:库自管,应用只注册回调;不要在应用里碰配置 socket 或 CAN 帧 0x101-0x105。
- **`LOG=n`(release)**:`log_process()` / `log_set_timestamp_func()` 需 `#ifdef CONFIG_LOG` 保护。
- **历史记录**:用 `send_history_data()` 入队(msgq),不要直接写文件;writer 线程异步落盘。
