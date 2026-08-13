# io-edge-hub 固件升级工具

提供两种实现, 功能对齐, 协议相同 (与固件 `libs/udp_fw_upgrade` + `libs/can_fw_upgrade` 对齐):

| 实现 | 文件 | 适用场景 |
|---|---|---|
| **Python** | `firmware_upgrade.py` | 跨平台 (Linux/Win/macOS), 易改易调试, 零编译 |
| **C (Linux)** | `firmware_upgrade.c` + `Makefile` | 单二进制 30KB, 零运行时依赖, 启动快, 适合 CI/运维脚本 |

支持两条升级通道:
- **UDP** (端口 8600, `FW_START/DATA/END`) — 跨子网, 常规运维
- **CAN** (帧 `0x101-0x105`, Linux SocketCAN) — UDP 不可达时备用

## 特性

- **MCUboot 镜像解析**: 自动校验 magic + 提取 KEYHASH TLV (32B SHA-256)
- **keyhash 预校验**: 升级前发到设备, 设备对比 `fw_keyhash.h`, 不匹配立即拒绝 (防错误密钥固件刷入)
- **CRC16-CCITT**: 与 Zephyr `crc16_ccitt` 完全一致 (poly 0x1021, init 0x0000, bit-reflected)
- **进度条**: 纯文本 (无 tqdm 依赖)
- **测试模式**: `--test` 升级后重启但不永久, MCUboot 下次启动自动回滚
- **结构化退出码**: 0=成功 / 1=镜像错误 / 2=通信失败 / 3=设备拒绝

## 依赖

### Python 版
- Python 3.7+, 标准库 (UDP 通道零依赖)
- CAN 通道额外: `pip install python-can`

### C 版
- Linux + glibc + SocketCAN 内核模块
- 构建: `cmake -B build && cmake --build build` (需 CMake ≥ 3.10)
- 运行测试: `cd build && ctest`
- 安装: `cmake --install build --prefix /usr/local` (默认 `/usr/local/bin`)
- 静态链接分发: `cmake -B build -DCMAKE_EXE_LINKER_FLAGS=-static`
- 运行时零依赖 (动态链接二进制 ~30KB, 仅 libc)

## 用法

### Python 版

```bash
python firmware_upgrade.py upgrade -i 192.168.12.101 -f zephyr.signed.bin
python firmware_upgrade.py upgrade -c can0 -f zephyr.signed.bin
python firmware_upgrade.py version -i 192.168.12.101
python firmware_upgrade.py upgrade -i 192.168.12.101 -f app.signed.bin --test
```

### C 版

```bash
cmake -B build && cmake --build build         # 配置 + 编译
cd build && ctest                              # 跑烟雾测试
./build/firmware_upgrade upgrade -i 192.168.12.101 -f zephyr.signed.bin
./build/firmware_upgrade upgrade -c can0 -f zephyr.signed.bin
./build/firmware_upgrade version -i 192.168.12.101
./build/firmware_upgrade upgrade -i 192.168.12.101 -f app.signed.bin --test
sudo cmake --install build --prefix /usr/local # 安装到 /usr/local/bin
```

## 退出码

| 码 | 含义 | 适用 |
|---|---|---|
| 0 | 升级成功 | upgrade / version |
| 1 | 参数/镜像错误 | upgrade (非 MCUboot 镜像, 文件不存在, 等) |
| 2 | 通信失败 | upgrade / version (设备无响应, 超时) |
| 3 | 设备拒绝 | upgrade (keyhash 不匹配, CRC 错误, 存储不足) |

## 升级流程详解

### UDP 通道 (端口 8600)

```
[1/4] FW_START (0x01)
        发: [size LE32][keyhash 32B 可选]
        等: 5s (设备擦 slot1 外部 flash)
        回: [status] (0=失败 1=成功 2=keyhash 不匹配)

[2/4] FW_DATA (0x02) × N
        发: [data ≤511B]
        回: [offset LE32] (流控, 客户端可校验非回退)
        进度: 0-90%

[3/4] FW_END (0x03)
        发: [test 1B][crc16 LE16]
        等: 10s (设备 flush + 按 64B 读回重算 CRC)
        回: [result] (0=失败 1=成功)

[4/4] 设备自动重启 → MCUboot SWAP_SCRATCH 交换镜像
```

### CAN 通道 (帧 0x101-0x105)

```
[1/4] keyhash (0x104) × 5
        发: data[0]=seq(0..4), data[1..7]=7B  (5 帧拼 32B SHA-256)
        无回复 (固件累积)

[2/4] START_UPDATE (0x101 cmd=0, arg=size)
        等: 10s (设备校验 keyhash + 擦 slot1)
        回: 0x102 code=OFFSET(0) arg=0  或  KEYHASH_ERROR(6) / FLASH_ERROR(4)

[3/4] FW_DATA (0x103) × N
        发: 8B/帧
        每 8 帧 (64B) 设备回 0x102 OFFSET 做流控
        全部写完回 0x102 UPDATE_SUCCESS(1)
        进度: 0-90%

[4/4] CONFIRM (0x101 cmd=1, arg=permanent?1:0)
        设备 boot_request_upgrade
        回: 0x102 CONFIRM(3) arg=0x55AA55AA
        设备下次重启 → MCUboot SWAP
```

## 镜像格式

脚本接受 MCUboot 标准签名镜像 (`zephyr.signed.bin`, 由 `west build` 生成):

```
[magic 4B: 0x96F3B83D]
[hdr_size 2B][pad 2B]
[img_size 4B]
[flags 4B]
... (header)
[镜像数据 img_size B]
[TLV info magic 2B: 0x6907][TLV 总长 2B]
[TLV 区: KEYHASH(0x01) 32B + SHA256(0x02) + SIGNATURE(0x03) ...]
```

工具自动提取 `KEYHASH TLV` 用于升级前预校验. 无 TLV 的旧镜像可用 `--no-keyhash` 跳过.

## 错误排查

| 现象 | 可能原因 |
|---|---|
| `设备无响应 (timeout)` | 设备未开机 / IP 错误 / 防火墙阻断 8600 端口 |
| `keyhash 不匹配` | 镜像签名密钥与设备 `boards/*.pem` 不一致 (重生成镜像或核对密钥) |
| `FW_END 失败 (CRC 不匹配)` | 网络丢包导致数据损坏 (UDP 应重传, 但本工具未实现); 检查链路质量 |
| `flash 擦除失败` | 外部 W25Q128 故障或 storage 分区损坏 |
| CAN `等待 0x102 超时` | 波特率不匹配 (设备 250kbps), 终端电阻缺失, A/B 极性反 |
| `文件过短, 不像 MCUboot 镜像` | 传错文件 (应传 `zephyr.signed.bin` 而非 `zephyr.bin`) |

## 注意事项

- **MCUboot 回滚**: 测试模式 (`--test`) 升级后, 下次重启 MCUboot 会自动回滚到旧固件. 永久升级不要加 `--test`
- **keyhash 强制**: 默认 `require_keyhash=True`. 用 `--no-keyhash` 跳过会失去"防错误密钥"保护, 仅用于无签名 TLV 的旧镜像
- **CAN 总线负载**: 数据流每 64B 等一次 OFFSET 回复, 不会压垮总线. 但若总线有其他高优先级流量, OFFSET 超时可能触发 (工具给 5s, 通常足够)
- **UDP 单次升级**: 工具不重传丢包, 网络不可靠时建议用 CAN 通道 (有 64B 流控 + CRC 最终校验)
- **Python vs C 选择**: Python 版适合开发/调试 (异常信息更友好), C 版适合部署 (无 Python 环境的运维机)

