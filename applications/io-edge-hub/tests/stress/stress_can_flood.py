"""压力测试 - CAN 总线高负载.

两阶段:
  1. 高频业务帧发送 (N 秒): 设备只 LOG, 不响应. 验证设备不崩.
  2. 高频 VERSION 查询 (N 秒): 完整请求-回复往返, 验证稳定性 + 错误率.

用法:
  python stress_can_flood.py --channel can0 --duration 120 --business-qps 500
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
from common.can_client import CanClient, CanError  # noqa: E402
from config import CAN_CHANNEL, CAN_INTERFACE, CAN_DEFAULT_BUSINESS_ID  # noqa: E402

stop_flag = threading.Event()


def signal_handler(sig, frame):
    print("\n收到 Ctrl+C, 正在停止...")
    stop_flag.set()


signal.signal(signal.SIGINT, signal_handler)


def phase_business_flood(can, duration_s, qps_limit):
    """阶段 1: 高频业务帧风暴."""
    print(f"\n[阶段 1] 业务帧风暴 ({duration_s}s, {qps_limit} QPS, id=0x{CAN_DEFAULT_BUSINESS_ID:03X})")
    interval = 1.0 / qps_limit if qps_limit > 0 else 0
    count_ok = 0
    count_err = 0
    err_kinds = Counter()
    end = time.monotonic() + duration_s
    next_send = time.monotonic()
    seq = 0
    while not stop_flag.is_set() and time.monotonic() < end:
        now = time.monotonic()
        if interval > 0 and now < next_send:
            time.sleep(min(0.001, next_send - now))
            continue
        next_send += interval
        try:
            # 8B 业务帧, seq 在 byte[0]
            payload = bytes([seq & 0xFF, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6])
            can.send_business(CAN_DEFAULT_BUSINESS_ID, payload)
            count_ok += 1
        except CanError as e:
            count_err += 1
            err_kinds[str(e)[:50]] += 1
        seq += 1
        if count_ok % 1000 == 0 and count_ok > 0:
            elapsed = time.monotonic() + duration_s - end
            print(f"  [{elapsed:5.1f}s] 发送 {count_ok} 帧, 错误 {count_err}", flush=True)

    return {
        "phase": "business_flood",
        "ok": count_ok,
        "err": count_err,
        "err_rate_pct": round(count_err / max(count_ok + count_err, 1) * 100, 3),
        "qps": round(count_ok / duration_s, 1),
        "top_errors": err_kinds.most_common(3),
    }


def phase_version_queries(can, duration_s, max_concurrent_qps=10):
    """阶段 2: 连续 VERSION 查询 (有完整回复往返)."""
    print(f"\n[阶段 2] VERSION 查询循环 ({duration_s}s)")
    interval = 1.0 / max_concurrent_qps if max_concurrent_qps > 0 else 0
    count_ok = 0
    count_err = 0
    err_kinds = Counter()
    rtts = []
    end = time.monotonic() + duration_s
    next_send = time.monotonic()

    while not stop_flag.is_set() and time.monotonic() < end:
        now = time.monotonic()
        if interval > 0 and now < next_send:
            time.sleep(min(0.001, next_send - now))
            continue
        next_send += interval
        try:
            t0 = time.perf_counter()
            can.query_version(timeout=3.0)
            rtts.append((time.perf_counter() - t0) * 1000)
            count_ok += 1
        except CanError as e:
            count_err += 1
            err_kinds[str(e)[:50]] += 1
        if count_ok % 20 == 0 and count_ok > 0:
            elapsed = time.monotonic() + duration_s - end
            print(f"  [{elapsed:5.1f}s] 成功 {count_ok}, 失败 {count_err}", flush=True)

    rtts.sort()
    return {
        "phase": "version_queries",
        "ok": count_ok,
        "err": count_err,
        "err_rate_pct": round(count_err / max(count_ok + count_err, 1) * 100, 3),
        "qps": round(count_ok / duration_s, 1),
        "rtt_min_ms": round(rtts[0], 2) if rtts else 0,
        "rtt_p50_ms": round(statistics.median(rtts), 2) if rtts else 0,
        "rtt_p95_ms": round(rtts[int(len(rtts) * 0.95)], 2) if rtts else 0,
        "rtt_max_ms": round(rtts[-1], 2) if rtts else 0,
        "top_errors": err_kinds.most_common(3),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", default=CAN_CHANNEL)
    ap.add_argument("--interface", default=CAN_INTERFACE)
    ap.add_argument("--duration", type=float, default=60.0,
                    help="每阶段持续秒数")
    ap.add_argument("--business-qps", type=int, default=500,
                    help="阶段 1 业务帧发送 QPS")
    ap.add_argument("--version-qps", type=int, default=10,
                    help="阶段 2 VERSION 查询 QPS (建议 ≤20)")
    ap.add_argument("--skip-business", action="store_true",
                    help="跳过阶段 1, 只跑阶段 2")
    args = ap.parse_args()

    print(f"=== CAN 总线压力测试 ===")
    print(f"目标: channel={args.channel}, 每阶段 {args.duration}s")
    print(f"按 Ctrl+C 提前停止")

    try:
        can = CanClient(channel=args.channel, interface=args.interface)
    except CanError as e:
        print(f"CAN 接口不可用: {e}")
        return 1

    results = []
    try:
        if not args.skip_business:
            r1 = phase_business_flood(can, args.duration, args.business_qps)
            results.append(r1)
            print(f"\n  阶段 1 结果: ok={r1['ok']:,} err={r1['err']:,} "
                  f"({r1['err_rate_pct']}%) qps={r1['qps']}")
            if r1["top_errors"]:
                for msg, cnt in r1["top_errors"]:
                    print(f"    [{cnt:>5}] {msg}")
            if stop_flag.is_set():
                return 0

        r2 = phase_version_queries(can, args.duration, args.version_qps)
        results.append(r2)
        print(f"\n  阶段 2 结果: ok={r2['ok']:,} err={r2['err']:,} "
              f"({r2['err_rate_pct']}%) qps={r2['qps']}")
        print(f"    RTT min={r2['rtt_min_ms']}ms P50={r2['rtt_p50_ms']}ms "
              f"P95={r2['rtt_p95_ms']}ms max={r2['rtt_max_ms']}ms")
        if r2["top_errors"]:
            for msg, cnt in r2["top_errors"]:
                print(f"    [{cnt:>5}] {msg}")
    finally:
        can.close()

    # 通过判据: 错误率 < 1%
    fail = any(r["err_rate_pct"] > 1.0 for r in results)
    print("\n=== 完成 ===" + (" (FAILED)" if fail else " (PASS)"))
    return 2 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
