"""压力测试 - Modbus TCP / RTU 长时间高频读.

策略:
- 主线程持续 FC03 读 holding 18 个寄存器
- 统计: 成功/失败次数, 错误类型分布, RTT 长尾, 累计 QPS
- 定期打印进度, 按 Ctrl+C 优雅停止
- 支持 TCP (默认) 与 RTU 两种传输

用法:
  # TCP (默认)
  python stress_modbus_long.py --ip 192.168.12.101 --duration 3600
  python stress_modbus_long.py --duration 60 --qps 200    # 限速 200 QPS

  # RTU (RS485, 注意 QPS 受 9600bps 物理层限制)
  python stress_modbus_long.py --transport rtu --port /dev/ttyUSB0 --baud 9600 \\
      --slave 1 --duration 300
"""
import argparse
import signal
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
)

stop_flag = threading.Event()


def signal_handler(sig, frame):
    print("\n收到 Ctrl+C, 正在停止...")
    stop_flag.set()


signal.signal(signal.SIGINT, signal_handler)


def make_client(args) -> MbClient:
    """按 --transport 创建 TCP 或 RTU 客户端."""
    if args.transport == "tcp":
        return MbClient(ip=args.ip, port=args.port, transport="tcp")
    if args.transport == "rtu":
        return MbClient(
            transport="rtu", serial_port=args.port, baudrate=args.baud,
            parity=args.parity, stopbits=args.stopbits, bytesize=args.bytesize,
        )
    raise ValueError(f"未知 --transport: {args.transport}")


def run(args):
    mb = make_client(args)
    if not mb.connect():
        print(f"无法连接 Modbus {mb.id_str}")
        return None

    slave = args.slave
    interval = 1.0 / args.qps if args.qps > 0 else 0
    count_ok = 0
    count_err = 0
    err_kinds = Counter()
    rtt_samples = []
    start = time.monotonic()
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
            mb.read_holding(0, 18, slave=slave)
            elapsed_ms = (time.perf_counter() - t0) * 1000
            rtt_samples.append(elapsed_ms)
            count_ok += 1
        except ModbusError as e:
            count_err += 1
            err_kinds[str(e)[:60]] += 1
        except OSError as e:
            count_err += 1
            err_kinds[f"OSError: {str(e)[:50]}"] += 1
            # 重连
            try:
                mb.close()
                mb = make_client(args)
                if not mb.connect():
                    time.sleep(0.5)
            except Exception:
                pass

        # 每 10 秒打印进度
        elapsed = int(time.monotonic() - start)
        if elapsed >= last_progress + 10:
            last_progress = elapsed
            if count_ok + count_err > 0:
                cur_qps = (count_ok + count_err) / elapsed
                err_rate = count_err / (count_ok + count_err) * 100
                print(f"  [{elapsed:6d}s] 总={(count_ok+count_err):>7}  "
                      f"OK={count_ok:>7}  ERR={count_err:>5} ({err_rate:5.2f}%)  "
                      f"avg_qps={cur_qps:6.1f}", flush=True)

    mb.close()
    actual_duration = time.monotonic() - start

    rtt_samples.sort()
    return {
        "transport": args.transport,
        "endpoint": mb.id_str,
        "duration_s": round(actual_duration, 1),
        "ok": count_ok,
        "err": count_err,
        "err_rate_pct": round(count_err / max(count_ok + count_err, 1) * 100, 3),
        "avg_qps": round((count_ok + count_err) / actual_duration, 1) if actual_duration > 0 else 0,
        "rtt_min_ms": round(rtt_samples[0], 2) if rtt_samples else 0,
        "rtt_p50_ms": round(rtt_samples[len(rtt_samples) // 2], 2) if rtt_samples else 0,
        "rtt_p95_ms": round(rtt_samples[int(len(rtt_samples) * 0.95)], 2) if rtt_samples else 0,
        "rtt_p99_ms": round(rtt_samples[int(len(rtt_samples) * 0.99)], 2) if rtt_samples else 0,
        "rtt_max_ms": round(rtt_samples[-1], 2) if rtt_samples else 0,
        "top_errors": err_kinds.most_common(5),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--transport", choices=["tcp", "rtu"], default="tcp")
    # TCP 参数
    ap.add_argument("--ip", default=DEVICE_IP)
    ap.add_argument("--port", default=None,
                    help=f"TCP 端口 (默认 {MODBUS_TCP_PORT}); RTU 模式下为串口设备 (默认 {MODBUS_RTU_PORT})")
    # RTU 参数
    ap.add_argument("--baud", type=int, default=MODBUS_RTU_BAUDRATE)
    ap.add_argument("--parity", default=MODBUS_RTU_PARITY)
    ap.add_argument("--stopbits", type=int, default=MODBUS_RTU_STOPBITS)
    ap.add_argument("--bytesize", type=int, default=MODBUS_RTU_BYTESIZE)
    ap.add_argument("--slave", type=int, default=MODBUS_RTU_SLAVE_ID,
                    help="Modbus slave_id (RTU 必填; TCP 默认 1)")
    # 通用
    ap.add_argument("--duration", type=float, default=300, help="运行秒数 (默认 300)")
    ap.add_argument("--qps", type=int, default=0, help="限速 QPS (0=不限速)")
    args = ap.parse_args()

    # 处理 port 默认值 (TCP/RTU 不同含义)
    if args.port is None:
        args.port = str(MODBUS_TCP_PORT) if args.transport == "tcp" else MODBUS_RTU_PORT
    else:
        args.port = args.port  # 字符串即可 (TCP 端口号也是字符串传给 connect)

    print(f"=== Modbus 长时间压力测试 ({args.transport.upper()}) ===")
    print(f"端点: {args.port if args.transport == 'rtu' else f'{args.ip}:{args.port}'}, "
          f"slave={args.slave}")
    print(f"时长: {args.duration}s, 限速: {'不限' if args.qps == 0 else str(args.qps) + ' QPS'}")
    print(f"按 Ctrl+C 提前停止\n")

    result = run(args)
    if result is None:
        return 1

    print("\n=== 压力测试结果 ===")
    print(f"  传输: {result['transport'].upper()}  端点: {result['endpoint']}")
    print(f"  实际运行: {result['duration_s']}s")
    print(f"  成功: {result['ok']:,}  失败: {result['err']:,}  "
          f"错误率: {result['err_rate_pct']}%")
    print(f"  平均 QPS: {result['avg_qps']}")
    print(f"  RTT  min={result['rtt_min_ms']}ms  P50={result['rtt_p50_ms']}ms  "
          f"P95={result['rtt_p95_ms']}ms  P99={result['rtt_p99_ms']}ms  "
          f"max={result['rtt_max_ms']}ms")
    if result["top_errors"]:
        print(f"  Top 错误:")
        for msg, cnt in result["top_errors"]:
            print(f"    [{cnt:>5}] {msg}")

    return 0 if result["err_rate_pct"] < 1.0 else 2


if __name__ == "__main__":
    sys.exit(main())

