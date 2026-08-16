# 数据采集卡 变更记录

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
