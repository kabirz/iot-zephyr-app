# 修复 libmodbus 默认 unit_id 问题的 LD_PRELOAD 劫持方案

> **适用对象**: 使用 libmodbus 库、但未调用 `modbus_set_slave()` 的 Modbus TCP 客户端程序
> **设备**: io-edge-hub (slave_id=1, Modbus TCP 端口 502)
> **日期**: 2026-08-16

---

## 目录

1. [问题现象](#1-问题现象)
2. [根因分析](#2-根因分析)
3. [排查过程](#3-排查过程)
4. [解决方案: LD_PRELOAD 劫持](#4-解决方案-ld_preload-劫持)
5. [使用说明](#5-使用说明)
6. [原理详解](#6-原理详解)
7. [为什么不能直接改二进制](#7-为什么不能直接改二进制)
8. [附录: Modbus 从站地址规范](#8-附录-modbus-从站地址规范)

---

## 1. 问题现象

一个编译好的 Modbus 测试程序 (`~/modbus`) 通过 TCP 连接设备, 连接成功但**每次写寄存器都失败**:

```
[Modbus Test] 正在连接 IO 采集卡 192.168.12.101:502...
[Modbus] 连接成功！开始每 5 秒交替切换 DO1 和 DO2...
[Test] 切换状态 -> DO1: 开, DO2: 关 (写入 Hex: 0x0001)
[Modbus] 写入寄存器失败: Slave device or server failure
```

TCP 连接建立正常, 但设备对每次写入都返回 Modbus 异常码 **4 (Server Device Failure)**。

## 2. 根因分析

程序使用 **libmodbus 库**发送请求, 但**没有调用 `modbus_set_slave()` 设置从站地址**。
libmodbus 的 TCP 客户端默认从站地址是 `MODBUS_TCP_SLAVE = 0xFF (255)`。

设备 (io-edge-hub) 的 Modbus TCP 服务器只接受与自身 `slave_id` (默认 1) 匹配的 unit_id
(广播 0 除外)。收到 unit_id=0xff 时, 固件在 `src/modbus/tcp.c` 中拒绝:

```c
/* unit_id 不匹配时 server 会丢帧不回复, 提前回异常避免等超时 */
if (req.unit_id != 0 && req.unit_id != srv_unit_id) {
    resp = req;
    modbus_raw_set_server_failure(&resp);   /* 返回异常码 4 */
    return reply_adu(client, &resp);
}
```

即: 设备端行为符合 Modbus 规范 (0xff 不是合法从站地址), **问题在客户端程序**未设置 slave_id。

## 3. 排查过程

### 3.1 strace 抓取网络调用字节

无 tcpdump/sudo 时, 用 strace 直接观察 `sendto`/`recvfrom` 的缓冲区内容:

```bash
strace -e trace=network -s 200 -x ~/modbus
```

输出:

```
sendto(3, "\x00\x01\x00\x00\x00\x06\xff\x06\x00\x00\x00\x01", 12, MSG_NOSIGNAL, NULL, 0) = 12
recvfrom(3, "\x00\x01\x00\x00\x00\x03\xff\x86", 8, 0, NULL, NULL) = 8
recvfrom(3, "\x04", 1, 0, NULL, NULL)   = 1
```

解析发送帧 `00 01 00 00 00 06 ff 06 00 00 00 01`:

| 字段 | 值 | 含义 |
|---|---|---|
| trans_id | 0x0001 | 事务 ID |
| proto_id | 0x0000 | 协议 ID (Modbus) |
| length | 0x0006 | 后续长度 |
| **unit_id** | **0xff** | **从站地址 = 255 (问题所在)** |
| FC | 0x06 | 写单个寄存器 |
| 地址 | 0x0000 | DO |
| 值 | 0x0001 | DO1 开 |

设备回复 `00 01 00 00 00 03 ff 86 04`: FC=0x86 (写异常), 异常码 0x04 = Server Device Failure。

### 3.2 gdb 定位 unit_id 来源

用 gdb 在 `send` 断点查看缓冲区来源与调用栈:

```bash
gdb -batch -ex "set breakpoint pending on" -ex "break send" -ex "run" \
    -ex "x/12bx \$rsi" -ex "bt 5" ~/modbus
```

输出:

```
Breakpoint 1, send () from /usr/lib/libc.so.6
0x7fffffffd914:  0x00  0x01  0x00  0x00  0x00  0x06  0xff  0x06 ...
#1  0x00007ffff7f781b0 in ?? () from /usr/lib/libmodbus.so.5
#3  0x0000555555555635 in main () at ../modbus/main.cpp:47
```

**结论**: `0xff` 由 **libmodbus 库**填入 (默认 `MODBUS_TCP_SLAVE`), 主程序 `main.cpp` 未调
`modbus_set_slave()`, 库调用链是 `main.cpp:47 → libmodbus modbus_write_register → send`。

## 4. 解决方案: LD_PRELOAD 劫持

由于程序**没有源码** (只有二进制), 且 `0xff` 在动态库 libmodbus 内部 (改主程序二进制无效),
最安全的方案是用 **LD_PRELOAD 劫持 `modbus_connect`**: 连接成功后自动调用
`modbus_set_slave(ctx, 1)` 把从站地址设为设备实际值。

### 劫持库源码 (fix_modbus_slave.c)

```c
/*
 * LD_PRELOAD: 修复 libmodbus 客户端默认 unit_id=0xff 的问题
 *
 * 有些程序用 libmodbus 但没调 modbus_set_slave(), 默认 unit_id=0xff
 * 不是合法从站地址, 设备 (slave_id=1) 返回 Server Device Failure.
 * 劫持 modbus_connect: 连接成功后强制 modbus_set_slave(ctx, 1).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <modbus/modbus.h>

int modbus_connect(modbus_t *ctx)
{
	static int (*real_connect)(modbus_t *) = NULL;

	if (!real_connect) {
		real_connect = dlsym(RTLD_NEXT, "modbus_connect");
	}
	int rc = real_connect(ctx);

	if (rc == 0) {
		modbus_set_slave(ctx, 1); /* 设备实际 slave_id=1 */
	}
	return rc;
}
```

### 编译

```bash
gcc -shared -fPIC -o fix_modbus_slave.so fix_modbus_slave.c -ldl -lmodbus
```

## 5. 使用说明

```bash
LD_PRELOAD=/path/to/fix_modbus_slave.so ~/modbus
```

验证: 程序不再报错, DO1/DO2 正常交替切换:

```
[Modbus Test] 正在连接 IO 采集卡 192.168.12.101:502...
[Modbus] 连接成功！开始每 5 秒交替切换 DO1 和 DO2...
[Test] 切换状态 -> DO1: 开, DO2: 关 (写入 Hex: 0x0001)
[Test] 切换状态 -> DO1: 关, DO2: 开 (写入 Hex: 0x0002)
```

> 若目标设备 slave_id 不是 1, 修改源码中的 `modbus_set_slave(ctx, 1)` 参数即可。

## 6. 原理详解

- **LD_PRELOAD**: 动态链接器优先加载指定共享库, 其中定义的符号会**覆盖**后续库的同名符号。
- **RTLD_NEXT**: `dlsym(RTLD_NEXT, "modbus_connect")` 获取真实 (原库) 的 `modbus_connect` 函数指针, 避免递归调用自己。
- **劫持时机**: 在真实 `modbus_connect` 成功返回后调用 `modbus_set_slave(ctx, 1)`。
  `modbus_set_slave` 只设置 `ctx->slave` 字段, 在连接前后调用均可, 因此能安全覆盖默认的 0xff。
- **为什么要劫持 connect 而不是 write_register**: `modbus_connect` 只调用一次, 每个连接设置一次即可;
  劫持 write_register 需在每个帧构造前设置, 冗余且有状态风险。

## 7. 为什么不能直接改二进制

- 程序 `~/modbus` 是动态链接的, 它的 `0xff` 来自 **libmodbus.so 内部**的 `MODBUS_TCP_SLAVE` 常量,
  **不在主程序二进制里**。直接改 `~/modbus` 文件字节无效。
- 主程序二进制中有 378 个 `0xff` 字节, 无法安全定位并修改那一个 (对应 unit_id)。
- 修改系统动态库 `libmodbus.so` 会影响所有使用它的程序, 风险大且不可维护。

**LD_PRELOAD 方案的优势**: 不改动任何现有文件, 只加载一个附加库, 针对单个程序生效, 可随时移除。

## 8. 附录: Modbus 从站地址规范

| 地址 | 用途 |
|---|---|
| **1-247** | 合法从站地址 |
| **0** | 广播地址 (所有从站执行, 不返回应答) |
| **248-255** | 保留 (协议扩展预留) |
| **255** | 不是合法从站地址 |

libmodbus 的 `MODBUS_TCP_SLAVE = 0xFF` 仅是库的**默认占位值** (表示"未设置"),
所有基于 libmodbus 的客户端**必须**调用 `modbus_set_slave()` 设置真实从站地址。

---

## 相关文件

- 本方案源码: 见 `docs/fix_modbus_slave.c` (或按需放置于项目 tools 目录)
- 设备固件 unit_id 校验: `applications/io-edge-hub/src/modbus/tcp.c`
