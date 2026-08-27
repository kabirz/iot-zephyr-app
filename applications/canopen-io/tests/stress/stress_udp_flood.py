"""压力测试 - UDP 命令风暴 (多线程 GET_IP/GET_MODBUS/SET_TIME).

验证固件 udp_fw_upgrade RX 线程高负载稳定性. Ctrl+C 优雅停止.

用法:
  python stress_udp_flood.py --ip 192.168.12.101 --duration 120 --threads 4
"""
import argparse
import signal
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
    udp = UdpClient(ip, port, timeout=2.0)
    count_ok = count_err = 0
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
            try:
                udp.close()
            except Exception:  # noqa: BLE001
                pass
            udp = UdpClient(ip, port, timeout=2.0)
        cmd_idx += 1

        if count_ok % 500 == 0 and time.monotonic() - end + duration_s > 0:
            remaining = end - time.monotonic()
            print(f"  [线程{idx} 剩 {remaining:5.1f}s] ok={count_ok} err={count_err}",
                  flush=True)

    udp.close()
    results[idx] = {"ok": count_ok, "err": count_err, "rtts": rtts,
                    "err_kinds": err_kinds}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=DEVICE_IP)
    ap.add_argument("--port", type=int, default=UDP_PORT)
    ap.add_argument("--duration", type=float, default=120)
    ap.add_argument("--threads", type=int, default=4)
    args = ap.parse_args()

    print(f"=== UDP 命令风暴 ===")
    print(f"目标 {args.ip}:{args.port}, {args.duration}s x {args.threads} 线程\n")

    results = [None] * args.threads
    threads = [threading.Thread(target=worker,
                                args=(args.ip, args.port, args.duration, i, results))
               for i in range(args.threads)]
    for t in threads:
        t.start()
    try:
        while any(t.is_alive() for t in threads):
            time.sleep(1)
    except KeyboardInterrupt:
        stop_flag.set()

    total_ok = sum(r["ok"] for r in results if r)
    total_err = sum(r["err"] for r in results if r)
    all_rtts = sorted(rt for r in results if r for rt in r["rtts"])
    kinds = Counter()
    for r in results:
        if r:
            kinds.update(r["err_kinds"])

    print(f"\n=== 结果 ===")
    print(f"ok={total_ok:,}  err={total_err:,}  "
          f"错误率={total_err / max(total_ok + total_err, 1) * 100:.2f}%")
    print(f"吞吐 {total_ok / args.duration:.0f} req/s")
    if all_rtts:
        print(f"RTT P50={statistics.median(all_rtts):.2f}ms "
              f"P95={all_rtts[int(len(all_rtts) * 0.95)]:.2f}ms "
              f"max={all_rtts[-1]:.2f}ms")
    for msg, cnt in kinds.most_common(5):
        print(f"  [{cnt:>6}] {msg}")

    fail = total_err / max(total_ok + total_err, 1) > 0.01
    print("\n=== 完成 ===" + (" (FAILED)" if fail else " (PASS)"))
    return 2 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
