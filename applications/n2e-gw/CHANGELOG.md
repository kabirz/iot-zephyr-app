# 手柄接收器 变更记录

## v2.0.3（2026-10-20）

- 发布标签：`v2.0.3-n2e-gw`
- 发布提交：`338e27a`
- 提交时间：`2026-10-20 22:31:53 +0800`

### 自 v2.0.2-n2e-gw 以来的改动

- `338e27a` 发布手柄接收器 v2.0.3：版本号改为 `2.0.3`，`EXTRAVERSION` 设为 `release`（`applications/n2e-gw/VERSION`）
- `6cf6e38` 追加 v2.0.2 的变更记录文档（`applications/n2e-gw/CHANGELOG.md`）
- `43b9afb` 修复 nRF24 持续数据流导致网络饿死的问题：原先 UDP 发送在 RF24 接收线程里同步执行，W5500 每包阻塞约 10ms，独占发送通路与 SPI 总线，叠加偏小的网络缓冲池引发级联饿死（发送池耗尽→接收池耗尽→网络线程冻结），表现为 ping 超时、配置端口无响应。现改为消息队列 + 专用 UDP 发送线程异步发送，配置回复与 ICMP 回复优先获得发送带宽；网络缓冲加大到 8/8/16/16，RAM 增加约 4.2KB（`applications/n2e-gw/Kconfig`、`applications/n2e-gw/prj.conf`、`applications/n2e-gw/src/udp.c`）
- `14bf5e3` 新增 UDP 吞吐测试 shell 命令 `gw bench tx`（发送 N 包测原始发送速率，绕过消息队列直发）与 `gw bench rx`（限时统计接收包数测接收速率），发送载荷含 4 字节小端序号供接收端统计丢包（`applications/n2e-gw/src/net_shell.c`）
- `2806fa6` shell 源文件改为 CMake 级条件编译：关闭 shell 功能时 `net_shell.c`/`rf24_shell.c` 完全不参与编译，而非编译成空文件（`applications/n2e-gw/CMakeLists.txt`、`applications/n2e-gw/src/net_shell.c`、`applications/n2e-gw/src/rf24_shell.c`）
- `021c03f` 新增双向 UDP 吞吐测试命令 `gw bench bidir`：单 socket 同时向主机发送并接收，报告双向速率；测试端口改用 19601/19602 避免与生产端口（9600/9602/8600）冲突。同时新增上位机测试脚本 `tools/udp_bench.py`，支持 rx/tx/bidir 三种模式，统计包速率与丢包率（`applications/n2e-gw/src/net_shell.c`、`tools/udp_bench.py`）
- `a16e65c` 固件升级库新增 FW_DATA_V2 窗口化传输协议：新增 0x06 帧携带 4 字节偏移，设备端只按偏移顺序写入、乱序/重复帧丢弃，回复携带期望偏移供上位机回退重传；FW_START 回复扩展出分块大小协商，本设备使用 512 字节分块（配合 128B 网络缓冲）。实测 325KB 镜像数据阶段从 7 秒缩短到 3 秒；旧版上位机走原停等协议，完全向后兼容（`applications/n2e-gw/prj.conf`，核心实现在 `libs/udp_fw_upgrade` 与 `tools/firmware_upgrade`）
- `6d8fc76` ARP 表从 2 条加大到 4 条：配置了不可达的上位机 IP 时，待解析条目会把上位机/网关的已解析条目挤出 ARP 表，引发额外解析延迟；配合 Zephyr ARP pending 队列上限补丁生效，RAM 增加 96 字节（`applications/n2e-gw/prj.conf`）

## v2.0.2（2026-08-07）

- 发布标签：`v2.0.2-n2e-gw`
- 发布提交：`a27e06a`
- 提交时间：`2026-08-07 00:05:56 +0800`

### 自 v2.0.1-n2e-gw 以来的改动

- `a27e06a` 发布手柄接收器 v2.0.2：版本号改为 `2.0.2`，`EXTRAVERSION` 设为 `release`（`applications/n2e-gw/VERSION`）
- `871df2c` 在配置端口（UDP 8600）新增恢复出厂设置命令（`0x16`）：执行后擦除设置分区，清除全部已保存配置（RF24/IP/端口/DHCP/主机名等），成功时回复 `[0x16][1]` 并在 100ms 后冷重启使默认配置生效，擦除失败时回复 `[0]` 且不重启（`applications/n2e-gw/src/udp.c`、`applications/n2e-gw/include/n2e_gw.h`、`applications/n2e-gw/USER_GUIDE.md`、`applications/n2e-gw/README.md`）
- `2d6a522` 追加 v2.0.1 的变更记录文档（`applications/n2e-gw/CHANGELOG.md`）

## v2.0.1（2026-08-06）

- 发布标签：`v2.0.1-n2e-gw`
- 发布提交：`fc1edf5`
- 提交时间：`2026-08-06 20:00:18 +0800`

### 自仓库初始提交以来的改动

- `9d817d4` 手柄接收器应用初始版本：包含 RF24 无线接收、UDP 网络上报、LED 状态指示、
  配置与参数持久化、网络/RF24 shell 调试等模块，以及 nrf24_f103rct6 板子配置、
  设备树覆盖、MCUboot 签名密钥和 sysbuild 配置（`applications/n2e-gw/` 全目录）
- `bf89def` MCUboot 签名密钥路径改为按板子名变量引用
  `${APP_DIR}/boards/${BOARD}.pem`，避免硬编码板子名（`applications/n2e-gw/sysbuild.conf`）
- `fc1edf5` 发布手柄接收器 v2.0.1：版本号改为 `2.0.1`，`EXTRAVERSION` 设为 `release`（`applications/n2e-gw/VERSION`）
