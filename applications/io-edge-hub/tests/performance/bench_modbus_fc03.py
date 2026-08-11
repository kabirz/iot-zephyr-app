"""性能测试 - Modbus TCP FC03 (单客户端 + 多客户端并发).

测量:
- 单次 RTT 分布 (P50/P95/P99)
- 持续 N 秒的 QPS
- 3 客户端并发时的聚合 QPS

用法:
  python bench_modbus_fc03.py --ip 192.168.12.101 --duration 10
"""
import argparse
import json
import statistics
import sys
import threading
import time
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common.modbus_client import MbClient, ModbusError  # noqa: E402
from config import DEVICE_IP, MODBUS_TCP_PORT  # noqa: E402


def measure_rtt(mb: MbClient, count: int = 200):
    """单客户端往返延迟分布."""
    rtts = []
    for _ in range(count):
        t0 = time.perf_counter()
        mb.read_holding(0, 18)  # 读全部 18 个 holding
        rtts.append((time.perf_counter() - t0) * 1000)
    rtts.sort()
    # P95/P99 仅在样本量足够时计算 (≥100/≥20), 否则 None
    def pct(p):
        if len(rtts) < 20:
            return None
        idx = int(len(rtts) * p / 100)
        if idx >= len(rtts):
            idx = len(rtts) - 1
        return round(rtts[idx], 3)
    return {
        "count": len(rtts),
        "min_ms": round(rtts[0], 3),
        "p50_ms": round(statistics.median(rtts), 3),
        "p95_ms": pct(95),
        "p99_ms": pct(99),
        "max_ms": round(rtts[-1], 3),
        "mean_ms": round(statistics.mean(rtts), 3),
        "stdev_ms": round(statistics.stdev(rtts), 3) if len(rtts) > 1 else 0,
    }


def measure_qps(mb: MbClient, duration_s: float = 10.0):
    """单客户端持续 N 秒的 QPS."""
    end = time.monotonic() + duration_s
    count = 0
    errors = 0
    while time.monotonic() < end:
        try:
            mb.read_holding(0, 18)
            count += 1
        except ModbusError:
            errors += 1
    qps = count / duration_s
    return {"qps": round(qps, 1), "count": count, "errors": errors, "duration_s": duration_s}


def measure_concurrent_qps(ip: str, port: int, clients: int, duration_s: float = 10.0):
    """多客户端并发聚合 QPS."""
    total_count = 0
    total_errors = 0
    lock = threading.Lock()

    def worker():
        nonlocal total_count, total_errors
        mb = MbClient(ip, port)
        if not mb.connect():
            return
        local_count = 0
        local_errors = 0
        end = time.monotonic() + duration_s
        while time.monotonic() < end:
            try:
                mb.read_holding(0, 18)
                local_count += 1
            except ModbusError:
                local_errors += 1
        mb.close()
        with lock:
            total_count += local_count
            total_errors += local_errors

    with ThreadPoolExecutor(max_workers=clients) as pool:
        futures = [pool.submit(worker) for _ in range(clients)]
        for f in futures:
            f.result()

    return {
        "clients": clients,
        "total_count": total_count,
        "total_errors": total_errors,
        "aggregated_qps": round(total_count / duration_s, 1),
        "per_client_qps": round(total_count / duration_s / clients, 1),
        "duration_s": duration_s,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=DEVICE_IP)
    ap.add_argument("--port", type=int, default=MODBUS_TCP_PORT)
    ap.add_argument("--duration", type=float, default=10.0, help="QPS 测试持续秒数")
    ap.add_argument("--rtt-count", type=int, default=200, help="RTT 采样次数")
    ap.add_argument("--clients", type=int, default=3, help="并发客户端数")
    ap.add_argument("--json", default=None, help="结果写入 JSON 文件")
    args = ap.parse_args()

    print(f"=== Modbus TCP FC03 性能测试 ({args.ip}:{args.port}) ===\n")

    mb = MbClient(args.ip, args.port)
    if not mb.connect():
        print(f"无法连接 {args.ip}:{args.port}")
        return 1

    try:
        print(f"[1/3] 单客户端 RTT ({args.rtt_count} 次采样)...")
        rtt = measure_rtt(mb, args.rtt_count)
        def fmt_ms(v):
            return f"{v:.2f}ms" if v is not None else "N/A    "
        print(f"      min={fmt_ms(rtt['min_ms'])}  P50={fmt_ms(rtt['p50_ms'])}  "
              f"P95={fmt_ms(rtt['p95_ms'])}  P99={fmt_ms(rtt['p99_ms'])}  "
              f"max={fmt_ms(rtt['max_ms'])}")
        print(f"      mean={fmt_ms(rtt['mean_ms'])}  stdev={fmt_ms(rtt['stdev_ms'])}\n")

        print(f"[2/3] 单客户端 QPS ({args.duration}s 持续)...")
        qps = measure_qps(mb, args.duration)
        print(f"      QPS={qps['qps']:.1f}  总请求={qps['count']}  "
              f"错误={qps['errors']}\n")

        print(f"[3/3] {args.clients} 客户端并发 ({args.duration}s)...")
        mb.close()  # 关闭单客户端, 让并发测试干净启动
        cc = measure_concurrent_qps(args.ip, args.port, args.clients, args.duration)
        print(f"      聚合 QPS={cc['aggregated_qps']:.1f}  "
              f"每客户端={cc['per_client_qps']:.1f}  "
              f"总请求={cc['total_count']}  错误={cc['total_errors']}\n")
    finally:
        mb.close()

    print("=== 完成 ===")
    if args.json:
        result = {"rtt": rtt, "single_client_qps": qps, "concurrent": cc}
        Path(args.json).write_text(json.dumps(result, indent=2, ensure_ascii=False))
        print(f"结果已写入 {args.json}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
