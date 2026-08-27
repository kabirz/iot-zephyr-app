"""压力测试 - CANopen 总线高负载 (SDO 风暴 + 未知 COB-ID 帧洪水).

两阶段 (替代 io-edge-hub 的自定义 CAN flood, 该协议在合并版固件已移除):
  1. 未知 ID 原始帧风暴: 设备硬件过滤器应静默丢弃, 之后 SDO/心跳不受影响
  2. SDO expedited 读循环 (0x1000:0): 错误率 <1%, 心跳持续

用法:
  python stress_sdo_load.py --channel can0 --node 10 --duration 60
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
import can  # noqa: E402

from config import CAN_CHANNEL, NODE_ID, BITRATE  # noqa: E402

stop_flag = threading.Event()


def signal_handler(sig, frame):
    print("\n收到 Ctrl+C, 正在停止...")
    stop_flag.set()


signal.signal(signal.SIGINT, signal_handler)


def send_nmt_reset(bus, node):
    """NMT Reset Node 自愈帧."""
    try:
        bus.send(can.Message(arbitration_id=0x000,
                             data=bytes([0x81, node]),
                             is_extended_id=False))
    except Exception:
        pass


def phase_frame_flood(bus, duration_s: float, qps_limit: int):
    """阶段 1: 未知 COB-ID 帧 (设备 NMT/SYNC/SDO/RPDO 过滤器之外) 洪水."""
    print(f"\n[阶段 1] 未知帧洪水 ({duration_s}s, {qps_limit} QPS, id=0x6EF)")
    interval = 1.0 / qps_limit if qps_limit > 0 else 0
    count_ok = count_err = 0
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
            data = bytes([seq & 0xFF]) + bytes([0xA0, 0xA1, 0xA2, 0xA3, 0xA4,
                                                0xA5, 0xA6])
            bus.send(__import__("can").Message(
                arbitration_id=0x6EF, data=data, is_extended_id=False))
            count_ok += 1
        except Exception as e:  # noqa: BLE001
            count_err += 1
            err_kinds[str(e)[:50]] += 1
        seq += 1

    return {"phase": "frame_flood", "sent": count_ok, "err": count_err,
            "qps": round(count_ok / duration_s, 1),
            "top_errors": err_kinds.most_common(3)}


def phase_sdo_loop(bus, node: int, duration_s: float, qps_limit: int, timeout_s: float):
    """阶段 2: SDO expedited 读循环 + 心跳间隙监测 (单线程收发内联分类,
    避免 python-can Bus 的跨线程 recv 竞争)."""
    import can

    sdo_tx, sdo_rx = 0x600 + node, 0x580 + node
    hb_cob = 0x700 + node
    req = bytes([0x40]) + bytes([0x00, 0x10, 0x00]) + bytes(3)

    last_hb = time.monotonic()
    hb_gaps = []
    count_ok = count_err = 0
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
            bus.send(can.Message(arbitration_id=sdo_tx, data=req,
                                 is_extended_id=False))
        except Exception as e:  # noqa: BLE001
            count_err += 1
            err_kinds[f"send {str(e)[:40]}"] += 1
            continue

        # 等响应期间顺带处理心跳帧 (内联分类)
        t0 = time.perf_counter()
        deadline = t0 + timeout_s
        got = False
        while time.monotonic() < deadline:
            msg = bus.recv(timeout=max(0.0, deadline - time.monotonic()))
            if msg is None or msg.is_extended_id or msg.arbitration_id == sdo_tx:
                continue
            aid = msg.arbitration_id
            if aid == sdo_rx:
                rtts.append((time.perf_counter() - t0) * 1000)
                got = True
                break
            if aid == hb_cob:
                gap = now_ts() - last_hb
                if gap > 2.5:
                    hb_gaps.append(round(gap, 2))
                last_hb = now_ts()
                # 心跳不消费本次请求等待, 继续等 SDO 响应
                continue
        if got:
            count_ok += 1
            consec_to = 0
        else:
            count_err += 1
            consec_to += 1
            err_kinds["timeout"] += 1
            if consec_to >= 8:
                consec_to = 0
                recoveries += 1
                print(f"  [自愈 {recoveries}] 连续超时, 发 NMT 复位...", flush=True)
                send_nmt_reset(bus, node)
                time.sleep(4)

        # 本轮结束后若无 SDO 响应也顺带检查心跳间隔
        cur_gap = now_ts() - last_hb
        if cur_gap > 2.5:
            hb_gaps.append(round(cur_gap, 2))
            last_hb = now_ts()

        if count_ok % 100 == 0 and count_ok:
            remaining = end - time.monotonic()
            print(f"  [剩 {remaining:5.1f}s] ok={count_ok} err={count_err}",
                  flush=True)

    rtts.sort()
    return {
        "phase": "sdo_loop",
        "ok": count_ok,
        "err": count_err,
        "err_rate_pct": round(count_err / max(count_ok + count_err, 1) * 100, 3),
        "qps": round(count_ok / duration_s, 1),
        "rtt_p50_ms": round(statistics.median(rtts), 2) if rtts else 0,
        "rtt_p95_ms": round(rtts[int(len(rtts) * 0.95)], 2) if len(rtts) >= 20 else 0,
        "rtt_max_ms": round(rtts[-1], 2) if rtts else 0,
        "hb_abnormal_gaps": len(hb_gaps),
        "recoveries": recoveries,
        "top_errors": err_kinds.most_common(3),
    }


def now_ts():
    return time.monotonic()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", default=CAN_CHANNEL)
    ap.add_argument("--interface", default="socketcan")
    ap.add_argument("--node", type=int, default=NODE_ID)
    ap.add_argument("--bitrate", type=int, default=BITRATE)
    ap.add_argument("--duration", type=float, default=60.0, help="每阶段秒数")
    ap.add_argument("--flood-qps", type=int, default=500)
    ap.add_argument("--sdo-qps", type=int, default=50)
    ap.add_argument("--skip-flood", action="store_true")
    args = ap.parse_args()

    import can

    print(f"=== CANopen 总线压力 ===")
    print(f"channel={args.channel} node={args.node}, 每阶段 {args.duration}s")

    try:
        bus = can.interface.Bus(interface=args.interface, channel=args.channel,
                                bitrate=args.bitrate, receive_own_messages=False)
    except Exception as e:  # noqa: BLE001
        print(f"CAN 接口不可用: {e}")
        return 1

    results = []
    try:
        signal.signal(signal.SIGINT, signal_handler)
        if not args.skip_flood:
            r1 = phase_frame_flood(bus, args.duration, args.flood_qps)
            results.append(r1)
            print(f"  发送 {r1['sent']:,} 帧 "
                  f"({r1['qps']} qps, 发送侧错误 {r1['err']})")
            for msg, cnt in r1["top_errors"]:
                print(f"    [{cnt:>6}] {msg}")
            if stop_flag.is_set():
                return 0

        r2 = phase_sdo_loop(bus, args.node, args.duration, args.sdo_qps, 1.0)
        results.append(r2)
        print(f"  ok={r2['ok']:,} err={r2['err']:,} ({r2['err_rate_pct']}%) "
              f"qps={r2['qps']}")
        print(f"  RTT P50={r2['rtt_p50_ms']}ms P95={r2['rtt_p95_ms']}ms "
              f"max={r2['rtt_max_ms']}ms")
        print(f"  心跳异常间隙 (>2.5s): {r2['hb_abnormal_gaps']}")
        for msg, cnt in r2["top_errors"]:
            print(f"    [{cnt:>6}] {msg}")
    finally:
        stop_flag.set()
        bus.shutdown()

    fail = any(r.get("err_rate_pct", 0) > 1.0 or r.get("hb_abnormal_gaps", 0) > 2
               for r in results)
    print("\n=== 完成 ===" + (" (FAILED)" if fail else " (PASS)"))
    return 2 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
