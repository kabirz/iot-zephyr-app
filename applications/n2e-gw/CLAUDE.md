# CLAUDE.md -- n2e-gw

## 项目愿景

n2e-gw 是运行在 STM32F103RCT6 (ARM Cortex-M3, 72MHz, 256KB Flash, 48KB RAM) 上的 Zephyr RTOS 嵌入式应用，作为 **数据中转网关**，在 angle-handler（手持控制器）与上位机之间转发数据。

- **版本**: v0.1.0_<6hex> (格式 `v<M>.<m>.<p>_<6位git commit>`)
- **硬件平台**: nrf24_f103rct6 (网关板，独立 PCB)
- **Bootloader**: MCUBoot (swap-with-scratch 模式)
- **无线模块**: Nordic nRF24L01+ (SPI2，2.4GHz，中断驱动)
- **网络芯片**: Wiznet W5500 (SPI3 以太网)
- **许可证**: Apache-2.0

---

## 单配置架构

网关板硬件：nRF24L01+ (SPI2) ↔ W5500 以太网 (SPI3)，**无 CAN，无模块电源管理**。nRF24 接收到的数据通过 W5500 UDP 转发给上位机，反向亦然。

网络是唯一数据通路，配置直接写在 `prj.conf`，无需 snippet 或 Kconfig 开关切换。

---

## 编译

```shell
west build -b nrf24_f103rct6 applications/n2e-gw --sysbuild
west flash
```

---

## 硬件引脚分配

| 功能 | 引脚 | 总线 | 说明 |
|------|------|------|------|
| nRF24 SCK/MISO/MOSI | PB13/PB14/PB15 | SPI2 | 8MHz |
| nRF24 CS / CE / IRQ | PB12 / PA9 / PC6 | SPI2 | nRF24L01P |
| W5500 SCK/MISO/MOSI | PB3/PB4/PB5 | SPI3 | 8MHz |
| W5500 CS | PA15 | SPI3 | W5500 片选 |
| W5500 INT / RST | PD2 / PC12 | — | 中断 / 复位 |

> **SPI3 JTAG 释放**：PB3/PB4/PA15 是 JTAG 引脚 (JTDO/JNTRST/JTDI)。Zephyr STM32 pinctrl 在使用 SPI3_REMAP0 时自动设置 `AFIO_MAPR_SWJ_CFG` 释放 JTAG（保留 SWD），无需手动 Kconfig。

> **无电源管理**：网关板所有模块常供电，无软件控制的电源使能 GPIO。

---

## 数据流

```
angle-handler --nRF24--> n2e-gw --UDP--> host (上位机)
host       --UDP-->  n2e-gw --nRF24--> angle-handler
```

---

## 帧协议

帧 ID 定义在 `n2e_gw.h::enum can_ids`（复用历史 CAN 11-bit 编号，已与 CAN 总线无关，仅作逻辑标识符）：

| 帧 ID | 名称 | 方向 | 用途 |
|-------|------|------|------|
| 0x1E3 | `HANDLER_STATE` | 手柄→网关 | 手柄状态 (X/Y BE + 按键) |
| 0x263 | `OVERBREAK_LASER` | 网关→手柄 | 超欠挖 + 激光测距 |
| 0x363 | `COORD_XY` | 网关→手柄 | X/Y 坐标 |
| 0x463 | `COORD_Z` | 网关→手柄 | Z 坐标 |
| 0x763 | `COBID_HEATBEAT` | 手柄→网关 | 心跳 |
| 0x777 | `TEST_FRAME` | 双向 | rf24 shell 测试帧 (ping/echo/data) |

---

## UDP 协议

### 双端口架构

| 端口 | 用途 | 可配 | 默认 |
|------|------|------|------|
| **数据端口** | nRF24→上位机数据转发 + 上位机→nRF24 扫描仪数据透传 | 默认值，持久化后可改 | 9600 |
| **配置端口** | 所有配置命令（网络参数/RF24 参数/HOST 目标/重启/固件升级） | Kconfig `CONFIG_UDP_FW_CONFIG_PORT` | 8600 |

- **双端口均支持广播收发**（详见下方 Zephyr 广播约束）。
- 配置端口绑定 `0.0.0.0:8600`，支持广播接收（上位机不知道设备 IP 时可广播 `DISCOVER` 发现设备）。
- **数据转发目标固定**：nRF24 数据固定单播到上位机 `host_ip:host_port`（默认 `192.168.11.150:9602`，通过配置端口 `UDP_CMD_SET_HOST` 配置，持久化），不再广播/学习发送方。
  - 数据端口：`gw_udp_send` 固定发往上位机 host_ip:host_port。
  - 配置端口：`config_send_resp` 按发送方 IP 判断。
- 两个 socket + 两个线程独立工作，配置流量不会劫持数据流量目标地址。

### Zephyr 广播约束（v4.4.0 实测）

- **必须绑定 `INADDR_ANY` (0.0.0.0)**：`connection.c::conn_addr_cmp` 会比较 socket 本地地址与包目的地址；绑定具体 IP 时，广播包（dst=`255.255.255.255`）会因 `具体IP ≠ 255.255.255.255` 被丢弃。我们两个 socket 都用部分初始化器（`.sin_port=...`，`sin_addr` 隐式为 0），正好踩中正确绑定方式。
- **`SO_BROADCAST` 是死代码**：`zsock_setsockopt` 未处理该选项，返回 `ENOPROTOOPT`。发送路径对 `255.255.255.255` 无任何限制，发广播不需要它。
- **只能用全局 `255.255.255.255`**（limited broadcast）：IPv4 接收门 (`ipv4.c:365-378`) 只对它放行 UDP；子网定向广播（如 `192.168.1.255`）接收不可靠。
- **广播不跨路由器**：协议本性，跨子网设备发现需走预配/主动单播上报，不在本项目范围内。

### 帧格式

- **数据帧**: `[帧 ID 2B BE][payload]`（透传扫描仪/手柄数据，走数据端口）
- **命令帧**: `[cmd 1B][data...]`（无魔数头，走配置端口）
- 固件升级命令 0x01~0x05（开始/数据/结束/查询版本/重启，由 `udp_fw_upgrade` 库内部处理）
- 网络参数命令 0x10/0x11（SET_IP/GET_NET）；RF24 参数命令 0x12/0x13（SET_RF24/GET_RF24）；上位机目标命令 0x14（SET_HOST）；广播发现 0x15（DISCOVER）
- **上位机目标命令 0x14（SET_HOST）**：配置 nRF24 数据转发目标 `host_ip:host_port`（默认 `192.168.11.150:9602`，持久化），不再广播/学习发送方；查询用 GET_NET（0x11）即可覆盖
- **广播回复限制**：配置端口支持广播接收，但跨子网广播回复会淹没子网内所有设备。`udp_fw_upgrade` 库新增 `CONFIG_UDP_FW_REPLY_BCAST_RESTRICT`（默认 `y`）开关：开启时仅对 `udp_fw_allow_broadcast_cmd()` 放行的命令做广播回复（n2e-gw 仅放行 DISCOVER，即"设备发现"），其余命令跨子网接收时静默丢弃；关闭时恢复任意命令广播回复的旧行为。GET_NET/SET_IP 需定向单播（上位机先 DISCOVER 拿到设备 IP）。
- **`0x14 SET_HOST`** 是固定单播目标配置（不涉及广播），不受上述限制影响。
- 静态模式掩码固定 255.255.255.0，网关 = 设备 IP 末段改 1（运行时派生，不传输）；DHCP 模式（`use_dhcp`，持久化重启生效）由服务器分配，DISCOVER 回复 live interface 地址

### 命令响应格式

- **0x10 SET_IP** 响应: `[0x10][1B: 1=成功/0=失败]`（DHCP 模式或 IP 非法时回 0）
- **0x11 GET_NET** 响应: `[0x11][data_port 2B][host_ip 4B][host_port 2B]`（8 字节；配置端口不在响应中返回，由 DISCOVER 带出）
- **0x13 GET_RF24** 响应: `[0x13][rf24_addr 5B]`（5 字节；信道固定为 1，不在帧中返回）
- **0x15 DISCOVER** 响应: `[0x15][ip 4B][config_port 2B]`（6 字节，IP 取自 live interface；上位机广播发现设备 IP + 配置端口）
- **0x04 GET_VERSION** 响应: `[0x04][版本字符串]`（格式 `v<M>.<m>.<p>_<6hex>`，如 `v0.1.0_0b4ee3`，不含末尾 `\0`）

---

## 关键设计决策

- **单一网络通路**：无 CAN，无模式切换 (linksw)，网络 (W5500 UDP) 是唯一数据通路。`connect_type` 字段已移除。
- **网络配置直写 prj.conf**：`CONFIG_NETWORKING`/`NET_IPV4`/`NET_UDP`/`NET_SOCKETS`/`POSIX_API`/`NET_L2_ETHERNET`/`NET_ARP` 直接写在 `prj.conf`。`ETH_DRIVER` 由 `NET_L2_ETHERNET` 自动拉起，`ETH_W5500` 由 devicetree W5500 节点自动拉起。
- **SPI 分离**：nRF24 用 SPI2，W5500 用 SPI3，各自独立 CS，避免总线共享。
- **STM32F103 无硬件 RNG**：网络栈随机源用 `CONFIG_TEST_RANDOM_GENERATOR`。
- **W5500 MAC 由 UID 派生**：设备树 `local-mac-address`（`00:08:DC:01:02:03`）只是默认/回退值；`main.c::net_init` 在 `net_if_up` 前用 `hwinfo_get_device_id()` 读 STM32 96-bit UID，前 3B 沿用 Wiznet OUI `00:08:DC`，末 3B 由 12B UID 折叠而来，通过 `net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS)` 覆盖（同时更新 W5500 SHAR 寄存器 + `net_if` link_addr），保证每块板 MAC 唯一，避免 ARP 冲突。需 `CONFIG_HWINFO=y` + `CONFIG_NET_L2_ETHERNET_MGMT=y`。
- **固件升级走 UDP**：`udp_fw_upgrade` 库内置固件升级命令 (0x01~0x03)，通过 UDP 接收固件写入 slot1。
- **帧 ID 复用历史编号**：`enum can_ids` 保留原 CAN 11-bit 编号作为 UDP/nRF24 帧的逻辑标识符，上位机协议兼容。

---

## 源码结构

```
n2e-gw/
  boards/
    nrf24_f103rct6.overlay   -- 板级覆盖（nRF24 SPI2 + W5500 SPI3 + RTC）
  include/n2e_gw.h          -- 公共定义（帧 ID 枚举、配置参数、n2e_gw_params_t）
  src/
    main.c                   -- 入口 + 网络初始化 (W5500 静态 IP)
    rf24.c                   -- nRF24L01P 收发（接收数据无条件走 UDP）
    rf24_shell.c             -- rf24 shell 测试命令（info/ch/addr/send/ping/listen/diag）
    udp.c                    -- UDP 透传 + 配置 + 固件升级
    config.c                 -- 配置加载（settings_load 封装）
    persist.c                -- Settings 持久化（gw/* 键，FCB 后端）
  CMakeLists.txt             -- 源文件列表
  Kconfig                    -- 线程栈/优先级配置
  prj.conf                   -- 应用配置（含网络栈）
  sysbuild.conf              -- MCUBoot sysbuild（签名密钥 ${APP_DIR}/boards/nrf24_f103rct6.pem）
  sysbuild/mcuboot.conf
  VERSION
```

### MCUboot 签名密钥（每应用独立）

`sysbuild.conf` 用 `${APP_DIR}/boards/nrf24_f103rct6.pem` 指向**本应用目录**下的密钥（不是板子目录的共享密钥）。这样 n2e-gw 和 angle-handler（共用同一块板 `nrf24_f103rct6`）**各自一把独立 RSA-2048 密钥**，互不通用：

- 一个 mcuboot 镜像只能验签它自己嵌入公钥对应的应用，**不能跨应用引导**（n2e-gw 烧的 bootloader 无法启动 angle-handler 镜像，反之亦然）。
- `${APP_DIR}` 在 sysbuild 作用域展开为 `west build` 指定的应用源目录（`zephyr/share/sysbuild/CMakeLists.txt`），每个 app 编译时各取各的。
- 密钥重新生成：`python bootloader/mcuboot/scripts/imgtool.py keygen -k <app>/boards/nrf24_f103rct6.pem -t rsa-2048`（改密钥后 mcuboot 和 app 必须重新烧写）。

### 线程清单

| 线程名 | 函数 | 栈/优先级 | 说明 |
|--------|------|-----------|------|
| `thread_rf24_rx` | `rf24_rx_thread` | 1024 / 8 | nRF24 接收，转发到数据端口 |
| `thread_udp_data_rx` | `udp_data_rx_thread` | 1024 / 10 | 数据端口 (9600) 接收，扫描仪数据透传到 nRF24 |
| `thread_udp_config_rx` | `udp_config_rx_thread` | 1024 / 10 | 配置端口 (8600) 接收，命令/固件升级分发 |

---

## AI 使用指引

- **无 CAN、无电源管理、无模式切换**：历史的双配置/CAN/snippet/linksw/电源管理代码已全部移除。
- **修改引脚**：全部在 `boards/nrf24_f103rct6.overlay`。
- **网络配置**：直接改 `prj.conf`，不涉及 Kconfig 开关。
- **UDP 双端口**：数据端口 (默认 9600, 可配) + 配置端口 (默认 8600, Kconfig 可配, 支持广播)。改端口分工看 `udp.c` 头注释。
- **修改帧协议**：同步更新 `n2e_gw.h::enum can_ids`，并与 angle-handler 保持一致（共用协议）。
- **`n2e_gw_params_t gw_params`**（定义在 `main.c`）是全局共享状态：RF24 配置、网络配置、运行标志。
- 与 angle-handler 的关系：angle-handler 是手柄端（采集+发送），n2e-gw 是接收/中转端，通过 nRF24 无线连接。
