"""性能测试 - UDP 命令 RTT (GET_IP / SET_TIME / GET_MODBUS).

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


def measure_cmd(udp: UdpClient, cmd_name: str, fn, count: int):
    """对单条命令做 N 次 RTT 采样."""
    rtts = []
    errors = 0
    for _ in range(count):
        try:
            t0 = time.perf_counter()
            fn()
            rtts.append((time.perf_counter() - t0) * 1000)
        except UdpError:
            errors += 1
    rtts.sort()
    if not rtts:
        return {"cmd": cmd_name, "count": 0, "errors": errors}
    return {
        "cmd": cmd_name,
        "count": len(rtts),
        "errors": errors,
        "min_ms": round(rtts[0], 3),
        "p50_ms": round(statistics.median(rtts), 3),
        "p95_ms": round(rtts[int(len(rtts) * 0.95)], 3),
        "p99_ms": round(rtts[int(len(rtts) * 0.99)], 3),
        "max_ms": round(rtts[-1], 3),
        "mean_ms": round(statistics.mean(rtts), 3),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=DEVICE_IP)
    ap.add_argument("--port", type=int, default=UDP_PORT)
    ap.add_argument("--count", type=int, default=500, help="每命令采样次数")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    print(f"=== UDP 命令 RTT 测试 ({args.ip}:{args.port}, 每命令 {args.count} 次) ===\n")
    print(f"{'命令':<14} {'min':>8} {'P50':>8} {'P95':>8} {'P99':>8} {'max':>8} "
          f"{'mean':>8} {'errors':>8}")
    print("-" * 80)

    results = []
    with UdpClient(args.ip, args.port) as udp:
        for name, fn in [
            ("GET_IP",      lambda: udp.get_ip()),
            ("GET_MODBUS",  lambda: udp.get_modbus()),
            ("SET_TIME",    lambda: udp.set_time(int(time.time()))),
        ]:
            r = measure_cmd(udp, name, fn, args.count)
            results.append(r)
            if r["count"] > 0:
                print(f"{name:<14} {r['min_ms']:>7.2f}m {r['p50_ms']:>7.2f}m "
                      f"{r['p95_ms']:>7.2f}m {r['p99_ms']:>7.2f}m {r['max_ms']:>7.2f}m "
                      f"{r['mean_ms']:>7.2f}m {r['errors']:>8}")
            else:
                print(f"{name:<14} {'-':>8} {'-':>8} {'-':>8} {'-':>8} {'-':>8} {'-':>8} "
                      f"{r['errors']:>8}")

    print("\n=== 完成 ===")
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2, ensure_ascii=False))
        print(f"结果已写入 {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
