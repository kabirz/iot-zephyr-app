"""压力测试 - Modbus TCP + UDP 混合负载.

策略:
- N 个 Modbus 线程持续 FC03 读
- M 个 UDP 线程持续发 GET_IP / SET_TIME
- 共享 stop_flag, 汇总各自统计

目的: 验证设备在多协议并行下的稳定性, 是否有资源争抢/死锁/复位.

用法:
  python stress_mixed.py --ip 192.168.12.101 --duration 600 \\
      --modbus-threads 2 --udp-threads 2
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
from common.udp_client import UdpClient, UdpError  # noqa: E402
from config import DEVICE_IP, MODBUS_TCP_PORT, UDP_PORT  # noqa: E402

stop_flag = threading.Event()


def signal_handler(sig, frame):
    print("\n收到 Ctrl+C, 正在停止...")
    stop_flag.set()


signal.signal(signal.SIGINT, signal_handler)


def modbus_worker(ip, port, idx, out, qps_limit):
    """qps_limit=0 表示不限速."""
    mb = MbClient(ip, port)
    if not mb.connect():
        out[idx] = {"kind": "modbus", "ok": 0, "err": 1, "err_kinds": Counter(["connect failed"])}
        return
    ok = 0
    err = 0
    err_kinds = Counter()
    interval = 1.0 / qps_limit if qps_limit > 0 else 0
    next_send = time.monotonic()
    last_report = time.monotonic()
    while not stop_flag.is_set():
        if interval > 0:
            now = time.monotonic()
            if now < next_send:
                time.sleep(min(0.001, next_send - now))
            next_send += interval
        try:
            mb.read_holding(0, 18)
            ok += 1
        except (ModbusError, OSError) as e:
            err += 1
            err_kinds[str(e)[:50]] += 1
            try:
                mb.close()
                mb = MbClient(ip, port)
                if not mb.connect():
                    time.sleep(0.5)
            except Exception:
                pass
        # 定期上报进度 (进度循环实时显示, 而非仅结束时)
        if time.monotonic() - last_report >= 1.0:
            out[idx] = {"kind": "modbus", "ok": ok, "err": err,
                        "err_kinds": err_kinds.copy()}
            last_report = time.monotonic()
    mb.close()
    out[idx] = {"kind": "modbus", "ok": ok, "err": err, "err_kinds": err_kinds}


def udp_worker(ip, port, idx, out, qps_limit):
    """qps_limit=0 表示不限速."""
    udp = UdpClient(ip, port, timeout=2.0)
    ok = 0
    err = 0
    err_kinds = Counter()
    toggle = 0
    interval = 1.0 / qps_limit if qps_limit > 0 else 0
    next_send = time.monotonic()
    last_report = time.monotonic()
    while not stop_flag.is_set():
        if interval > 0:
            now = time.monotonic()
            if now < next_send:
                time.sleep(min(0.001, next_send - now))
            next_send += interval
        try:
            if toggle % 2 == 0:
                udp.get_ip()
            else:
                udp.set_time(int(time.time()))
            ok += 1
        except (UdpError, OSError) as e:
            err += 1
            err_kinds[str(e)[:50]] += 1
            try:
                udp.close()
            except Exception:
                pass
            udp = UdpClient(ip, port, timeout=2.0)
        toggle += 1
        # 定期上报进度 (进度循环实时显示, 而非仅结束时)
        if time.monotonic() - last_report >= 1.0:
            out[idx] = {"kind": "udp", "ok": ok, "err": err,
                        "err_kinds": err_kinds.copy()}
            last_report = time.monotonic()
    udp.close()
    out[idx] = {"kind": "udp", "ok": ok, "err": err, "err_kinds": err_kinds}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=DEVICE_IP)
    ap.add_argument("--mb-port", type=int, default=MODBUS_TCP_PORT)
    ap.add_argument("--udp-port", type=int, default=UDP_PORT)
    ap.add_argument("--duration", type=float, default=300)
    ap.add_argument("--modbus-threads", type=int, default=2)
    ap.add_argument("--udp-threads", type=int, default=2)
    ap.add_argument("--modbus-qps", type=int, default=0, help="每个 Modbus 线程 QPS 限速 (0=不限)")
    ap.add_argument("--udp-qps", type=int, default=0, help="每个 UDP 线程 QPS 限速 (0=不限)")
    args = ap.parse_args()

    total_threads = args.modbus_threads + args.udp_threads
    results = [None] * total_threads
    threads = []

    print(f"=== Modbus + UDP 混合压力测试 ===")
    print(f"目标: {args.ip}, 时长: {args.duration}s")
    print(f"Modbus 线程: {args.modbus_threads} (端口 {args.mb_port}, "
          f"{'不限速' if args.modbus_qps == 0 else str(args.modbus_qps) + ' QPS/线程'})")
    print(f"UDP 线程:    {args.udp_threads} (端口 {args.udp_port}, "
          f"{'不限速' if args.udp_qps == 0 else str(args.udp_qps) + ' QPS/线程'})")
    print(f"按 Ctrl+C 提前停止\n")

    idx = 0
    for _ in range(args.modbus_threads):
        t = threading.Thread(target=modbus_worker,
                             args=(args.ip, args.mb_port, idx, results, args.modbus_qps),
                             daemon=True)
        t.start()
        threads.append(t)
        idx += 1
    for _ in range(args.udp_threads):
        t = threading.Thread(target=udp_worker,
                             args=(args.ip, args.udp_port, idx, results, args.udp_qps),
                             daemon=True)
        t.start()
        threads.append(t)
        idx += 1

    # 进度
    start = time.monotonic()
    next_print = start + 10
    while any(t.is_alive() for t in threads) and not stop_flag.is_set():
        # --duration 生效: 到时自动停止
        if time.monotonic() - start >= args.duration:
            stop_flag.set()
            break
        now = time.monotonic()
        if now >= next_print:
            elapsed = now - start
            mb_ok = sum(r["ok"] for r in results if r and r["kind"] == "modbus")
            mb_err = sum(r["err"] for r in results if r and r["kind"] == "modbus")
            udp_ok = sum(r["ok"] for r in results if r and r["kind"] == "udp")
            udp_err = sum(r["err"] for r in results if r and r["kind"] == "udp")
            print(f"  [{elapsed:6.0f}s] Modbus: ok={mb_ok:>7} err={mb_err:>5}  "
                  f"UDP: ok={udp_ok:>7} err={udp_err:>5}", flush=True)
            next_print = now + 10
        time.sleep(0.5)

    for t in threads:
        t.join(timeout=5)

    actual_duration = time.monotonic() - start

    # 汇总
    mb_ok = sum(r["ok"] for r in results if r and r["kind"] == "modbus")
    mb_err = sum(r["err"] for r in results if r and r["kind"] == "modbus")
    udp_ok = sum(r["ok"] for r in results if r and r["kind"] == "udp")
    udp_err = sum(r["err"] for r in results if r and r["kind"] == "udp")
    mb_errs = Counter()
    udp_errs = Counter()
    for r in results:
        if not r:
            continue
        if r["kind"] == "modbus":
            mb_errs.update(r["err_kinds"])
        else:
            udp_errs.update(r["err_kinds"])

    print("\n=== 混合压力测试结果 ===")
    print(f"  实际运行: {actual_duration:.1f}s")
    print(f"  Modbus: ok={mb_ok:,}  err={mb_err:,}  "
          f"错误率={mb_err/max(mb_ok+mb_err,1)*100:.3f}%  "
          f"QPS={(mb_ok+mb_err)/actual_duration:.1f}")
    print(f"  UDP:    ok={udp_ok:,}  err={udp_err:,}  "
          f"错误率={udp_err/max(udp_ok+udp_err,1)*100:.3f}%  "
          f"QPS={(udp_ok+udp_err)/actual_duration:.1f}")
    if mb_errs:
        print(f"  Modbus Top 错误:")
        for msg, cnt in mb_errs.most_common(3):
            print(f"    [{cnt:>5}] {msg}")
    if udp_errs:
        print(f"  UDP Top 错误:")
        for msg, cnt in udp_errs.most_common(3):
            print(f"    [{cnt:>5}] {msg}")

    fail = mb_err / max(mb_ok + mb_err, 1) * 100 > 1.0 or \
           udp_err / max(udp_ok + udp_err, 1) * 100 > 5.0
    return 2 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
