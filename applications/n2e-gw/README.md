# n2e-gw - 数据中转网关

运行在 STM32F103RCT6 上的 Zephyr RTOS 嵌入式应用，作为 angle-handler（手持控制器）与上位机之间的数据中转网关。通过 nRF24L01+ 无线接收手柄数据，经 W5500 以太网 UDP 转发给上位机。

## 编译

```bash
west build -b nrf24_f103rct6 applications/n2e-gw --sysbuild
west flash
```

## 硬件

### 组件

| 组件 | 型号 | 接口 |
|------|------|------|
| MCU | STM32F103RCT6 | ARM Cortex-M3 72MHz, 256KB Flash, 48KB RAM |
| 无线 | nRF24L01+ | SPI2 (8MHz) |
| 以太网 | Wiznet W5500 | SPI3 (18MHz) |

### 引脚分配

| 功能 | 引脚 | 总线 | 说明 |
|------|------|------|------|
| nRF24 SCK/MISO/MOSI | PB13/PB14/PB15 | SPI2 | 8MHz |
| nRF24 CS / CE / IRQ | PB12 / PA9 / PC6 | SPI2 | nRF24L01P |
| W5500 SCK/MISO/MOSI | PB3/PB4/PB5 | SPI3 | 8MHz |
| W5500 CS | PA15 | SPI3 | 片选 |
| W5500 INT / RST | PD2 / PC12 | — | 中断 / 复位 |
| LED rf24 | PA1 | GPIO | 2.4G 活动灯 (平时常亮, 收发时 5Hz 闪烁) |
| LED err | PA3 | GPIO | 错误灯 (栈溢出/hardfault 点亮锁定) |
| LED sys | PA2 | GPIO | 系统灯 (进入主循环点亮) |

> PB3/PB4/PA15 是 JTAG 引脚，Zephyr pinctrl 使用 SPI3 时自动释放 JTAG（保留 SWD）。

> 网关板所有模块常供电，无软件控制的电源使能 GPIO。

## 数据流

- 上行: angle-handler → nRF24 → n2e-gw → UDP → 上位机
- 下行: 上位机 → UDP → n2e-gw → nRF24 → angle-handler

## 帧协议

帧 ID 定义在 `n2e_gw.h::enum can_ids`（复用历史 CAN 11-bit 编号，仅作逻辑标识符）：

| 帧 ID | 名称 | 方向 | 用途 |
|-------|------|------|------|
| 0x1E3 | HANDLER_STATE | 手柄→网关 | 手柄状态 (X/Y + 按键) |
| 0x263 | OVERBREAK_LASER | 网关→手柄 | 超欠挖 + 激光测距 |
| 0x363 | COORD_XY | 网关→手柄 | X/Y 坐标 |
| 0x463 | COORD_Z | 网关→手柄 | Z 坐标 |
| 0x763 | COBID_HEATBEAT | 手柄→网关 | 心跳 |

## UDP 协议

### 双端口

| 端口 | 用途 | 可配 | 默认 |
|------|------|------|------|
| **数据端口** | nRF24↔上位机数据转发 (本机监听) | 默认值，持久化后可改 | 9600 |
| **配置端口** | 所有配置命令 + 固件升级 | Kconfig `CONFIG_UDP_FW_CONFIG_PORT` | 8600 |

- **数据转发目标固定**：nRF24 数据固定单播到上位机 `host_ip:host_port`（默认 `192.168.11.150:9602`，`UDP_CMD_SET_HOST` 可配，持久化），不再广播/学习发送方。
- **配置端口支持广播接收**：上位机不知道设备 IP 时可向 `255.255.255.255:8600` 广播 `DISCOVER` 命令发现设备；回复同子网发送方单播，跨子网广播。
- **广播命令限制**（`CONFIG_UDP_FW_REPLY_BCAST_RESTRICT`，默认开）：仅放行的命令跨子网（广播）接收时才处理并回复，其余静默丢弃，避免广播淹没子网内所有设备。n2e-gw 仅放行 `DISCOVER`（设备发现）；`GET_NET`/`SET_IP` 需上位机先 `DISCOVER` 拿到设备 IP 后定向单播。

### 数据帧格式
`[帧 ID 2B BE][payload]` (透传手柄/扫描仪数据，走数据端口)

### 命令帧格式
`[cmd 1B][data...]` (无魔数头，走配置端口 8600)

### 配置命令

业务命令从 0x10 起；0x01-0x05 由 `udp_fw_upgrade` 库内部处理 (FW_START/DATA/END/GET_VERSION/REBOOT)。

> **升级 keyhash 校验（默认开启）**：`udp_fw_upgrade` 库在编译期由 `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE` 派生 32B keyhash（SHA-256 of RSA 公钥 PKCS#1 DER，即镜像 `IMG_TLV_KEYHASH`）。新上位机可在 0x01 FW_START 后追加该 keyhash，不一致时回状态 2 拒绝且不擦写 flash；老上位机发不带 keyhash 的旧 4B 帧仍放行，兼容旧协议。

> **网络参数**：静态模式下掩码固定 `255.255.255.0`，网关 = 设备 IP 末段改 1 (`a.b.c.x` → `a.b.c.1`)，均由固件运行时派生，不在帧中传输。DHCP 模式下 IP/掩码/网关由 DHCP 服务器分配，`DISCOVER` 回复的 IP 取自 live interface（DHCP 模式下上位机广播 `DISCOVER` 发现设备，回复带 `[ip 4B][config_port 2B]`）。配置端口由 Kconfig `CONFIG_UDP_FW_CONFIG_PORT` 决定（默认 8600），由 `DISCOVER` 回复带出，不在 `GET_NET` 中重复。

| 命令 | 格式 | 说明 |
|------|------|------|
| 0x01 | `[0x01][size 4B LE][keyhash 32B]` | 开始固件升级 (库处理, 回 `[0x01][1/0/2]`；`[keyhash 32B]` 可选，携带时校验不一致回 2 拒绝) |
| 0x02 | `[0x02][data ≤511B]` | 固件数据 (库处理, 回 `[0x02][offset 4B LE]`) |
| 0x03 | `[0x03][test 1B][crc 2B LE]` | 结束固件升级并重启 (库处理, 回 `[0x03][1/0]`) |
| 0x04 | `[0x04]` | 查询版本 (库处理, 回 `v<M>.<m>.<p>_<6hex>` 变长, 如 `v0.1.0_0b4ee3`) |
| 0x05 | `[0x05]` | 重启设备 (库处理) |
| 0x10 | `[0x10][ip 4B]` | 设置静态 IP (持久化, 重启生效; IP 有效性校验: 拒绝 0.0.0.0/127.x/224+/255.255.255.255; DHCP 模式拒绝; 回 `[0x10][1=成功/0=失败]`) |
| 0x11 | `[0x11]` | 查询网络参数 (回 8B: `[data_port 2B][host_ip 4B][host_port 2B]`) |
| 0x12 | `[0x12][rf24_addr 5B]` | 设置 RF24 地址 (信道固定为 1; 持久化, 即时应用到硬件; 回 5B 设置后的值) |
| 0x13 | `[0x13]` | 查询 RF24 地址 (回 5B: `[rf24_addr 5B]`) |
| 0x14 | `[0x14][host ip 4B][port 2B BE]` | 设置 nRF24 数据转发目标 host_ip:host_port (持久化; 回 6B 回显) |
| 0x15 | `[0x15]` | 广播发现 (回 6B: `[ip 4B][config_port 2B]`，IP 取自 live interface; 用于上位机跨子网发现设备 IP + 配置端口) |
| 0x16 | `[0x16]` | 恢复出厂设置 (擦除全部持久化配置 rf24/ip/port/dhcp/host; 回 `[0x16][1=成功/0=失败]`; 擦除成功后自动冷重启, 默认配置即时生效) |

## Shell 命令

```
rf24 info                       查看 addr/device 状态 (信道固定为 1)
rf24 addr <b0 b1 b2 b3 b4>      设置 5 字节地址
rf24 send <text...>             发送 DATA 帧
rf24 ping [count=5] [iv_ms=200] ping/echo 往返测试, 统计 RTT
rf24 listen [on|off]            切换监听 (打印 DATA + 自动回 ECHO)
rf24 diag                       打印 nRF24 硬件寄存器快照

gw info                         查看网络配置 (IP/掩码/网关/端口)
gw ip <addr>                    设置静态 IP (持久化, 重启生效)
gw port <1-65535>               设置数据端口 (持久化, 重启生效)
gw reset                        清除所有 settings (rf24/ip/port/dhcp), 重启后恢复默认
```

## 资源占用

| FLASH | RAM |
|-------|-----|
| 156 KB / 190 KB (81.8%) | 40 KB / 48 KB (84.3%) |

## MAC 地址

设备树 `local-mac-address`（`00:08:DC:01:02:03`）为默认/回退值。`main.c::net_init` 在接口 up 前用 `hwinfo_get_device_id()` 读 STM32 96-bit UID，前 3B 沿用 Wiznet OUI `00:08:DC`，末 3B 由 12B UID 异或折叠而来，通过 `net_mgmt(SET_MAC_ADDRESS)` 覆盖（同时更新 W5500 SHAR 寄存器与 `net_if` link_addr），保证每块板 MAC 唯一，避免 ARP 冲突。需 `CONFIG_HWINFO=y` + `CONFIG_NET_L2_ETHERNET_MGMT=y`。

## 目录结构

```
n2e-gw/
  boards/
    nrf24_f103rct6.overlay  -- 板级覆盖 (nRF24 SPI2 + W5500 SPI3 + RTC)
  include/n2e_gw.h          -- 公共定义 (帧 ID 枚举、配置参数)
  src/
    main.c                   -- 入口 + 网络初始化 (W5500 静态 IP)
    led.c                    -- 三路状态灯 (PA1 rf24 活动 / PA2 sys / PA3 err)
    rf24.c                   -- nRF24L01P 收发
    rf24_shell.c             -- rf24 shell 测试命令
    udp.c                    -- UDP 透传 + 配置 + 固件升级
    config.c                 -- 配置管理
    persist.c                -- Settings 持久化
  CMakeLists.txt             -- 源文件列表
  Kconfig                    -- 线程栈/优先级配置
  prj.conf                   -- 应用配置 (含网络栈)
  VERSION
  CLAUDE.md
  README.md
```
