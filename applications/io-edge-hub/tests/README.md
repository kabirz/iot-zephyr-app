# io-edge-hub 测试套件

针对 io-edge-hub 固件 (v3.4) 的功能 / 性能 / 压力测试, 覆盖 **Modbus TCP** / **UDP 配置** / **CAN** 三条通道.

## 依赖

```bash
pip install -r requirements.txt
# pymodbus>=3.5  Modbus TCP 客户端
# python-can>=4.0  Linux SocketCAN 后端
# pytest>=7.0    功能测试框架
```

## 环境配置

### Modbus TCP / UDP (默认)
- 设备 IP: `192.168.12.101` (或经环境变量 `IOEDGE_IP` 覆盖)
- Modbus TCP 端口: 502 (或 `IOEDGE_MB_PORT`)
- UDP 配置端口: 8600 (或 `IOEDGE_UDP_PORT`)

### Modbus RTU (RS485)
设备默认 9600bps 8N1, slave_id=1 (出厂值, 改后需重启设备生效). 需要 USB-RS485 适配器接到设备的 RS485 端子 (A/B).

环境变量:
- `IOEDGE_RTU_PORT` 默认 `/dev/ttyUSB0` (Linux USB-RS485)
- `IOEDGE_RTU_BAUDRATE` 默认 `9600`
- `IOEDGE_RTU_PARITY` 默认 `N`
- `IOEDGE_RTU_STOPBITS` 默认 `1`
- `IOEDGE_RTU_BYTESIZE` 默认 `8`
- `IOEDGE_RTU_SLAVE_ID` 默认 `1`

命令行覆盖 (优先级高于环境变量): `--rtu-port /dev/ttyUSB1 --rtu-baud 19200 --rtu-slave 1`

无串口时 RTU 测试自动 `skip` (不算失败).

### CAN (Linux SocketCAN)
设备默认 250kbps, 标准 11-bit 帧.

**真实 CAN 接口 (USB-CAN 适配器)**:
```bash
sudo ip link set can0 type can bitrate 250000
sudo ip link set can0 up
```

**虚拟 CAN (无硬件, 仅自测脚本链路)**:
```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set vcan0 up
# 然后: IOEDGE_CAN_CHANNEL=vcan0 pytest functional/test_can_*.py
```

环境变量:
- `IOEDGE_CAN_CHANNEL` 默认 `can0`
- `IOEDGE_CAN_INTERFACE` 默认 `socketcan`

## 目录结构

```
tests/
├── config.py                       设备 IP/端口/寄存器布局/CAN 协议常量
├── conftest.py                     pytest fixtures (modbus / udp / can)
├── common/
│   ├── modbus_client.py            pymodbus 包装 (FC01-FC16)
│   ├── udp_client.py               UDP 客户端 + 广播发现
│   ├── can_client.py               python-can 封装 + VERSION 查询
│   └── wait_helpers.py             等待重启 / 链路 up
├── functional/                     pytest, 单次 ~30s
│   ├── test_holding_rd_wr.py       18 个 holding 读 + 边界
│   ├── test_input_regs.py          6 个 input 读 + 版本字段
│   ├── test_coil_do.py             FC01/02/05/15 DO + LED
│   ├── test_timestamp_live.py      TIMESTAMP_HI/LO 实时性
│   ├── test_holding_side_effect.py CONFIG_SAVE/REBOOT/HISTORY_ENABLE
│   ├── test_modbus_multiclient.py  3 客户端 + 第 4 个被拒
│   ├── test_modbus_rtu.py          RTU FC03/04/06 + 与 TCP 一致性
│   ├── test_udp_commands.py        6 条 UDP 命令 + 错误路径
│   ├── test_udp_discover.py        GET_IP 广播发现
│   ├── test_can_version.py         CAN VERSION 查询
│   └── test_can_business.py        CAN 业务帧 ID
├── performance/                    独立脚本, 输出表格 + JSON
│   ├── bench_modbus_fc03.py        TCP FC03 RTT/QPS + 并发
│   ├── bench_modbus_rtu.py         RTU FC03 RTT/QPS (9600bps)
│   ├── bench_udp_rtt.py            UDP 命令 RTT 分布
│   └── bench_can_version.py        CAN VERSION RTT
└── stress/                         独立脚本, 长时间
    ├── stress_modbus_long.py       高频 FC03 持续 N 秒 (--transport tcp|rtu)
    ├── stress_udp_flood.py         多线程 UDP 命令风暴
    ├── stress_can_flood.py         业务帧 + VERSION 高频
    └── stress_mixed.py             Modbus + UDP 并行
```

## 运行

### 功能测试 (默认只读)
```bash
cd tests
pytest functional/ -v                       # 全部
pytest functional/test_holding_rd_wr.py -v  # 单文件
pytest functional/ -v -k "udp"              # 关键字筛选
pytest functional/ -v --can-channel vcan0   # 指定 CAN 通道
pytest functional/ -v --no-can              # 跳过所有 CAN 测试
```

### 启用写测试 (会修改设备参数, 推荐)
```bash
pytest functional/ -v --write               # 启用所有 write 标记测试
pytest functional/ -v --write -k "reboot"   # 只跑重启相关
```

写测试默认关闭, 防止误改设备参数 (DO/IP/REBOOT/FACTORY_RESET).

### 覆盖设备 IP
```bash
pytest functional/ -v --ip 192.168.1.50
# 或
IOEDGE_IP=192.168.1.50 pytest functional/ -v
```

### 跳过特定通道 (无硬件时)
```bash
pytest functional/ -v --no-can   # 跳过 CAN 测试 (无 SocketCAN 接口)
pytest functional/ -v --no-rtu   # 跳过 Modbus RTU 测试 (无 RS485 适配器)
```

CAN / RTU fixture 在底层接口不可用时**自动 skip**, 无需手动跳过.

### 性能测试
```bash
python performance/bench_modbus_fc03.py --duration 10 --json modbus_tcp.json
python performance/bench_modbus_rtu.py --port /dev/ttyUSB0 --count 100 --json modbus_rtu.json
python performance/bench_udp_rtt.py --count 500
python performance/bench_can_version.py --channel can0 --count 100
```

### 压力测试
```bash
# Modbus TCP (默认)
python stress/stress_modbus_long.py --transport tcp --duration 3600 --qps 200
# Modbus RTU (9600bps 上限约 10-20 QPS)
python stress/stress_modbus_long.py --transport rtu --port /dev/ttyUSB0 --slave 1 --duration 300

python stress/stress_udp_flood.py --duration 120 --threads 4
python stress/stress_can_flood.py --duration 60 --business-qps 500
python stress/stress_mixed.py --duration 600 --modbus-threads 2 --udp-threads 2
```

压力测试退出码: `0` = 通过 (错误率 < 阈值), `2` = 失败. `Ctrl+C` 优雅停止 + 输出累计统计.

## 测试覆盖矩阵

### Modbus TCP (pymodbus)
| 功能 | 文件 | 类型 |
|---|---|---|
| FC03 读 18 个 holding + 默认值 | `test_holding_rd_wr.py` | 功能 |
| FC04 读 6 个 input + 版本字段 | `test_input_regs.py` | 功能 |
| FC01/02/05/15 DO + LED | `test_coil_do.py` | 功能 (write) |
| TIMESTAMP_HI/LO 实时性 | `test_timestamp_live.py` | 功能 |
| CONFIG_SAVE/REBOOT/HISTORY | `test_holding_side_effect.py` | 功能 (write) |
| 3 客户端 + 第 4 个被拒 | `test_modbus_multiclient.py` | 功能 |
| FC03 RTT/QPS + 并发 | `bench_modbus_fc03.py` | 性能 |
| 长时间高频读 | `stress_modbus_long.py` | 压力 |

### UDP 配置端口
| 功能 | 文件 | 类型 |
|---|---|---|
| SET/GET IP/MODBUS/TIME | `test_udp_commands.py` | 功能 |
| 非法 IP 拒绝 (7 条规则) | `test_udp_commands.py` | 功能 (write) |
| GET_IP 广播发现 | `test_udp_discover.py` | 功能 |
| 命令 RTT 分布 | `bench_udp_rtt.py` | 性能 |
| 多线程命令风暴 | `stress_udp_flood.py` | 压力 |

### Modbus RTU (RS485)
| 功能 | 文件 | 类型 |
|---|---|---|
| FC03/04 读 + RTU/TCP 数据一致性 | `test_modbus_rtu.py` | 功能 |
| FC05/06 写 + 错误 slave_id | `test_modbus_rtu.py` | 功能 (write) |
| RTU RTT + QPS (9600bps) | `bench_modbus_rtu.py` | 性能 |
| RTU 长时间高频 | `stress_modbus_long.py --transport rtu` | 压力 |

### CAN (SocketCAN)
| 功能 | 文件 | 类型 |
|---|---|---|
| VERSION 查询 (0x101→0x102+0x105) | `test_can_version.py` | 功能 |
| 业务帧不崩溃 + ID 过滤 | `test_can_business.py` | 功能 (write) |
| VERSION RTT 分布 | `bench_can_version.py` | 性能 |
| 业务帧风暴 + VERSION 高频 | `stress_can_flood.py` | 压力 |

### 混合
| 功能 | 文件 | 类型 |
|---|---|---|
| Modbus + UDP 并行负载 | `stress_mixed.py` | 压力 |

## 注意事项

- **设备状态污染**: write 测试用 `restore_holding` fixture 在结束时恢复原值, 但 REBOOT/SET_IP/FACTORY_RESET 不可逆
- **多设备广播**: `test_udp_discover` 会发现同子网所有 io-edge-hub 设备
- **物理观察**: 部分 DO/LED 测试需要人眼确认物理输出 (LED 亮/继电器吸合)
- **CAN 总线**: 必须确保上位机与设备波特率一致 (250kbps); 总线需有 120Ω 终端电阻
- **Modbus RTU**: 设备 slave_id 和波特率从 holding 0x09/0x08 启动时读取, 修改后**需重启**生效; A/B 极性接反通信会失败, 颠倒即可
- **vcan 局限**: 虚拟 CAN 无真实设备, 仅用于链路层自测; 测试设备功能必须用物理 CAN 接口
