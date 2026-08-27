"""性能测试 - Modbus TCP FC03 (单客户端 RTT/QPS + 多客户端并发聚合).

用法:
  python bench_modbus_fc03.py --ip 192.168.12.101 --duration 10 --clients 8
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
from config import DEVICE_IP, MODBUS_TCP_PORT, HOLDING_COUNT  # noqa: E402

READ_COUNT = HOLDING_COUNT


def measure_rtt(mb: MbClient, count: int = 200):
    rtts = []
    for _ in range(count):
        t0 = time.perf_counter()
        mb.read_holding(0, READ_COUNT)
        rtts.append((time.perf_counter() - t0) * 1000)
    rtts.sort()

    def pct(p):
        if len(rtts) < 20:
            return None
        idx = min(int(len(rtts) * p / 100), len(rtts) - 1)
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
    end = time.monotonic() + duration_s
    count = errors = 0
    while time.monotonic() < end:
        try:
            mb.read_holding(0, READ_COUNT)
            count += 1
        except ModbusError:
            errors += 1
    return {"qps": round(count / duration_s, 1), "count": count,
            "errors": errors, "duration_s": duration_s}


def measure_concurrent_qps(ip: str, port: int, clients: int, duration_s: float = 10.0):
    total = {"ok": 0, "err": 0}
    lock = threading.Lock()
    stop = threading.Event()

    def worker():
        cli = MbClient(ip=ip, port=port)
        ok = err = 0
        try:
            if not cli.connect():
                with lock:
                    total["err"] += 1
                return
            while not stop.is_set():
                try:
                    cli.read_holding(0, READ_COUNT)
                    ok += 1
                except (ModbusError, OSError):
                    err += 1
        finally:
            cli.close()
            with lock:
                total["ok"] += ok
                total["err"] += err

    threads = [threading.Thread(target=worker) for _ in range(clients)]
    for t in threads:
        t.start()
    time.sleep(duration_s)
    stop.set()
    for t in threads:
        t.join()
    return {"clients": clients, "agg_qps": round(total["ok"] / duration_s, 1),
            **total}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=DEVICE_IP)
    ap.add_argument("--port", type=int, default=MODBUS_TCP_PORT)
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--clients", type=int, default=8)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    print(f"=== Modbus TCP FC03 性能 ({args.ip}:{args.port}, 读 {READ_COUNT} 寄存器) ===\n")
    mb = MbClient(ip=args.ip, port=args.port)
    if not mb.connect():
        print("无法连接设备")
        return 1

    results = {}
    try:
        print(f"[1/3] RTT 分布 (200 次采样)...")
        rtt = measure_rtt(mb)
        results["rtt"] = rtt
        print(f"      min={rtt['min_ms']}ms P50={rtt['p50_ms']}ms "
              f"P95={rtt['p95_ms']}ms P99={rtt['p99_ms']}ms max={rtt['max_ms']}ms")

        print(f"[2/3] 单客户端 QPS ({args.duration}s)...")
        qps = measure_qps(mb, args.duration)
        results["qps"] = qps
        print(f"      {qps['qps']} req/s (errors={qps['errors']})")

        print(f"[3/3] {args.clients} 客户端并发...")
        conc = measure_concurrent_qps(args.ip, args.port, args.clients, args.duration)
        results["concurrent"] = conc
        print(f"      聚合 {conc['agg_qps']} req/s (ok={conc['ok']}, err={conc['err']})")
    finally:
        mb.close()

    print("\n=== 完成 ===")
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2, ensure_ascii=False))
        print(f"结果已写入 {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
