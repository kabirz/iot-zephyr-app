#!/usr/bin/env python3
"""TCP 发送端: 配合 eth-bwtest 例程的 9900 端口测带宽.

默认模式 (sink): 先发一个 'R' 让板端进入纯收流模式, 板端在连接结束
时打印收到的字节数/耗时/速率 (板端 RX 带宽); 本脚本打印主机侧发送
速率 (TCP 流控下两者应接近).

--echo 模式: 首字节不发 'R', 板端原样回环 (双向吞吐); 加 --drain 才
读取回包, 否则只统计发送.

用法:
  python3 tcp_send.py 192.168.12.200                 # 100MB sink 测试
  python3 tcp_send.py 192.168.12.200 -m 500          # 500MB
  python3 tcp_send.py 192.168.12.200 --echo          # 回环模式
"""

import argparse
import socket
import threading
import time


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("ip", help="board IPv4 address")
    p.add_argument("--port", type=int, default=9900)
    p.add_argument("-m", "--mbytes", type=float, default=100.0,
                   help="payload size in MiB (default 100)")
    p.add_argument("--chunk", type=int, default=4096)
    p.add_argument("--echo", action="store_true",
                   help="echo mode instead of receive-only sink")
    args = p.parse_args()

    total = int(args.mbytes * 1024 * 1024)
    chunk = b"\x00" * args.chunk

    s = socket.create_connection((args.ip, args.port), timeout=5)
    print(f"connected to {args.ip}:{args.port} "
          f"({'echo' if args.echo else 'sink'} mode, "
          f"{total / 1048576:.0f} MiB, chunk {args.chunk} B)")

    recv = 0
    if args.echo:
        # echo 模式必须持续读回包, 否则主机 rcv 窗口关闭, 板端 send 阻塞,
        # 板端不再 recv -> 双向零窗口死锁 (实测会拖到 0.1 Mbit/s 量级)
        stop = threading.Event()

        def reader():
            nonlocal recv
            s.settimeout(1)
            while not stop.is_set():
                try:
                    recv += len(s.recv(1 << 20))
                except socket.timeout:
                    pass

        t = threading.Thread(target=reader, daemon=True)
        t.start()

    sent = 0
    t0 = time.monotonic()

    try:
        if not args.echo:
            s.sendall(b"R")  # 首字节选模式
            total -= 1
        while sent < total:
            s.settimeout(30)
            n = s.send(chunk[: min(args.chunk, total - sent)])
            sent += n
    except (BrokenPipeError, ConnectionResetError):
        print("connection closed by peer (board reported its rate on the console)")
    finally:
        dt = time.monotonic() - t0
        if args.echo:
            stop.set()
            time.sleep(1)  # 让读线程收尾
        s.close()

    print(f"host TX: {sent} B in {dt:.3f} s = {sent * 8 / dt / 1e6:.2f} Mbit/s")
    if args.echo:
        print(f"host RX: {recv} B "
              f"({recv * 8 / dt / 1e6:.2f} Mbit/s, {recv * 100 // max(sent, 1)}% of sent)")


if __name__ == "__main__":
    main()
