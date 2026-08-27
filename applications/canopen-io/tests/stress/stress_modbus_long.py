"""压力测试 - Modbus TCP 长时间高频读 (支持 QPS 限速, Ctrl+C 优雅停止).

用法:
  python stress_modbus_long.py --ip 192.168.12.101 --duration 3600
  python stress_modbus_long.py --duration 60 --qps 200
  python stress_modbus_long.py --transport rtu --port /dev/ttyUSB0 --baud 9600
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
from common.modbus_client import MbClient, ModbusError  # noqa: E402
from config import (  # noqa: E402
    DEVICE_IP, MODBUS_TCP_PORT,
    MODBUS_RTU_PORT, MODBUS_RTU_BAUDRATE, MODBUS_RTU_PARITY,
    MODBUS_RTU_STOPBITS, MODBUS_RTU_BYTESIZE, MODBUS_RTU_SLAVE_ID,
    HOLDING_COUNT,
)

stop_flag = threading.Event()


def signal_handler(sig, frame):
    print("\n收到 Ctrl+C, 正在停止...")
    stop_flag.set()


signal.signal(signal.SIGINT, signal_handler)


def make_client(args) -> MbClient:
    if args.transport == "rtu":
        return MbClient(transport="rtu", serial_port=args.port, baudrate=args.baud,
                        parity=args.parity, stopbits=args.stopbits,
                        bytesize=args.bytesize)
    return MbClient(ip=args.ip, port=args.port)


def run(args):
    mb = make_client(args)
    if not mb.connect():
        print(f"无法连接 Modbus {mb.id_str}")
        return None

    slave = args.slave
    interval = 1.0 / args.qps if args.qps > 0 else 0
    count_ok = count_err = 0
    err_kinds = Counter()
    rtt_samples = []
    start = end = time.monotonic()
    end = start + args.duration

    next_send = start
    last_progress = 0
    while not stop_flag.is_set() and time.monotonic() < end:
        now = time.monotonic()
        if interval > 0 and now < next_send:
            time.sleep(min(0.001, next_send - now))
            continue
        next_send += interval

        try:
            t0 = time.perf_counter()
            mb.read_holding(0, HOLDING_COUNT, slave=slave)
            rtt_samples.append((time.perf_counter() - t0) * 1000)
            count_ok += 1
        except ModbusError as e:
            count_err += 1
            err_kinds[str(e)[:60]] += 1
        except OSError as e:
            count_err += 1
            err_kinds[f"OSError: {str(e)[:50]}"] += 1
            try:
                mb.close()
                mb = make_client(args)
                if not mb.connect():
                    time.sleep(0.5)
            except Exception:  # noqa: BLE001
                pass

        elapsed = args.duration - (time.monotonic() - start)
        if count_ok % 200 == 0 and time.monotonic() - last_progress >= 2.0:
            last_progress = time.monotonic()
            print(f"  [剩 {elapsed:6.1f}s] ok={count_ok:,} err={count_err}",
                  flush=True)

    elapsed = time.monotonic() - start
    rtts = sorted(rtt_samples)
    stat = {
        "transport": args.transport,
        "ok": count_ok,
        "err": count_err,
        "err_rate_pct": round(count_err / max(count_ok + count_err, 1) * 100, 3),
        "qps": round(count_ok / max(elapsed, 0.1), 1),
        "rtt_p50_ms": round(statistics.median(rtts), 2) if rtts else 0,
        "rtt_p95_ms": round(rtts[int(len(rtts) * 0.95)], 2) if len(rtts) >= 20 else 0,
        "rtt_max_ms": round(rtts[-1], 2) if rtts else 0,
        "top_errors": err_kinds.most_common(3),
    }
    return stat


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=DEVICE_IP)
    ap.add_argument("--mb-port", type=int, default=MODBUS_TCP_PORT)
    ap.add_argument("--transport", choices=["tcp", "rtu"], default="tcp")
    ap.add_argument("--port", default=MODBUS_RTU_PORT, help="RTU 串口")
    ap.add_argument("--baud", type=int, default=MODBUS_RTU_BAUDRATE)
    ap.add_argument("--parity", default=MODBUS_RTU_PARITY)
    ap.add_argument("--stopbits", type=int, default=MODBUS_RTU_STOPBITS)
    ap.add_argument("--bytesize", type=int, default=MODBUS_RTU_BYTESIZE)
    ap.add_argument("--slave", type=int, default=MODBUS_RTU_SLAVE_ID)
    ap.add_argument("--duration", type=float, default=300)
    ap.add_argument("--qps", type=int, default=0, help="限速 (0=不限)")
    args = ap.parse_args()

    print(f"=== Modbus {args.transport.upper()} 长稳压力 ===")
    print(f"时长 {args.duration}s, {'不限速' if args.qps == 0 else f'{args.qps} QPS'}")
    print("按 Ctrl+C 提前停止\n")

    stat = run(args)
    if stat is None:
        return 1

    print(f"\n结果: ok={stat['ok']:,} err={stat['err']:,} ({stat['err_rate_pct']}%) "
          f"qps={stat['qps']}")
    print(f"RTT P50={stat['rtt_p50_ms']}ms P95={stat['rtt_p95_ms']}ms "
          f"max={stat['rtt_max_ms']}ms")
    for msg, cnt in stat["top_errors"]:
        print(f"  [{cnt:>6}] {msg}")

    fail = stat["err_rate_pct"] > 1.0
    print("\n=== 完成 ===" + (" (FAILED)" if fail else " (PASS)"))
    return 2 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
