#!/usr/bin/env python3
"""UDP 洪泛发送端: 配合 eth-bwtest 例程的 `bench rx` 命令.

板端 shell 先执行 `bench rx` (默认端口 19601, 5 秒), 再在主机上运行
本脚本向板端灌包. 板端统计收到的包数/字节数并计算 RX 速率; 本脚本
打印主机侧发送速率, 两者包数之差即丢包.

用法:
  python3 udp_flood.py 192.168.12.200                       # 5 秒全速
  python3 udp_flood.py 192.168.12.200 -t 10 -s 1400         # 10 秒, 1400B/包
  python3 udp_flood.py 192.168.12.200 --pps 5000            # 限速 5k pkt/s
"""

import argparse
import socket
import time


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("ip", help="board IPv4 address")
    p.add_argument("--port", type=int, default=19601)
    p.add_argument("-t", "--seconds", type=float, default=5.0)
    p.add_argument("-s", "--size", type=int, default=1024,
                   help="payload bytes per packet (default 1024)")
    p.add_argument("--pps", type=int, default=0,
                   help="rate cap in packets/s (0 = as fast as possible)")
    args = p.parse_args()

    if not 1 <= args.size <= 1472:
        raise SystemExit("payload size must be 1..1472 (UDP/IPv4 MTU limit)")

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payload = b"\xAA" * args.size

    period = 1.0 / args.pps if args.pps else 0.0
    next_t = t0 = time.monotonic()
    deadline = t0 + args.seconds
    count = 0

    while True:
        now = time.monotonic()
        if now >= deadline:
            break
        if period and now < next_t:
            continue
        if period:
            next_t += period
        s.sendto(payload, (args.ip, args.port))
        count += 1

    dt = time.monotonic() - t0
    total = count * args.size
    print(f"host TX: {count} pkts ({total} B) in {dt:.3f} s = "
          f"{total * 8 / dt / 1e6:.2f} Mbit/s, {count / dt:.0f} pkt/s")
    print("(compare with the `bench rx` counters on the board console "
          "to get the loss rate)")


if __name__ == "__main__":
    main()
