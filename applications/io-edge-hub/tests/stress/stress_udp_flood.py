"""压力测试 - UDP 命令风暴.

策略:
- 多线程并发发 UDP 命令 (GET_IP / GET_MODBUS / SET_TIME)
- 统计: 成功/失败次数, RTT 分布, 错误类型
- 目的: 验证固件 udp_fw_upgrade RX 线程在高负载下的稳定性

用法:
  python stress_udp_flood.py --ip 192.168.12.101 --duration 120 --threads 4
"""
import argparse
import signal
import socket
import statistics
import sys
import threading
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common.udp_client import UdpClient, UdpError  # noqa: E402
from config import DEVICE_IP, UDP_PORT  # noqa: E402

stop_flag = threading.Event()


def signal_handler(sig, frame):
    print("\n收到 Ctrl+C, 正在停止...")
    stop_flag.set()


signal.signal(signal.SIGINT, signal_handler)


def worker(ip: str, port: int, duration_s: float, idx: int, results: list):
    """每个线程独立的 UdpClient 实例, 顺序发 3 种命令."""
    udp = UdpClient(ip, port, timeout=2.0)
    count_ok = 0
    count_err = 0
    err_kinds = Counter()
    rtts = []
    end = time.monotonic() + duration_s

    cmd_idx = 0
    while not stop_flag.is_set() and time.monotonic() < end:
        try:
            t0 = time.perf_counter()
            if cmd_idx % 3 == 0:
                udp.get_ip()
            elif cmd_idx % 3 == 1:
                udp.get_modbus()
            else:
                udp.set_time(int(time.time()))
            rtts.append((time.perf_counter() - t0) * 1000)
            count_ok += 1
        except UdpError as e:
            count_err += 1
            err_kinds[str(e)[:50]] += 1
        except OSError as e:
            count_err += 1
            err_kinds[f"OSError: {str(e)[:40]}"] += 1
            # socket 可能损坏, 重建
            try:
                udp.close()
            except Exception:
                pass
            udp = UdpClient(ip, port, timeout=2.0)
        cmd_idx += 1

    udp.close()
    results[idx] = {
        "ok": count_ok,
        "err": count_err,
        "rtts": rtts,
        "err_kinds": err_kinds,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=DEVICE_IP)
    ap.add_argument("--port", type=int, default=UDP_PORT)
    ap.add_argument("--duration", type=float, default=60)
    ap.add_argument("--threads", type=int, default=4)
    args = ap.parse_args()

    print(f"=== UDP 命令风暴压力测试 ===")
    print(f"目标: {args.ip}:{args.port}, 时长: {args.duration}s, "
          f"线程: {args.threads}")
    print(f"按 Ctrl+C 提前停止\n")

    results = [None] * args.threads
    threads = []
    start = time.monotonic()

    for i in range(args.threads):
        t = threading.Thread(target=worker,
                             args=(args.ip, args.port, args.duration, i, results),
                             daemon=True)
        t.start()
        threads.append(t)

    # 进度打印
    while any(t.is_alive() for t in threads) and not stop_flag.is_set():
        time.sleep(10)
        elapsed = time.monotonic() - start
        if elapsed < args.duration:
            print(f"  [{elapsed:6.0f}s] 进行中...", flush=True)

    for t in threads:
        t.join(timeout=5)

    # 汇总
    total_ok = sum(r["ok"] for r in results if r)
    total_err = sum(r["err"] for r in results if r)
    all_rtts = []
    all_errs = Counter()
    for r in results:
        if r:
            all_rtts.extend(r["rtts"])
            all_errs.update(r["err_kinds"])
    all_rtts.sort()
    actual_duration = time.monotonic() - start

    print("\n=== 压力测试结果 ===")
    print(f"  实际运行: {actual_duration:.1f}s")
    print(f"  成功: {total_ok:,}  失败: {total_err:,}  "
          f"错误率: {total_err/max(total_ok+total_err,1)*100:.3f}%")
    print(f"  聚合 QPS: {(total_ok+total_err)/actual_duration:.1f}")
    if all_rtts:
        print(f"  RTT  min={all_rtts[0]:.2f}ms  "
              f"P50={statistics.median(all_rtts):.2f}ms  "
              f"P95={all_rtts[int(len(all_rtts)*0.95)]:.2f}ms  "
              f"P99={all_rtts[int(len(all_rtts)*0.99)]:.2f}ms  "
              f"max={all_rtts[-1]:.2f}ms")
    if all_errs:
        print(f"  Top 错误:")
        for msg, cnt in all_errs.most_common(5):
            print(f"    [{cnt:>5}] {msg}")

    err_rate = total_err / max(total_ok + total_err, 1) * 100
    return 0 if err_rate < 5.0 else 2


if __name__ == "__main__":
    sys.exit(main())
