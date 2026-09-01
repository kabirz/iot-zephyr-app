# 数据采集卡 变更记录

## v2.0.1（2026-09-01）

- 发布标签：`v2.0.1-io-edge-hub`
- 发布提交：`051a8e4`
- 提交时间：`2026-09-01 21:37:30 +0800`

### 自 v2.0.0-io-edge-hub 以来的改动

- `a8bca12` 修复 v2.0.0 中全部 AI 通道无读数的问题：STM32F4 上无 DMA 支持的多通道单次扫描会持续发生 ADC 溢出（每次转换的 EOC 中断跟不上 3 个时钟的采样节奏），撤销"单次扫描读取 + 共享通道模板"方案，恢复逐通道阻塞读取，设备树同步恢复 4 路 AI 独立通道节点（channel@a–d）配置（`applications/io-edge-hub/src/io/adc.c`、`boards/io_edge_f407vet6.overlay`）

## v2.0.0（2026-08-30）

- 发布标签：`v2.0.0-io-edge-hub`
- 发布提交：`ee43979`
- 提交时间：`2026-08-30 20:25:31 +0800`

### 自 v0.2.1-io-edge-hub 以来的改动

> 注：`v0.2.3-io-edge-hub` 是一次未完成的发版尝试（其 tag 指向的提交不在主干上，未产出镜像与文档），本版本直接承接 v0.2.1 之后的全部主干改动。

- `c926cb2` RTC 时钟源由内部 LSI 改为 HSE/16 分频，提高走时精度（`applications/io-edge-hub/boards/io_edge_f407vet6.overlay`）
- `1e07225` 新增可选 Web 管理界面：内置 HTTP 服务器 + WebSocket 实时通道，网页查看/控制 IO、修改配置，网页资源压缩后存放于 ROM 专用段（`applications/io-edge-hub/src/web/`）
- `4afba83` 新增 `io` 调试 shell 命令组，可在线查看 DI/DO/AI 状态并修改采样配置（`applications/io-edge-hub/src/shell.c`）
- `1129095` 源码目录按功能域重构拆分（history/io/settings/sys/web 等），设计文档更新至 v3.5（`applications/io-edge-hub/src/`）
- `5b25bc2` Modbus TCP 会话超时改为 Kconfig 可选项且默认关闭，避免误踢长连接主站（`applications/io-edge-hub/Kconfig`、`src/modbus/tcp.c`）
- `067d70c` 切换到个人 Zephyr fork 以获得 DTCM 网络内存池与 UART 修复，网络缓冲配置随之调整（`applications/io-edge-hub/prj.conf`、`boards/io_edge_f407vet6/`）
- `39cf7fc` 历史记录写入效率优化，新增重启前数据同步；CAN 升级库配合增加同步接口（`applications/io-edge-hub/src/history/history.c`、`libs/can_fw_upgrade/`）
- `a956b2a` Web 固件升级从 HTTP 迁移到 WebSocket 通道，Web UI 整体打磨，新增网页资源压缩脚本（`applications/io-edge-hub/src/web/`）
- `a16e65c` UDP 固件升级新增 FW_DATA_V2 窗口化传输协议，升级更快更可靠，固件与主机工具两侧同步实现（`applications/io-edge-hub/prj.conf`、`tools/firmware_upgrade/`）
- `54d9c72` CAN 固件升级支持双槽（dual-slot）MCUboot 布局，主机升级工具同步改进（`libs/can_fw_upgrade/can_fw_upgrade.c`、`tools/firmware_upgrade/`）
- `0d17877` Web 升级前校验镜像 keyhash，新增网页端恢复出厂设置（`applications/io-edge-hub/src/web/`、`src/shell.c`）
- `8c29b46` 修复 bootloader CAN 救援模式 START 帧超时问题（`libs/can_fw_upgrade/`）
- `c390b3f` CAN 升级通道波特率改从持久化设置读取，与运行配置一致（`libs/can_fw_upgrade/`、`applications/io-edge-hub/src/settings/init.c`）
- `5045fe7` 修复 CAN 波特率不匹配时 MCUboot 卡死的问题（`libs/can_fw_upgrade/`）
- `c43db20` WebSocket OTA 在擦除 Flash 前先校验镜像 keyhash，校验失败不再破坏现有固件（`applications/io-edge-hub/src/web/ws_io.c`）
- `ba50f94` 日志时间戳改用 RTC 时间并统一 UTC+8 时区（`applications/io-edge-hub/src/sys/time.c`）
- `c81d7b3` 全部初始化顺序与线程优先级改为可通过 Kconfig 配置（`applications/io-edge-hub/Kconfig`）
- `49d8759` 新增 UDP/CAN/WebSocket 三通道固件升级互斥锁，防止并发升级冲突（`applications/io-edge-hub/src/fw_upgrade_state.c`、`libs/can_fw_upgrade/`）
- `1cb6878` 取消 Modbus TCP 客户端数量上限（`applications/io-edge-hub/src/modbus/tcp.c`）
- `4ec10bf` 代码审查集中修复 7 项问题（历史记录、UDP 配置、WebSocket 通道等）（`applications/io-edge-hub/src/`）
- `d0fe948` 全部 C 源码统一应用 clang-format 代码风格（`applications/io-edge-hub/src/` 全部源文件）
- `e3f9133` 清理编译警告：删除死代码，板级 dts 补充 erase-block-size（`applications/io-edge-hub/src/modbus/tcp.c`、`boards/io_edge_f407vet6/`）
- `4c57a2b` 历史记录重新使能时续写同一文件而不是新建文件（`applications/io-edge-hub/src/history/history.c`）
- `aeb2bbf` 修复 FTP 大文件传输损坏，Web/Modbus/UDP 行为与 FreeRTOS 移植版对齐（`applications/io-edge-hub/src/ftp_server/ftpd.c` 等）
- `d1e659e` 新增 CANopen 固件升级主机工具（CiA 302-2 协议，基于 python-canopen）（`tools/firmware_upgrade/canopen_fw_upgrade.py`）
- `8e64206` 修复 CANopen 升级工具 CLI、异常处理与版本检查缺陷（`tools/firmware_upgrade/canopen_fw_upgrade.py`）
- `8c717dc` CANopen 升级工具最终审查修复：回退逻辑、静态存储、测试排序（`tools/firmware_upgrade/canopen_fw_upgrade.py`）
- `0425b37` CANopen 升级工具改用原始 SDO 访问，无需 EDS 文件即可工作（`tools/firmware_upgrade/canopen_fw_upgrade.py`）
- `01b3636` CANopen 升级工具对 0x1F50/0x1F51 对象按 VAR（子索引 0）寻址，兼容更多从站实现（`tools/firmware_upgrade/canopen_fw_upgrade.py`）
- `848a7f8` ADC 改为单次扫描读取，设备树改用共享通道模板简化配置（`applications/io-edge-hub/src/io/adc.c`、`boards/io_edge_f407vet6.overlay`）
- `766959f` 修复固件升级锁的生命周期管理，Modbus 触发的重启延后到响应发送之后执行（`applications/io-edge-hub/src/fw_upgrade_state.c`、`src/modbus/function.c`、`src/web/ws_io.c`）
- `425750d` history_sync 改经历史工作队列执行，避免阻塞调用线程（`applications/io-edge-hub/src/history/history.c`）

## v0.2.1（2026-08-16）

- 发布标签：`v0.2.1-io-edge-hub`
- 发布提交：`1946fda`
- 提交时间：`2026-08-16 18:32:33 +0800`

### 自 仓库初始提交 以来的改动

- `6da82f0` 新增 io-edge-hub 工业 IO 采集边缘节点应用（STM32F407VET6）：16 路 DI / 8 路 DO / 4 路 AI 采集、Modbus TCP(RAW ADU)+RTU、FTP 服务器、LittleFS 历史存储、UDP+CAN 双通道固件升级、settings 映射的保持寄存器、RTC、看门狗与硬件随机数；同时新增 `io_edge_f407vet6` 板定义与 Flash 分区，`west.yml` 引入 littlefs 模块（`applications/io-edge-hub/`、`boards/io_edge_f407vet6/`、`west.yml`）
- `9b39b6b` FTP 服务改为单线程 select 多路复用：最多 3 个客户端、命令交错、传输时独占；支持 PASV/EPSV + PORT/EPRT 数据连接、TYPE A/I、REST 断点续传、标准 `ls -l` LIST、120s 空闲超时与路径穿越保护（`applications/io-edge-hub/src/ftp_server/ftpd.c`）
- `1d04436` 历史记录改为 1MB 大小轮转（`data_MMDD_HHMMSS.raw`，保留 10 个）+ msgq 缓冲 + 系统工作队列批量落盘，去掉独立 writer 线程，降低 Flash 写频率（`applications/io-edge-hub/src/modbus/history.c`）
- `6a7eac7` 修复全局临时 ADU 数据竞争（TCP 响应缓冲局部化 + trans_id 匹配）；去掉应用层心跳、改用 TCP Keepalive 检测主站掉线；重启后恢复历史记录使能；DI/AI 采样间隔钳位到 5s 防看门狗复位；ADC 通道与系数改为设备树配置（`applications/io-edge-hub/src/`）
- `5133d48` 第二轮代码审查修复：缓存服务器 unit_id 用于请求校验（修复运行中改 slave_id 后 TCP 全部超时）；ADC 采样循环上限钳位防 ai_coeff[] 越界；历史 flush 移到独立工作队列隔离 LittleFS 写阻塞；UDP SET_IP 后补发重启标记（`applications/io-edge-hub/src/`）
- `ec24c00` 修复通信逻辑与运行稳定性（12 项）：TCP 短读 / MBAP 帧错误、gmtime 空指针与时间戳越界导致的 HardFault、UDP 波特率字段截断（115200→9152）、FTP APPE 误截断旧文件；超时收敛防止单个慢客户端拖垮全部 3 路连接；互斥锁保护 DO 位读写与持久化；广播 unit_id=0 不回复；会话超时放宽避免误踢空闲主站（`applications/io-edge-hub/src/`）
- `59be217` 简化 UDP 协议（12 条命令合并为 6 条：SET/GET IP、SET/GET MODBUS、SET_TIME、FACTORY_RESET）+ 重命名保持寄存器标识符；SET_IP 不再自动重启、由客户端触发；IP 校验统一并拒绝 0.x/127.x/224+ 等非法段；看门狗文档纠正为 30s、喂狗移到主循环（`applications/io-edge-hub/src/udp.c` 等）
- `b25c53c` 新增 v3.4 固件的功能 / 性能 / 压力测试套件：覆盖 Modbus TCP、Modbus RTU、UDP 配置与 CAN 通道；10 个功能测试文件、4 个独立基准、4 个长跑压力脚本与统一客户端库（`applications/io-edge-hub/tests/`）
- `cfa2446` 代码审查修复：set_timestamp() 返回结果布尔化，UDP SET_TIME 透传设备端校验结果；同步看门狗注释；删除无用头文件；文档化 TIMESTAMP 读取在 RTC 未初始化时的行为（`applications/io-edge-hub/src/sys/time.c` 等）
- `314a847` 测试套件审查修复：为会触发设备重启的用例加 write 标记；udp_client 合并跨平台发现逻辑；can_client 重命名接口；基准脚本对小样本量做 P95/P99 防护（`applications/io-edge-hub/tests/`）
- `b15fd5c` 新增固件升级 CLI（Python 跨平台 + Linux C 单文件，双通道 UDP/CAN）：MCUboot 镜像解析与 keyhash 预校验、CRC16 校验、文本进度条、`--test` 回滚模式（`tools/firmware_upgrade/`）
- `ca06546` 硬件联调发现的固件修复：拒绝 TS_MAX 时间戳；重启前 log_process() 冲刷延迟日志；CAN 升级 START_UPDATE 时总是重擦 slot1 并复位计数（修复中途中止后重试写到错误 Flash 偏移）（`applications/io-edge-hub/src/`）
- `f965720` 升级工具升级后自动发送 REBOOT 触发 MCUboot 交换；CAN 通道 write() 遇 EAGAIN 重试代替直接中止；末段镜像按真实 DLC 发送（修复补齐 8 字节导致字节计数溢出、流控卡死）（`tools/firmware_upgrade/`）
- `7433983` 修复测试套件缺陷：pymodbus 版本锁定、Modbus 异常统一转换、设备重启后 TCP 自动重连一次、会话夹具占用连接槽处理、持久化测试后恢复出厂参数避免跨轮污染；新增 1000 次 TCP 连接压力测试（`applications/io-edge-hub/tests/`）
- `edc125a` 修复历史记录崩溃与文件打不开：文件名补 `/lfs1` 挂载前缀（Zephyr 要求绝对路径）；历史工作队列栈 2048→4096、压缩过大的文件名数组（`applications/io-edge-hub/src/modbus/history.c`）
- `6fb1a40` Modbus TCP 响应改为单次 send() 合并发送（原 8 字节头 + 数据分两次 send 会拆成两个 TCP 段，部分 HMI/SCADA 主站按段解析失败）（`applications/io-edge-hub/src/modbus/tcp.c`）
- `1935c51` 保持寄存器 0x07 CAN 波特率默认值由 10 改为 250，与实际总线速率（250kbps）一致（`applications/io-edge-hub/src/modbus/function.c`）
- `01af0f1` FTP 健壮性与 RFC 合规：所有回复统一补 CRLF 行结束符（修复 ftplib/curl 等按行定帧客户端挂起）；数据连接 accept() 加 select 超时，客户端不连数据端口不再卡死整个 FTP 线程（`applications/io-edge-hub/src/ftp_server/ftpd.c`）
- `4951aeb` 性能优化：SPI2 加 DMA 通道（F407 实测吞吐约提升 50%，3.4→5.0Mbps）；网络缓冲 data size 128→512、包数 8→12；新增默认关闭的带宽测试端点 CONFIG_IO_BW_TEST（`applications/io-edge-hub/boards/io_edge_f407vet6.overlay`、`prj.conf`）
- `b9421d7` 历史文件名年月日时分秒按合法范围钳位，消除 GCC 截断告警并防止坏 RTC 数据产生非法文件名（`applications/io-edge-hub/src/modbus/history.c`）
- `ef03fff` 新增 MCUboot 内置 CAN 固件升级：bootloader 启动时发 0x106 探测帧等待主机应答，收到升级帧则进入等待、CONFIRM 后当会话内完成 swap；加 0x108 trace 帧与 0xDE flash dump；修复 mcuboot 域 flash_img_init 误写 slot0 的问题；keyhash 回退用 bootloader 签名密钥；升级 CLI 加 `-b bootloader` 模式；bootloader 配置经 `-Dmcuboot_EXTRA_CONF_FILE` 注入（`libs/can_fw_upgrade/`、`boards/nrf24_f103rct6`）
- `6d555e3` 修复压力测试脚本：workers 每秒上报 ok/err 计数让进度条实时显示；主循环按 `--duration` 自动停止（`applications/io-edge-hub/tests/stress/`）
- `01829e0` Modbus 版本寄存器（输入寄存器 0x00）编码由 `MAJOR<<8|MINOR` 改为 `MAJOR<<12|MINOR<<8|PATCH`，单次 FC04 读取返回完整 vX.Y.Z；主站解析方式随之变更（`applications/io-edge-hub/src/modbus/function.c`）
- `690d8cd` Modbus TCP 接受任意 unit_id 并在响应中原样回显（此前拒绝非匹配 unit_id 返回 Server Device Failure）；广播 unit_id=0 仍执行副作用但不回复（`applications/io-edge-hub/src/modbus/tcp.c`）
