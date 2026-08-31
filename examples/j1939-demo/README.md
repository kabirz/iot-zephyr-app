# j1939-demo

Zephyr J1939 应用示例:主线 Zephyr 没有内置 J1939 协议栈,
本工程在原生 CAN API 之上自建了一个演示级协议栈,把
`io_edge_f407vet6` 板子作为一个 J1939 "IO 计测节点"运行。

## 功能

- **地址声明** (J1939/81,简化):Request 探询 + 0.5~1.5s 竞争窗口
  + NAME 数值仲裁,地址被占时让步到更低地址;地址被更强节点抢占后自动重声明
- **周期广播** PGN 65280 (0xFF00,proprietary B):8 字节 IO 快照
  (16 路 DI 位图 + 2 路 AI 电压,模拟值)
- **TP BAM 多包传输** (J1939/21):每 N 帧用 TP.CM/TP.DT 广播
  16 字节扩展快照 PGN 65281,演示 >8 字节报文分片
- **Request 应答** (PGN 59904):应答对 65280/65281 及地址声明的请求
- 29 位 ID 编解码 / PDU1-PDU2 寻址 / PGN 软件分发

## 目录结构

```
├── CMakeLists.txt
├── Kconfig            # 首选地址 / 广播周期 / BAM 频率
├── prj.conf
└── src/
    ├── j1939.h        # 协议常量、ID 编解码、API
    ├── j1939.c        # 收发 / PGN 分发 / 地址声明
    ├── j1939_tp.c     # TP BAM 多包发送
    └── main.c         # 应用: 模拟 IO 广播 + Request 应答
```

## 构建与烧录

```shell
west build -b io_edge_f407vet6 examples/j1939-demo
west flash
```

无需 overlay:板级 DTS 已将 can1 配为 250 kbps(J1939/11 标准速率)。

## 总线观测 (Linux + SocketCAN)

```shell
ip link set can0 type can bitrate 250000
ip link set can0 up
candump -a can0
```

SA=128 (0x80) 时的典型流量:

```
can0  18EEFF80   [8]  00 28 00 00 2B F1 4A 9A   # Address Claimed (NAME)
can0  18FF0080   [8]  01 59 13 ...              # PGN 65280 IO 快照 (1s 周期)
can0  1CECFF80   [8]  20 10 00 03 FF 01 FF 00   # TP.CM BAM: 16B/3包 → PGN 0xFF01
can0  1CEBFF80   [8]  01 xx xx xx xx xx xx xx   # TP.DT #1..#3 (间隔 55ms)
```

主动请求节点的 IO 状态(以 SA=200 (0xC8) 的身份发 Request):

```shell
cansend can0 18EA80C8#00FF00    # 请求 PGN 65280, 节点会立即回发一帧
```

## Python 上位机 (host/j1939_host.py)

零依赖 (纯标准库 SocketCAN), 与固件侧逻辑对称: 相同的 ID 编解码、
同样的简化地址声明 (SA 默认 200)、BAM 收包重组、演示 PGN 解码。

```shell
ip link set can0 type can bitrate 250000 && ip link set can0 up

# 声明地址并监听 30s, 期间向节点 (SA=128) 请求 65280 / 65281
./host/j1939_host.py -i can0 -t 30 --req 65280 --req 65281

# 纯监听 (不声明地址, 不参与仲裁)
./host/j1939_host.py -i can0 --no-claim -t 0
```

典型输出:

```
[  0.712] address claimed: SA=200
[  0.914] 18EEFF80 Address Claimed SA=128  NAME func=40 ... mfg=1234 identity=0xaf12b
[  1.201] 18FF0080 PGN 65280 (IO Status) SA=128  seq=1 DI=0x1359 AI0=1500mV AI1=1503mV
[  5.023] 1CECFF80 TP.CM BAM SA=128: 16 bytes / 3 pkts -> PGN 65281
[  5.135] PGN 65281 (IO Status Ext) [BAM 完成] SA=128  seq=5 DI=0x1359 AI=1500mV/... uptime=5s
```

无硬件时可用 vcan 干跑: `ip link add dev vcan0 type vcan && ip link set vcan0 up`。

Wireshark 自带 J1939 解析器(对 can0 抓包即可按 PGN/SPN 解码);
can-utils 的 `j1939spy` / `j1939acd` 等工具也可配合使用。

## 配置项

| Kconfig | 默认 | 说明 |
|---|---|---|
| `J1939_DEMO_PREFERRED_ADDRESS` | 128 | 首选源地址(避开 0=发动机等行业约定地址) |
| `J1939_DEMO_TX_PERIOD_MS` | 1000 | IO 快照广播周期 |
| `J1939_DEMO_BAM_EVERY_N` | 5 | 每 N 帧快照后发一次 BAM 扩展报文 |

## 已知简化 (按需扩展)

- TP 仅实现发送侧 BAM;收包重组、RTS/CTS 流控未实现
- 地址声明未实现完整 TR1~TR7 定时器组,竞争窗口为简化时序
- NAME 为编译期固定值;正式产品应由 MCU UID 派生 Identity Number
- 未实现 J1939/73 诊断 (DM1/DM2) 与 J1939/74 刷写;对不支持的
  Request 不回 ACK/NACK (PGN 59392)
- 总线 bus-off 未做恢复处理

## 参考

- SAE J1939 规范族:/11 物理层、/21 传输层、/71 应用层、
  /73 诊断、/74 配置、/81 网络管理、J1939-DA (PGN/SPN 数据库)
- Wireshark J1939 dissector、Linux 内核 AF_CAN J1939
