"""性能测试 - UDP 命令 RTT (GET_IP / GET_MODBUS / SET_TIME).

用法:
  python bench_udp_rtt.py --ip 192.168.12.101 --count 500
"""
import argparse
import json
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common.udp_client import UdpClient, UdpError  # noqa: E402
from config import DEVICE_IP, UDP_PORT  # noqa: E402


def measure_cmd(udp: UdpClient, name: str, fn, count: int):
    rtts = []
    errors = 0
    for _ in range(count):
        try:
            t0 = time.perf_counter()
            fn()
            rtts.append((time.perf_counter() - t0) * 1000)
        except (UdpError, OSError):
            errors += 1
    rtts.sort()

    def pct(p):
        if len(rtts) < 20:
            return None
        idx = min(int(len(rtts) * p / 100), len(rtts) - 1)
        return round(rtts[idx], 3)

    return {
        "cmd": name,
        "count": len(rtts),
        "errors": errors,
        "min_ms": round(rtts[0], 3) if rtts else None,
        "p50_ms": round(statistics.median(rtts), 3) if rtts else None,
        "p95_ms": pct(95),
        "p99_ms": pct(99),
        "max_ms": round(rtts[-1], 3) if rtts else None,
        "mean_ms": round(statistics.mean(rtts), 3) if rtts else None,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=DEVICE_IP)
    ap.add_argument("--port", type=int, default=UDP_PORT)
    ap.add_argument("--count", type=int, default=500, help="每命令采样次数")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    print(f"=== UDP 命令 RTT ({args.ip}:{args.port}, 每命令 {args.count} 次) ===\n")
    print(f"{'命令':<14} {'min':>8} {'P50':>8} {'P95':>8} {'P99':>8} "
          f"{'max':>8} {'mean':>8} {'errors':>8}")
    print("-" * 80)

    results = []
    with UdpClient(args.ip, args.port) as udp:
        for name, fn in [
            ("GET_IP",     lambda: udp.get_ip()),
            ("GET_MODBUS", lambda: udp.get_modbus()),
            ("SET_TIME",   lambda: udp.set_time(int(time.time()))),
        ]:
            r = measure_cmd(udp, name, fn, args.count)
            results.append(r)
            fmt = lambda v: f"{v:>7.2f}" if v is not None else f"{'N/A':>8}"  # noqa: E731
            print(f"{name:<14} {fmt(r['min_ms'])} {fmt(r['p50_ms'])} "
                  f"{fmt(r['p95_ms'])} {fmt(r['p99_ms'])} {fmt(r['max_ms'])} "
                  f"{fmt(r['mean_ms'])} {r['errors']:>8}")

    print("\n=== 完成 ===")
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2, ensure_ascii=False))
        print(f"结果已写入 {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
