# eth-bwtest — 阿波罗 V2 STM32H743 以太网使能 + 带宽测试

针对 `apollo_v2_h743iit6` 板 (STM32H743IIT6 + YT8512C PHY, RMII) 的
网卡点亮与带宽验证例程。静态 IP `192.168.12.200/24`, 网线直连开发机
`enp3s0` (假设为 `192.168.12.100/24`, 不同请改 `prj.conf` 与
`src/bench_shell.c` 的 `BENCH_HOST_IP_DEF`)。

板 DTS 已使能: MAC (RMII) + MDIO + `motorcomm,yt8512` PHY 驱动
(`apps/drivers/yt8512`, PHY 复位经 PCF8574 P7) + USART1 控制台,
以及 QSPI/SDRAM/EEPROM/ADC/DAC/PWM/TF 卡/RTC 等存储外设 (对应驱动
需各自 CONFIG 才会编入, 本 app 未开, 不占 RAM)。

## 实测结果 (2026-09-04, 链路协商 100M 全双工)

### zperf × iperf2 联测 (app 已内置 `CONFIG_NET_ZPERF=y`)

> 注意: **zperf 只兼容 iperf v2 协议** (README 明说 "Compatible with
> iPerf v2.0.10 and newer")。iperf3 需要先建 TCP 控制连接协商, 与 zperf
> 不兼容 —— 主机侧要用 `iperf`(v2) 而不是 `iperf3`。本机 Arch 的
> `iperf` 包即 iperf2; 免 root 安装:
> `mkdir ~/iperf2-loc && cd ~/iperf2-loc && curl -LO <镜像>/iperf-2.2.1-*-x86_64.pkg.tar.zst && tar -xf *.zst && ./usr/bin/iperf --version`

| 方向 | 板端命令 | 主机命令 | 板端速率 | 主机速率 | 质量 |
|---|---|---|---|---|---|
| 下行 UDP | `zperf udp download 5001` | `iperf -c 192.168.12.200 -u -b 95M -t 10 -l 1400` | **95.71 Mbps** | 95.6 Mbps (Server Report 95.7) | 丢 2/85364, jitter 466µs |
| 上行 UDP | `zperf udp upload 192.168.12.100 5001 10 1400 95M` | `iperf -s -u` | 44.75 Mbps | 44.8 Mbps | **0 丢失/0 乱序**, jitter 146µs |
| 下行 TCP | `zperf tcp download 5001` | `iperf -c 192.168.12.200 -t 10` | — | 19~23 Mbps | 受 Zephyr TCP 接收窗口限制 |
| 上行 TCP | `zperf tcp upload 192.168.12.100 5001 10 1400 95M` | `iperf -s` | 47.88 Mbps | 47.8 Mbps | 0 错误 |

iperf2 UDP client 还能收到板端回传的 **Server Report** (速率/丢包/抖动),
两个方向的数字完全对得上。

zperf server 踩坑记录: `CONFIG_ZVFS_POLL_MAX` 默认 3 不够 (zperf 的
TCP+UDP 接收服务要 4+1 个 poll 表项), 会导致 "Cannot register socket
service handler", prj.conf 已设为 10。zperf 内部日志需同时开
`CONFIG_NET_LOG=y` 才可见。

### 自带测试工具 (bwtest/bench)

| 测试 | 方向 | 结果 |
|---|---|---|
| ping | — | RTT 0.18~0.37 ms, 0% 丢包 |
| TCP sink 200MB (`tcp_send.py -m 200`) | 主机→板 | **94.97 Mbit/s (百兆线速)** |
| UDP 1442B 帧 ×1000 (`bench tx 1000 1400`) | 板→主机 | **1000/1000 送达, 66.7 Mbit/s** |
| UDP 1066B 帧 ×1000 (`bench tx 1000 1024`) | 板→主机 | 1000/1000 送达, 61 Mbit/s |
| UDP 554B 帧 ×1000 (`bench tx 1000 512`) | 板→主机 | 1000/1000 送达, 47 Mbit/s |
| UDP 洪泛 10s (`udp_flood.py -t 10 -s 1400` + `bench rx`) | 主机→板 | 主机线速 95.6 Mbit/s 发送, 板端实收 59993/85329 包 (~70%), 稳定 5000 pkt/s, 最长空轮询 2 ms |
| TCP echo (`tcp_send.py --echo`) | 双向 | **已知问题, 见下** |

结论: 网卡双向硬件路径均达到百兆线速; UDP 下行用 zperf 接收器
(socket service 专用线程) 可做到 0.002% 丢包线速接收, 比 bench rx
轮询式接收 (~70%) 强; TCP 下行用应用层紧循环 sink 可跑满 95M,
zperf TCP 接收 (~20M) 与 UDP/上行发送 (~45M) 受 Zephyr 协议栈
处理能力限制。

## 编译 / 烧录 / 串口

```shell
source ~/.venv/zephyr/bin/activate
cd <west 顶层>            # 即本仓库 apps/ 所在的 west workspace
west build -b apollo_v2_h743iit6 apps/examples/eth-bwtest
west flash                # openocd + ST-LINK, 默认 runner
```

串口 (/dev/ttyUSB0, CH340C, 115200)。**注意**: CH340 的 RTS/DTR
连接着 BOOT0/RESET (一键下载电路)。打开串口必须带 `--rts 0 --dtr 0`;
即便如此, 打开瞬间仍可能有 ~100ms 毛刺把板子复位 (小概率直接进
bootloader, 症状: ping 不通且无控制台输出)。

**可靠的重启+观察流程** (实测最稳):

```shell
# 1) 先开串口 (毛刺复位一次无所谓)
python -m serial.tools.miniterm /dev/ttyUSB0 115200 --rts 0 --dtr 0
# 2) 串口保持打开 (RTS/DTR 稳定为低 = BOOT0 低), 用 ST-LINK 复位
st-flash --connect-under-reset reset
# → 板子必定从 flash 启动, miniterm 里能看到完整启动日志
```

若板子已进 bootloader (无 ping / 无控制台): 关掉所有占用串口的进程,
然后 `st-flash --connect-under-reset reset` 即可恢复。

## 预期启动日志

```
phy_motorcomm_yt8512: PHY (0) ID:0x128
phy_motorcomm_yt8512: PHY (0) Link speed 100 Mb, full duplex
eth_bwtest: network up, IPv4 192.168.12.200
bwtest: BW test TCP server on port 9900 ('R' = sink, else echo)
```

红灯 100ms 闪烁 = 网络未就绪; 绿灯 1s 闪烁 = 已连接。

PHY 驱动若报 `reg3=0x... is not a YT8512` 或 `MDIO read failed`,
先在 shell 执行 `mdio scan` 找到实际 PHY 地址, 再改板 DTS 中
`ethernet-phy@0` 的 `reg` (本板实测地址 0, ID 0x00000128 =
Type_No 0x12 Rev 8, 与 datasheet 一致)。

## 带宽测试用法

### 1. 连通性

```shell
ping 192.168.12.200
```

板端 shell: `net iface` / `net stats` 查看链路与收发统计。

### 2. TCP 下行 (主机 → 板, 板端 RX 带宽)

```shell
python3 apps/examples/eth-bwtest/host/tcp_send.py 192.168.12.200 -m 200
```

主机打印发送速率, 板端日志在连接结束时打印 `BW sink done: ...
(... Mbit/s)`。

### 3. UDP 下行 (主机 → 板, 洪泛 + 丢包)

板端 shell: `bench rx 19601 10000`, 主机 10 秒内:

```shell
python3 apps/examples/eth-bwtest/host/udp_flood.py 192.168.12.200 -t 10 -s 1400
```

板端 `done/rate` 与主机 `host TX` 对比即丢包率; `longest empty poll`
反映收包实时性。

### 4. UDP 上行 (板 → 主机)

板端 shell: `bench tx 1000 1400 192.168.12.100 19602`,
主机用任意 UDP 收包工具统计 (如 python socket bind 19602)。

## 已知问题

- **TCP 回环 (双向) 异常**: echo 模式下板端 `zsock_send` 持续重传后
  `send failed: 105 (ENOBUFS)`, 仅 ~8% 数据送达, 板端伴随
  `eth_stm32_hal: Failed to obtain RX buffer` (app 阻塞在 send 导致
  recv 停止、RX 池耗尽的连锁症状)。对照实验: 同尺寸 UDP 大帧
  (1442B) 100% 送达、TCP 纯收流 (sink, 小包 ACK 反向线速) 95Mbit/s
  正常 —— 排除硬件/TX 路径, 指向本仓库 Zephyr fork (v4.4.2-27) 的
  TCP 发送路径问题 (疑与 `NET_BUF_DATA_SIZE=1536` 大缓冲下 TCP
  重传队列/buffer 归还交互有关), 待跟进。`ETH_STM32_HAL_TX_ASYNC`
  开关均复现。
- **主机侧 echo 测试必须持续读回包** (`tcp_send.py --echo` 已内置
  读线程): 只发不读会造成双向零窗口死锁, 测出 0.1 Mbit/s 量级的
  假慢速。
- SDK 自带 openocd (hla_swd) 对睡眠中的 H7 读寄存器会返回垃圾值
  (PARTNO unrecognized), 属调试域时钟门控, 不影响烧录。

## 主机侧网卡配置

开发机 `enp3s0` 需与板同网段 (本仓库既有惯例 `192.168.12.100/24`):

```shell
sudo ip addr add 192.168.12.100/24 dev enp3s0   # 若尚未配置
ip link set enp3s0 up
```

## 常见问题

- **板子 ping 不通 + 控制台无输出**: 多半被串口打开毛刺复位进了
  bootloader, 按"可靠的重启+观察流程"恢复。
- **PHY 无应答**: `mdio scan` 查实际地址; 检查底板 P4 (VREF) 跳线与
  网口 LINK 指示灯。
- **`bench tx` size 上限 1472** (UDP payload 最大值), 超出会被钳位。
