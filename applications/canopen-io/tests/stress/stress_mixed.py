"""压力测试 - Modbus TCP + UDP 混合负载 (多协议并行稳定性).

N 个 Modbus FC03 线程 + M 个 UDP 命令线程持续 N 秒, 汇总错误率.
通过判据: 总错误率 < 1% 且无设备复位 (结束后 TCP 可用).

用法:
  python stress_mixed.py --ip 192.168.12.101 --duration 600 \\
      --modbus-threads 2 --udp-threads 2
"""
import argparse
import signal
import threading
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common.modbus_client import MbClient, ModbusError  # noqa: E402
from common.udp_client import UdpClient, UdpError  # noqa: E402
from config import DEVICE_IP, MODBUS_TCP_PORT, UDP_PORT, HOLDING_COUNT  # noqa: E402

stop_flag = threading.Event()


def signal_handler(sig, frame):
    print("\n收到 Ctrl+C, 正在停止...")
    stop_flag.set()


signal.signal(signal.SIGINT, signal_handler)


def modbus_worker(ip, port, idx, out, qps_limit):
    mb = MbClient(ip, port)
    if not mb.connect():
        out[idx] = {"kind": "modbus", "ok": 0, "err": 1,
                    "err_kinds": Counter(["connect failed"])}
        return
    ok = err = 0
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
            mb.read_holding(0, HOLDING_COUNT)
            ok += 1
        except (ModbusError, OSError) as e:
            err += 1
            err_kinds[str(e)[:50]] += 1
            try:
                mb.close()
                mb = MbClient(ip, port)
                if not mb.connect():
                    time.sleep(0.5)
            except Exception:  # noqa: BLE001
                pass
        if time.monotonic() - last_report >= 1.0:
            out[idx] = {"kind": "modbus", "ok": ok, "err": err,
                        "err_kinds": err_kinds.copy()}
            last_report = time.monotonic()

    try:
        mb.close()
    except Exception:  # noqa: BLE001
        pass
    out[idx] = {"kind": "modbus", "ok": ok, "err": err, "err_kinds": err_kinds}


def udp_worker(ip, port, idx, out, qps_limit):
    udp = UdpClient(ip, port, timeout=2.0)
    ok = err = 0
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
            except Exception:  # noqa: BLE001
                pass
            udp = UdpClient(ip, port, timeout=2.0)
        toggle += 1
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
    ap.add_argument("--modbus-qps", type=int, default=0)
    ap.add_argument("--udp-qps", type=int, default=0)
    args = ap.parse_args()

    total_threads = args.modbus_threads + args.udp_threads
    results = [None] * total_threads

    print(f"=== Modbus + UDP 混合压力 ===")
    print(f"目标 {args.ip}, 时长 {args.duration}s")
    print(f"Modbus {args.modbus_threads} 线程 / UDP {args.udp_threads} 线程\n")

    threads = []
    for i in range(args.modbus_threads):
        threads.append(threading.Thread(target=modbus_worker,
                                        args=(args.ip, args.mb_port, i, results,
                                              args.modbus_qps), daemon=True))
    for j in range(args.udp_threads):
        threads.append(threading.Thread(target=udp_worker,
                                        args=(args.ip, args.udp_port,
                                              args.modbus_threads + j, results,
                                              args.udp_qps), daemon=True))
    signal.signal(signal.SIGINT, signal_handler)
    for t in threads:
        t.start()

    start = time.monotonic()
    try:
        while time.monotonic() - start < args.duration and \
                any(t.is_alive() for t in threads):
            time.sleep(1)
            ok = sum(r["ok"] for r in results if r)
            err = sum(r["err"] for r in results if r)
            print(f"\r  [{time.monotonic() - start:6.1f}s] ok={ok:,} err={err:,}",
                  end="", flush=True)
    except KeyboardInterrupt:
        stop_flag.set()
    stop_flag.set()
    for t in threads:
        t.join(timeout=5)
    print()

    total_ok = sum(r["ok"] for r in results if r)
    total_err = sum(r["err"] for r in results if r)
    kinds = Counter()
    kinds_by_kind = {}
    for r in results:
        if r:
            kinds.update(r["err_kinds"])
            key = f"{r['kind']}:{r['ok']}/{r['err']}"
            kinds_by_kind[key] = True

    print(f"\n=== 结果 ===")
    for k in sorted(kinds_by_kind):
        print(f"  {k}")
    rate = total_err / max(total_ok + total_err, 1) * 100
    print(f"总错误率 {rate:.3f}%")

    # 结束后设备仍应可服务
    cli = MbClient(ip=args.ip)
    alive = cli.connect() and _read_ok(cli)
    cli.close()
    print(f"设备存活: {'是' if alive else '否'}")

    fail = rate > 1.0 or not alive
    print("\n=== 完成 ===" + (" (FAILED)" if fail else " (PASS)"))
    return 2 if fail else 0


def _read_ok(cli):
    try:
        cli.read_holding(0, 1)
        return True
    except Exception:  # noqa: BLE001
        return False


if __name__ == "__main__":
    sys.exit(main())
