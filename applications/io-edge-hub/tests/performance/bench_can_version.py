"""性能测试 - CAN VERSION 查询 RTT.

测量完整 VERSION 流程 (0x101 → 0x102 → N×0x105) 的 RTT 分布.

用法:
  python bench_can_version.py --channel can0 --count 100
"""
import argparse
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common.can_client import CanClient, CanError  # noqa: E402
from config import CAN_CHANNEL, CAN_INTERFACE  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", default=CAN_CHANNEL)
    ap.add_argument("--interface", default=CAN_INTERFACE)
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    print(f"=== CAN VERSION RTT 测试 (channel={args.channel}, count={args.count}) ===\n")

    try:
        can = CanClient(channel=args.channel, interface=args.interface)
    except CanError as e:
        print(f"CAN 接口不可用: {e}")
        return 1

    rtts = []
    errors = 0
    versions = set()
    try:
        for i in range(args.count):
            try:
                import time
                t0 = time.perf_counter()
                v = can.query_version(timeout=3.0)
                rtts.append((time.perf_counter() - t0) * 1000)
                versions.add(v)
            except CanError as e:
                errors += 1
                if errors <= 3:
                    print(f"  错误 {errors}: {e}")
    finally:
        can.close()

    if not rtts:
        print(f"全部失败 ({errors} errors)")
        return 2

    rtts.sort()
    result = {
        "count": len(rtts),
        "errors": errors,
        "min_ms": round(rtts[0], 2),
        "p50_ms": round(statistics.median(rtts), 2),
        "p95_ms": round(rtts[int(len(rtts) * 0.95)], 2),
        "p99_ms": round(rtts[int(len(rtts) * 0.99)], 2),
        "max_ms": round(rtts[-1], 2),
        "mean_ms": round(statistics.mean(rtts), 2),
        "distinct_versions": len(versions),
    }

    print(f"{'count':>6} {'errors':>7} {'min':>8} {'P50':>8} {'P95':>8} "
          f"{'P99':>8} {'max':>8} {'mean':>8}")
    print("-" * 70)
    print(f"{result['count']:>6} {result['errors']:>7} {result['min_ms']:>7.2f}m "
          f"{result['p50_ms']:>7.2f}m {result['p95_ms']:>7.2f}m "
          f"{result['p99_ms']:>7.2f}m {result['max_ms']:>7.2f}m "
          f"{result['mean_ms']:>7.2f}m")
    print(f"\n  版本字符串 (去重): {versions}")

    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=2, ensure_ascii=False))
        print(f"\n结果已写入 {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
