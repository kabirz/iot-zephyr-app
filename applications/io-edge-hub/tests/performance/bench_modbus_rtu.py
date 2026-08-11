"""性能测试 - Modbus RTU (RS485) FC03 RTT 与 QPS.

9600bps 8N1 物理层限制:
- 读 18 个 holding (≈45 字节 RTU 帧) ≈ 50-90ms RTT
- 单客户端 QPS 上限约 10-20

用法:
  python bench_modbus_rtu.py --port /dev/ttyUSB0 --baud 9600 --count 100
"""
import argparse
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common.modbus_client import MbClient, ModbusError  # noqa: E402
from config import (  # noqa: E402
    MODBUS_RTU_PORT, MODBUS_RTU_BAUDRATE, MODBUS_RTU_PARITY,
    MODBUS_RTU_STOPBITS, MODBUS_RTU_BYTESIZE, MODBUS_RTU_TIMEOUT,
    MODBUS_RTU_SLAVE_ID, HOLDING_COUNT,
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
    ap.add_argument("--duration", type=float, default=10.0,
                    help="QPS 测试持续秒数")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    print(f"=== Modbus RTU FC03 性能测试 ===")
    print(f"串口: {args.port} @ {args.baud}bps {args.parity}{args.bytesize}{args.stopbits}, "
          f"slave_id={args.slave}")
    print(f"采样: RTT {args.count} 次, QPS {args.duration}s\n")

    try:
        mb = MbClient(transport="rtu", serial_port=args.port, baudrate=args.baud,
                      parity=args.parity, stopbits=args.stopbits, bytesize=args.bytesize)
    except ModbusError as e:
        print(f"RTU 客户端创建失败: {e}")
        return 1
    if not mb.connect():
        print(f"无法打开串口 {args.port}@{args.baud}")
        return 1

    try:
        # [1] RTT 分布
        print(f"[1/2] 单次 RTT ({args.count} 次采样, 读 {HOLDING_COUNT} holding)...")
        rtts = []
        for _ in range(args.count):
            import time
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
        print(f"      min={rtt_stat['min_ms']:.2f}ms  P50={rtt_stat['p50_ms']:.2f}ms  "
              f"P95={rtt_stat['p95_ms']:.2f}ms  P99={rtt_stat['p99_ms']:.2f}ms  "
              f"max={rtt_stat['max_ms']:.2f}ms")
        print(f"      mean={rtt_stat['mean_ms']:.2f}ms\n")

        # [2] 持续 QPS
        print(f"[2/2] 持续 QPS ({args.duration}s)...")
        import time
        end = time.monotonic() + args.duration
        count_ok = 0
        count_err = 0
        while time.monotonic() < end:
            try:
                mb.read_holding(0, HOLDING_COUNT, slave=args.slave)
                count_ok += 1
            except ModbusError:
                count_err += 1
        qps_stat = {
            "qps": round(count_ok / args.duration, 2),
            "count": count_ok,
            "errors": count_err,
            "duration_s": args.duration,
        }
        print(f"      QPS={qps_stat['qps']:.2f}  总请求={count_ok}  错误={count_err}\n")
    finally:
        mb.close()

    print("=== 完成 ===")
    if args.json:
        result = {"rtt": rtt_stat, "qps": qps_stat,
                  "config": {"port": args.port, "baud": args.baud, "slave": args.slave}}
        Path(args.json).write_text(json.dumps(result, indent=2, ensure_ascii=False))
        print(f"结果已写入 {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
