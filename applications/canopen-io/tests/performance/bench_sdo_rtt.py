"""性能测试 - CANopen SDO expedited 读 RTT (0x1000:0 设备类型).

度量单次 SDO 往返 (0x600+node → 0x580+node) 延迟分布; 对应 io-edge-hub
的 bench_can_version (自定义 CAN VERSION 协议).

用法:
  python bench_sdo_rtt.py --channel can0 --node 10 --count 500
"""
import argparse
import json
import statistics
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import can  # noqa: E402

from config import CAN_CHANNEL, NODE_ID, BITRATE  # noqa: E402


def send_recovery(bus, node: int):
    """连续超时后的自愈: 发 NMT Reset Node 让设备重建 CANopen 栈."""
    try:
        bus.send(can.Message(arbitration_id=0x000,
                             data=bytes([0x81, node]),
                             is_extended_id=False))
    except Exception as e:  # noqa: BLE001
        print(f"NMT 恢复帧发送失败: {e}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", default=CAN_CHANNEL)
    ap.add_argument("--interface", default="socketcan")
    ap.add_argument("--node", type=int, default=NODE_ID)
    ap.add_argument("--bitrate", type=int, default=BITRATE)
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--timeout", type=float, default=1.0,
                    help="单次响应等待上限 (秒)")
    ap.add_argument("--recover-after", type=int, default=5,
                    help="连续超时多少次后发 NMT 复位自救")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    print(f"=== CANopen SDO RTT (channel={args.channel} node={args.node}, "
          f"{args.count} 次) ===\n")

    bus = can.interface.Bus(interface=args.interface, channel=args.channel,
                            bitrate=args.bitrate,
                            receive_own_messages=False)
    bus.set_filters([{"can_id": 0x580 + args.node, "can_mask": 0x7FF}])
    sdo_tx = 0x600 + args.node
    req = bytes([0x40]) + struct.pack("<H", 0x1000) + bytes([0]) + bytes(3)

    rtts = []
    timeouts = 0
    consec_to = 0
    recoveries = 0
    try:
        # 排空 RX
        end = time.monotonic() + 0.2
        while time.monotonic() < end:
            if bus.recv(timeout=0.05) is None:
                break

        max_attempts = args.count * 3 + 100
        attempts = 0
        while len(rtts) < args.count and attempts < max_attempts:
            attempts += 1
            bus.send(can.Message(arbitration_id=sdo_tx, data=req,
                                 is_extended_id=False))
            t0 = time.perf_counter()
            deadline = t0 + args.timeout
            got = False
            while time.monotonic() < deadline:
                msg = bus.recv(timeout=max(0.0, deadline - time.monotonic()))
                if msg is None:
                    break
                rtts.append((time.perf_counter() - t0) * 1000)
                got = True
                break
            if got:
                consec_to = 0
            else:
                timeouts += 1
                consec_to += 1
                if consec_to >= args.recover_after:
                    consec_to = 0
                    recoveries += 1
                    print(f"[ok={len(rtts)} to={timeouts}] 连续超时, "
                          f"NMT 自愈第 {recoveries} 次...", flush=True)
                    send_recovery(bus, args.node)
                    time.sleep(4)

        rtts.sort()

        def pct(p):
            if len(rtts) < 20:
                return None
            idx = min(int(len(rtts) * p / 100), len(rtts) - 1)
            return round(rtts[idx], 3)

        stat = {
            "count": len(rtts),
            "timeouts": timeouts,
            "recoveries": recoveries,
            "min_ms": round(rtts[0], 3) if rtts else None,
            "p50_ms": round(statistics.median(rtts), 3) if rtts else None,
            "p95_ms": pct(95),
            "p99_ms": pct(99),
            "max_ms": round(rtts[-1], 3) if rtts else None,
            "mean_ms": round(statistics.mean(rtts), 3) if rtts else None,
        }
        fmt = lambda v: f"{v:.2f}" if v is not None else "N/A"  # noqa: E731
        print(f"成功 {stat['count']}/{args.count} (超时 {timeouts}, "
              f"自愈 {recoveries})")
        print(f"min={fmt(stat['min_ms'])}ms P50={fmt(stat['p50_ms'])}ms "
              f"P95={fmt(stat['p95_ms'])}ms P99={fmt(stat['p99_ms'])}ms "
              f"max={fmt(stat['max_ms'])}ms mean={fmt(stat['mean_ms'])}ms")
        err_rate = timeouts / max(len(rtts) + timeouts, 1) * 100
        print(f"\n错误率 {err_rate:.2f}% " +
              ("PASS" if err_rate < 1.0 and len(rtts) == args.count
               else "FAIL (阈值 1%)"))
        results = {"sdo_rtt": stat}
        if args.json:
            Path(args.json).write_text(json.dumps(results, indent=2, ensure_ascii=False))
            print(f"\n结果已写入 {args.json}")
        return 2 if (err_rate >= 1.0 or len(rtts) != args.count) else 0
    finally:
        bus.shutdown()


if __name__ == "__main__":
    sys.exit(main())
