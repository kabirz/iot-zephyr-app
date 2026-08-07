#!/usr/bin/env python3
"""
UDP 性能测试工具 — 配合 n2e-gw 网关 gw bench 命令使用

测试模式 (从 PC 视角):
  rx      PC 接收设备发送的 UDP 包      设备端: gw bench tx
  tx      PC 向设备发送 UDP 包          设备端: gw bench rx
  bidir   双向同时收发                  设备端: gw bench bidir

数据包格式: [seq 4B LE][0xAA 填充], 收方可据 seq 检测丢包.

示例:
  # 设备 TX 吞吐 (PC 收包, 设备: gw bench tx 500 64)
  python udp_bench.py --host 192.168.11.100 --mode rx

  # 设备 RX 吞吐 (PC 发包 1000 x 64B, 设备: gw bench rx)
  python udp_bench.py --host 192.168.11.100 --mode tx --count 1000

  # 双向同时收发 10 秒 (设备: gw bench bidir 10000 64 19601)
  python udp_bench.py --host 192.168.11.100 --mode bidir --duration 10
"""

import argparse
import socket
import struct
import sys
import threading
import time

DEFAULT_HOST = "192.168.11.100"
DEFAULT_SPORT = 19601   # 设备收包端口 (bench rx/bidir, 避开业务端口)
DEFAULT_RPORT = 19602   # PC 收包端口 (bench tx 目标, 避开业务端口)
DEFAULT_SIZE = 64
DEFAULT_COUNT = 1000
DEFAULT_DURATION = 5
PKT_HEAD = struct.Struct("<I")   # 4B LE seq


def make_payload(seq, size):
    buf = bytearray(size)
    PKT_HEAD.pack_into(buf, 0, seq & 0xFFFFFFFF)
    for i in range(4, size):
        buf[i] = 0xAA
    return bytes(buf)


def fmt_rate(pkts, byts, elapsed_ms):
    if elapsed_ms <= 0:
        return "0 pkt/s, 0 B/s"
    pps = pkts * 1000 // elapsed_ms
    bps = byts * 1000 // elapsed_ms
    return f"{pps} pkt/s, {bps} B/s ({bps / 1024:.1f} KB/s)"


def do_rx(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.rport))
    sock.settimeout(0.5)

    print(f"[RX] listen :{args.rport}  {args.duration}s")
    print(f"     设备端执行: gw bench tx 500 {args.size} <PC_IP> {args.rport}")

    pkts = 0
    total = 0
    lost = 0
    last_seq = -1
    t0 = time.monotonic()
    deadline = t0 + args.duration
    next_print = t0 + 1

    while time.monotonic() < deadline:
        try:
            data, _ = sock.recvfrom(2048)
        except socket.timeout:
            continue
        pkts += 1
        total += len(data)
        if len(data) >= 4:
            seq = PKT_HEAD.unpack(data[:4])[0]
            if last_seq >= 0 and seq > last_seq + 1:
                lost += seq - last_seq - 1
            last_seq = seq

        now = time.monotonic()
        if now >= next_print:
            print(f"  ... {now - t0:.0f}s: {pkts} pkts, {total} B")
            next_print = now + 1

    elapsed = time.monotonic() - t0
    sock.close()

    print(f"\n--- RX 结果 ---")
    print(f"收到: {pkts} 包, {total} B, 耗时 {elapsed:.1f}s")
    print(f"速率: {fmt_rate(pkts, total, int(elapsed * 1000))}")
    if pkts > 0 and lost > 0:
        expected = pkts + lost
        print(f"丢包: {lost} ({lost * 100 / expected:.1f}%)")
    elif pkts > 0:
        print("丢包: 0 (无间隙)")


def do_tx(args):
    dst = (args.host, args.sport)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    interval = args.interval / 1000.0 if args.interval > 0 else 0

    print(f"[TX] {args.count} pkts x {args.size}B -> {args.host}:{args.sport}")
    print(f"     设备端执行: gw bench rx {args.sport}")

    payload = bytearray(args.size)
    for i in range(4, args.size):
        payload[i] = 0xAA

    t0 = time.monotonic()
    for i in range(args.count):
        PKT_HEAD.pack_into(payload, 0, i)
        sock.sendto(payload, dst)
        if interval > 0:
            time.sleep(interval)

    elapsed = time.monotonic() - t0
    sock.close()

    total = args.count * args.size
    print(f"\n--- TX 结果 ---")
    print(f"发送: {args.count} 包, {total} B, 耗时 {elapsed:.1f}s")
    print(f"速率: {fmt_rate(args.count, total, int(elapsed * 1000))}")


def do_bidir(args):
    res = {"tx_pkts": 0, "tx_fail": 0, "rx_pkts": 0, "rx_bytes": 0, "rx_lost": 0}
    stop = threading.Event()

    def sender():
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        dst = (args.host, args.sport)
        payload = bytearray(args.size)
        for i in range(4, args.size):
            payload[i] = 0xAA
        seq = 0
        while not stop.is_set():
            PKT_HEAD.pack_into(payload, 0, seq)
            try:
                sock.sendto(payload, dst)
                res["tx_pkts"] += 1
            except OSError:
                res["tx_fail"] += 1
            seq += 1
        sock.close()

    def receiver():
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", args.rport))
        sock.settimeout(0.5)
        last_seq = -1
        while not stop.is_set():
            try:
                data, _ = sock.recvfrom(2048)
            except socket.timeout:
                continue
            res["rx_pkts"] += 1
            res["rx_bytes"] += len(data)
            if len(data) >= 4:
                seq = PKT_HEAD.unpack(data[:4])[0]
                if last_seq >= 0 and seq > last_seq + 1:
                    res["rx_lost"] += seq - last_seq - 1
                last_seq = seq
        sock.close()

    print(f"[BIDIR] TX->{args.host}:{args.sport}  RX on :{args.rport}  "
          f"{args.duration}s  {args.size}B/pkt")
    print(f"        设备端执行: gw bench bidir {int(args.duration * 1000)} {args.size} {args.sport}")

    t_rx = threading.Thread(target=receiver, daemon=True)
    t_tx = threading.Thread(target=sender, daemon=True)

    t0 = time.monotonic()
    t_rx.start()
    t_tx.start()

    next_print = t0 + 1
    while time.monotonic() - t0 < args.duration:
        time.sleep(0.2)
        now = time.monotonic()
        if now >= next_print:
            print(f"  ... {now - t0:.0f}s: TX {res['tx_pkts']}, RX {res['rx_pkts']} pkts")
            next_print = now + 1

    stop.set()
    t_tx.join(timeout=2)
    t_rx.join(timeout=2)

    elapsed = time.monotonic() - t0
    em = int(elapsed * 1000)
    tx_bytes = res["tx_pkts"] * args.size

    print(f"\n--- BIDIR 结果 ---")
    print(f"TX: {res['tx_pkts']} 发送, {res['tx_fail']} 失败, {tx_bytes} B")
    print(f"RX: {res['rx_pkts']} 包, {res['rx_bytes']} B")
    print(f"耗时: {elapsed:.1f}s")
    print(f"TX 速率: {fmt_rate(res['tx_pkts'], tx_bytes, em)}")
    print(f"RX 速率: {fmt_rate(res['rx_pkts'], res['rx_bytes'], em)}")
    if res["rx_pkts"] > 0 and res["rx_lost"] > 0:
        expected = res["rx_pkts"] + res["rx_lost"]
        print(f"RX 丢包: {res['rx_lost']} ({res['rx_lost'] * 100 / expected:.1f}%)")


def main():
    p = argparse.ArgumentParser(
        description="n2e-gw UDP 性能测试工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
测试模式 (PC 视角):
  rx      PC 收包, 测设备 TX 吞吐     设备: gw bench tx
  tx      PC 发包, 测设备 RX 吞吐     设备: gw bench rx
  bidir   双向同时收发                设备: gw bench bidir

示例:
  %(prog)s --host 192.168.11.100 --mode rx
  %(prog)s --host 192.168.11.100 --mode tx --count 1000
  %(prog)s --host 192.168.11.100 --mode bidir --duration 10
""")
    p.add_argument("--host", default=DEFAULT_HOST, metavar="IP",
                   help=f"设备 IP (默认 {DEFAULT_HOST})")
    p.add_argument("--mode", choices=["rx", "tx", "bidir"], default="rx",
                   help="测试模式 (默认 rx)")
    p.add_argument("--sport", type=int, default=DEFAULT_SPORT, metavar="PORT",
                   help=f"设备收包端口 (默认 {DEFAULT_SPORT})")
    p.add_argument("--rport", type=int, default=DEFAULT_RPORT, metavar="PORT",
                   help=f"PC 收包端口 (默认 {DEFAULT_RPORT})")
    p.add_argument("--size", type=int, default=DEFAULT_SIZE, metavar="N",
                   help=f"包大小 bytes (默认 {DEFAULT_SIZE})")
    p.add_argument("--count", type=int, default=DEFAULT_COUNT, metavar="N",
                   help=f"发送包数, tx 模式 (默认 {DEFAULT_COUNT})")
    p.add_argument("--duration", type=float, default=DEFAULT_DURATION, metavar="S",
                   help=f"测试时长 秒, rx/bidir 模式 (默认 {DEFAULT_DURATION})")
    p.add_argument("--interval", type=float, default=0, metavar="MS",
                   help="发送间隔 ms, tx 模式 (默认 0=最快)")
    args = p.parse_args()

    if args.size < 4:
        p.error("--size 最小 4 (seq 占 4 字节)")

    if args.mode == "rx":
        do_rx(args)
    elif args.mode == "tx":
        do_tx(args)
    elif args.mode == "bidir":
        do_bidir(args)


if __name__ == "__main__":
    main()
