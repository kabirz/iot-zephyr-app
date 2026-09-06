#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""极简 MCUmgr SMP over UDP 客户端 (apollo-v2 固件升级用)。

因网络所限无法安装官方 mcumgr CLI (Go), 按 SMP 协议规范手写实现,
仅覆盖固件升级所需命令。协议参考:
  include/zephyr/mgmt/mcumgr/transport/smp.h (帧头 8 字节)
  include/zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h (op/group)
  include/zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h (ID_STATE=0/ID_UPLOAD=1)
  include/zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt.h (ID_RESET=5)

用法:
  smp_udp.py <host> list                     # 查询镜像/槽位状态
  smp_udp.py <host> upload <signed.bin>      # 上传镜像到 slot1
  smp_udp.py <host> test                     # 标记 pending 为试运行 (复位后 swap, 不确认则回滚)
  smp_udp.py <host> confirm                  # 确认当前运行镜像
  smp_udp.py <host> reset                    # 复位重启
  smp_udp.py <host> fswrite <local> <remote> # 上传文件到设备 fs (如字库)
可选: --port 1337 --chunk 512 --timeout 2.0
"""
import argparse
import hashlib
import os
import socket
import struct
import sys
import time

# ---- mcumgr 常量 ----
MGMT_OP_READ = 0
MGMT_OP_READ_RSP = 1
MGMT_OP_WRITE = 2
MGMT_OP_WRITE_RSP = 3

MGMT_GROUP_OS = 0
MGMT_GROUP_IMAGE = 1
MGMT_GROUP_FS = 8

IMG_MGMT_ID_STATE = 0
IMG_MGMT_ID_UPLOAD = 1
OS_MGMT_ID_RESET = 5
FS_MGMT_ID_FILE = 0


# ---------------- 极简 CBOR ----------------
def cbor_uint(major, value):
    if value < 24:
        return bytes([(major << 5) | value])
    if value < 0x100:
        return bytes([(major << 5) | 24, value])
    if value < 0x10000:
        return bytes([(major << 5) | 25]) + value.to_bytes(2, "big")
    return bytes([(major << 5) | 26]) + value.to_bytes(4, "big")


def cbor_encode(obj):
    """支持: dict(text 键), list, bytes, str, 0..2^32-1, bool, None"""
    if obj is True:
        return b"\xf5"
    if obj is False:
        return b"\xf4"
    if obj is None:
        return b"\xf6"
    if isinstance(obj, int):
        assert 0 <= obj < 2 ** 32
        return cbor_uint(0, obj)
    if isinstance(obj, bytes):
        return cbor_uint(2, len(obj)) + obj
    if isinstance(obj, str):
        raw = obj.encode()
        return cbor_uint(3, len(raw)) + raw
    if isinstance(obj, list):
        return cbor_uint(4, len(obj)) + b"".join(cbor_encode(i) for i in obj)
    if isinstance(obj, dict):
        out = cbor_uint(5, len(obj))
        for k, v in obj.items():
            out += cbor_encode(k) + cbor_encode(v)
        return out
    raise TypeError(f"cbor: 不支持的类型 {type(obj)}")


def cbor_decode(buf, pos=0):
    """返回 (值, 新偏移)。支持上面编码器产出的全部类型 + 不定长
    map/array (SMP 响应用 0xbf/0x9f ... 0xff 形式)。"""
    ib = buf[pos]
    major, info = ib >> 5, ib & 0x1F
    pos += 1

    def read_len(info, pos):
        if info < 24:
            return info, pos
        if info == 24:
            return buf[pos], pos + 1
        if info == 25:
            return int.from_bytes(buf[pos:pos + 2], "big"), pos + 2
        if info == 26:
            return int.from_bytes(buf[pos:pos + 4], "big"), pos + 4
        if info == 27:
            return int.from_bytes(buf[pos:pos + 8], "big"), pos + 8
        raise ValueError("cbor: 长度过大")

    if major == 0:
        val, pos = read_len(info, pos)
        return val, pos
    if major in (2, 3):
        n, pos = read_len(info, pos)
        raw = bytes(buf[pos:pos + n])
        return (raw if major == 2 else raw.decode(errors="replace")), pos + n
    if major == 4:
        if info == 31:		# 不定长 array
            out = []
            while buf[pos] != 0xFF:
                v, pos = cbor_decode(buf, pos)
                out.append(v)
            return out, pos + 1
        n, pos = read_len(info, pos)
        out = []
        for _ in range(n):
            v, pos = cbor_decode(buf, pos)
            out.append(v)
        return out, pos
    if major == 5:
        if info == 31:		# 不定长 map
            out = {}
            while buf[pos] != 0xFF:
                k, pos = cbor_decode(buf, pos)
                v, pos = cbor_decode(buf, pos)
                out[k] = v
            return out, pos + 1
        n, pos = read_len(info, pos)
        out = {}
        for _ in range(n):
            k, pos = cbor_decode(buf, pos)
            v, pos = cbor_decode(buf, pos)
            out[k if not isinstance(k, str) else k] = v
        return out, pos
    if major == 7:
        if info == 20:
            return False, pos
        if info == 21:
            return True, pos
        if info == 22:
            return None, pos
    raise ValueError(f"cbor: 不支持的 initial byte 0x{ib:02x}")


# ---------------- SMP 传输 ----------------
class SmpClient:
    def __init__(self, host, port, timeout):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        self.addr = (host, port)
        self.timeout = timeout
        self.seq = 0
        self.retries = 10

    def request(self, op, group, cmd_id, payload):
        body = cbor_encode(payload)
        hdr = struct.pack(">BBHHBB", op, 0, len(body), group, self.seq, cmd_id)
        frame = hdr + body
        for attempt in range(self.retries):
            self.sock.sendto(frame, self.addr)
            try:
                resp, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                print(f"  超时重试 {attempt + 1}/{self.retries}", file=sys.stderr)
                continue
            r_op, _flags, r_len, r_group, r_seq, r_id = struct.unpack(
                ">BBHHBB", resp[:8])
            if r_group != group or r_id != cmd_id or r_seq != self.seq:
                continue  # 串包/迟到帧, 重收
            self.seq = (self.seq + 1) & 0xFF
            value, _ = cbor_decode(resp[8:8 + r_len])
            if not isinstance(value, dict):
                raise RuntimeError(f"响应不是 map: {value!r}")
            return value
        raise RuntimeError("设备无响应 (检查 IP/端口/固件)")

    def read(self, group, cmd_id, payload={}):
        return self.request(MGMT_OP_READ, group, cmd_id, payload)

    def write(self, group, cmd_id, payload={}):
        return self.request(MGMT_OP_WRITE, group, cmd_id, payload)


# ---------------- 命令 ----------------
def cmd_list(c, _args):
    r = c.read(MGMT_GROUP_IMAGE, IMG_MGMT_ID_STATE)
    err = r.get("rc")
    if err:
        print(f"错误 rc={err}")
        return 1
    for i, img in enumerate(r.get("images", [])):
        ver = img.get("version", "?")
        slot = img.get("slot")
        state = []
        if img.get("active"):
            state.append("active")
        if img.get("confirmed"):
            state.append("confirmed")
        if img.get("pending"):
            state.append("pending")
        h = img.get("hash")
        hs = h.hex()[:12] if isinstance(h, bytes) else "-"
        print(f"slot{slot}: v{ver:<12} {'/'.join(state) or '-':<24} hash {hs}")
    return 0


def cmd_upload(c, args):
    with open(args.file, "rb") as f:
        data = f.read()
    total = len(data)
    sha = hashlib.sha256(data).digest()
    print(f"上传 {args.file}: {total} 字节, sha256 {sha.hex()[:16]}...")

    off = 0
    t0 = time.time()
    first = True
    while off < total:
        end = min(off + args.chunk, total)
        payload = {"off": off, "data": data[off:end]}
        if first:
            payload["len"] = total
            payload["sha"] = sha
            first = False
        r = c.write(MGMT_GROUP_IMAGE, IMG_MGMT_ID_UPLOAD, payload)
        err = r.get("rc")
        if err:
            print(f"\n上传失败 rc={err} (off={off})")
            return 1
        ack = r.get("off", off)
        if ack == off:
            # 设备要求原地重发 (可能要求更小的分片)
            if args.chunk > 128:
                args.chunk //= 2
                print(f"\n设备请求缩小分片 -> {args.chunk}")
                continue
            raise RuntimeError(f"设备未推进偏移 (off={off})")
        off = ack
        pct = off * 100 // total
        print(f"\r  {pct:3d}%  {off}/{total}", end="", flush=True)
    dt = time.time() - t0
    print(f"\n完成: {total} 字节 / {dt:.1f}s = {total / max(dt, 0.01) / 1024:.0f} KB/s")
    return cmd_list(c, None)


def cmd_test(c, _args):
    r = c.write(MGMT_GROUP_IMAGE, IMG_MGMT_ID_STATE, {"test": True})
    if r.get("rc"):
        print(f"test 失败 rc={r['rc']} (slot1 有镜像吗?)")
        return 1
    print("已标记试运行 (pending), 复位后 MCUboot swap, 不确认将回滚")
    return 0


def cmd_confirm(c, _args):
    r = c.write(MGMT_GROUP_IMAGE, IMG_MGMT_ID_STATE, {"confirm": True})
    if r.get("rc"):
        print(f"confirm 失败 rc={r['rc']}")
        return 1
    print("当前镜像已确认")
    return 0


def cmd_reset(c, _args):
    c.write(MGMT_GROUP_OS, OS_MGMT_ID_RESET, {})
    print("复位指令已发送")
    return 0


def cmd_fswrite(c, args):
    """上传文件到设备文件系统 (fs_mgmt), 如字库 /SD:/font.bin"""
    with open(args.file, "rb") as f:
        data = f.read()
    remote = args.remote
    total = len(data)
    print(f"fswrite {args.file} -> {remote}: {total} 字节")
    off = 0
    first = True
    chunk = 512
    t0 = time.time()
    t_last = 0.0
    fails = 0
    while off < total:
        end = min(off + chunk, total)
        payload = {"name": remote, "off": off, "data": data[off:end]}
        if first:
            payload["len"] = total
            first = False
        try:
            r = c.write(MGMT_GROUP_FS, FS_MGMT_ID_FILE, payload)
        except RuntimeError:
            r = {"rc": -99}
        err = r.get("rc")
        if err:
            # SD 写在 IT 模式下遇负载会偶发失败 (V1 教训 #1 场景):
            # fs_mgmt 会话支持断点续传, 从最后确认偏移重试
            fails += 1
            if fails > 200:
                print(f"\n重试超限放弃 (off={off}, rc={err})")
                return 1
            time.sleep(0.2)
            c = SmpClient(c.addr[0], c.addr[1], c.timeout)
            continue
        ack = r.get("off", off)
        if ack == off:
            if chunk > 128:
                chunk //= 2
                continue
            raise RuntimeError("设备未推进偏移")
        off = ack
        print(f"\r  {off*100//total}%", end="", flush=True)
        if args.rate:
            dt_min = 1.0 / args.rate
            dt_now = time.time() - t_last
            if dt_now < dt_min:
                time.sleep(dt_min - dt_now)
        t_last = time.time()
    dt = time.time() - t0
    print(f"\n完成: {total} 字节 / {dt:.1f}s (重试 {fails} 次)")
    return 0


def main():
    ap = argparse.ArgumentParser(description="极简 SMP over UDP 客户端")
    ap.add_argument("host")
    ap.add_argument("command",
                   choices=["list", "upload", "test", "confirm", "reset", "fswrite"])
    ap.add_argument("file", nargs="?", help="upload/fswrite: local file")
    ap.add_argument("remote", nargs="?", help="fswrite: remote path e.g. /SD:/font.bin")
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--chunk", type=int, default=512, help="每包数据字节数")
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("--rate", type=int, default=0,
                    help="fswrite 限速 (packets/s, 0=不限; 板子供电余量不足时"
                         "用 ~50 摊薄 QSPI 擦写电流突发)")
    args = ap.parse_args()

    if args.command == "upload" and not args.file:
        ap.error("upload 需要文件参数")
    if args.command == "fswrite" and (not args.file or not args.remote):
        ap.error("fswrite 需要 <本地文件> <设备路径> 参数")

    c = SmpClient(args.host, args.port, args.timeout)
    rc = {
        "list": cmd_list,
        "upload": cmd_upload,
        "test": cmd_test,
        "confirm": cmd_confirm,
        "reset": cmd_reset,
        "fswrite": cmd_fswrite,
    }[args.command](c, args)
    sys.exit(rc or 0)


if __name__ == "__main__":
    main()
