"""性能测试 - Modbus RTU FC03 (9600bps 物理层受限于串口带宽).

用法:
  python bench_modbus_rtu.py --port /dev/ttyUSB0 --count 100
"""
import argparse
import json
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common.modbus_client import MbClient, ModbusError  # noqa: E402
from config import (  # noqa: E402
    MODBUS_RTU_PORT, MODBUS_RTU_BAUDRATE, MODBUS_RTU_PARITY,
    MODBUS_RTU_STOPBITS, MODBUS_RTU_BYTESIZE, MODBUS_RTU_SLAVE_ID,
    HOLDING_COUNT,
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=MODBUS_RTU_PORT)
    ap.add_argument("--baud", type=int, default=MODBUS_RTU_BAUDRATE)
    ap.add_argument("--parity", default=MODBUS_RTU_PARITY)
    ap.add_argument("--stopbits", type=int, default=MODBUS_RTU_STOPBITS)
    ap.add_argument("--bytesize", type=int, default=MODBUS_RTU_BYTESIZE)
    ap.add_argument("--slave", type=int, default=MODBUS_RTU_SLAVE_ID)
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    print(f"=== Modbus RTU FC03 性能 ===")
    print(f"串口: {args.port} @ {args.baud}bps {args.parity}{args.bytesize}"
          f"{args.stopbits}, slave_id={args.slave}, 读 {HOLDING_COUNT} 寄存器\n")

    mb = MbClient(transport="rtu", serial_port=args.port, baudrate=args.baud,
                  parity=args.parity, stopbits=args.stopbits,
                  bytesize=args.bytesize)
    if not mb.connect():
        print(f"无法打开串口 {args.port}@{args.baud}")
        return 1

    try:
        print(f"[1/2] 单次 RTT ({args.count} 次)...")
        rtts = []
        for _ in range(args.count):
            t0 = time.perf_counter()
            mb.read_holding(0, HOLDING_COUNT, slave=args.slave)
            rtts.append((time.perf_counter() - t0) * 1000)
        rtts.sort()
        rtt_stat = {
            "count": len(rtts),
            "min_ms": round(rtts[0], 2),
            "p50_ms": round(statistics.median(rtts), 2),
            "p95_ms": round(rtts[int(len(rtts) * 0.95)], 2),
            "p99_ms": round(rtts[int(len(rtts) * 0.99)], 2),
            "max_ms": round(rtts[-1], 2),
            "mean_ms": round(statistics.mean(rtts), 2),
        }
        print(f"      min={rtt_stat['min_ms']}ms P50={rtt_stat['p50_ms']}ms "
              f"P95={rtt_stat['p95_ms']}ms P99={rtt_stat['p99_ms']}ms "
              f"max={rtt_stat['max_ms']}ms")

        # [2/2] 理论 QPS 由 RTT 决定 (9600bps 读 16 寄存器 ≈ 30ms/轮 → ~33QPS)
        est_qps = round(1000.0 / rtt_stat["mean_ms"], 1)
        rtt_stat["est_qps"] = est_qps
        print(f"\n      预估稳态 QPS ≈ {est_qps} (9600bps 物理层限制)")

        results = {"rtu_fc03": rtt_stat}
        if args.json:
            Path(args.json).write_text(json.dumps(results, indent=2, ensure_ascii=False))
            print(f"\n结果已写入 {args.json}")
    finally:
        mb.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
