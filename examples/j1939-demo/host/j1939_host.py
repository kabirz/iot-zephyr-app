#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
j1939_host.py — 与 examples/j1939-demo (Zephyr J1939 节点) 通信的上位机

纯 Python 标准库实现 (SocketCAN raw socket), 无第三方依赖。
与固件侧 src/j1939*.c 逻辑对称:

  - 29 位 ID <-> 优先级/PGN/SA/DA 编解码
  - 简化版地址声明 (J1939/81): Request 探询 + 竞争窗口 + NAME 仲裁
  - 应答对我方地址的声明请求; 地址被更小 NAME 抢占时自动让步重声明
  - TP BAM 收包重组 (TP.CM_BAM + TP.DT, 超时 1.25s 丢弃)
  - 解码 demo 的 PGN 65280 (单帧 IO 快照) / 65281 (BAM 扩展快照)
    以及任意其他 PGN 的十六进制转储

用法:
  ./j1939_host.py -i can0                          # 声明地址并监听 10s
  ./j1939_host.py -i can0 -t 0                     # 持续监听 (Ctrl-C 退出)
  ./j1939_host.py -i can0 --req 65280 --req 65281  # 向节点请求指定 PGN
  ./j1939_host.py -i vcan0 --no-claim -t 5         # 纯监听, 不声明地址

硬件连接: CAN 适配器 (如 USB-CAN) 接入总线, 250 kbps:
  ip link set can0 type can bitrate 250000 && ip link set can0 up
"""
import argparse
import queue
import signal
import socket
import struct
import sys
import threading
import time

# ---------------- CAN 常量 ----------------
CAN_EFF_FLAG = 0x80000000     # 扩展帧 (29 位 ID) 标志
CAN_EFF_MASK = 0x1FFFFFFF
CAN_FRAME = struct.Struct("=IB3x8s")   # linux can_frame
CAN_FILTER = struct.Struct("=II")      # linux can_filter
SOL_CAN_RAW = getattr(socket, "SOL_CAN_RAW", 101)
CAN_RAW_FILTER = getattr(socket, "CAN_RAW_FILTER", 1)

# ---------------- J1939 常量 (与固件 j1939.h 对应) ----------------
PGN_REQUEST = 59904           # 0xEA00, PDU1
PGN_TP_DT = 60160             # 0xEB00, PDU1
PGN_TP_CM = 60416             # 0xEC00, PDU1
PGN_ADDRESS_CLAIMED = 60928   # 0xEE00, PDU1

SA_NULL = 254
SA_GLOBAL = 255

TP_CM_RTS = 0x10
TP_CM_BAM = 0x20

PGN_NAMES = {
    PGN_REQUEST: "Request",
    PGN_TP_DT: "TP.DT",
    PGN_TP_CM: "TP.CM",
    PGN_ADDRESS_CLAIMED: "Address Claimed",
    65280: "IO Status",
    65281: "IO Status Ext",
}


# ---------------- ID 编解码 ----------------
def pgn_is_pdu2(pgn):
    return (pgn >> 8) & 0xFF >= 0xF0


def make_id(prio, pgn, sa, da):
    cid = ((prio & 0x7) << 26) | ((pgn & 0x3FFFF) << 8) | sa
    if not pgn_is_pdu2(pgn):
        cid |= da << 8
    return cid


def id_to_prio(cid):
    return (cid >> 26) & 0x7


def id_to_sa(cid):
    return cid & 0xFF


def id_to_da(cid):
    return (cid >> 8) & 0xFF


def id_to_pgn(cid):
    pf = (cid >> 16) & 0xFF
    if pf >= 0xF0:
        return (cid >> 8) & 0x3FFFF   # PDU2: 含组扩展
    return (cid >> 8) & 0x3FF00       # PDU1: 去掉 DA


# ---------------- NAME (64 位设备名) ----------------
def pack_name(function=0, func_inst=0, ecu_inst=0, veh_sys=0, veh_inst=0,
              industry=0, mfg=0, identity=0):
    return ((ecu_inst & 0x7)
            | ((func_inst & 0x1F) << 3)
            | ((function & 0xFF) << 8)
            | ((veh_sys & 0x7F) << 17)
            | ((veh_inst & 0xF) << 24)
            | ((industry & 0x7) << 28)
            | ((identity & 0x1FFFFF) << 32)
            | ((mfg & 0x7FF) << 53))


def fmt_name(name):
    return ("NAME func=%d func_inst=%d ecu_inst=%d veh_sys=0x%x veh_inst=%d "
            "ind=%d mfg=%d identity=0x%x" % (
                (name >> 8) & 0xFF, (name >> 3) & 0x1F, name & 0x7,
                (name >> 17) & 0x7F, (name >> 24) & 0xF,
                (name >> 28) & 0x7, (name >> 53) & 0x7FF,
                (name >> 32) & 0x1FFFFF))


# ---------------- 应用层解码 ----------------
def decode_io_status(data):
    seq = data[0]
    di = int.from_bytes(data[1:3], "little")
    ai0 = int.from_bytes(data[3:5], "little")
    ai1 = int.from_bytes(data[5:7], "little")
    return "seq=%d DI=0x%04x AI0=%dmV AI1=%dmV" % (seq, di, ai0, ai1)


def decode_io_status_ext(data):
    seq = data[0]
    di = int.from_bytes(data[1:3], "little")
    ai = [int.from_bytes(data[3 + i * 2:5 + i * 2], "little") for i in range(4)]
    uptime = int.from_bytes(data[11:15], "little")
    return ("seq=%d DI=0x%04x AI=%s uptime=%ds" % (
        seq, di, "/".join("%dmV" % v for v in ai), uptime))


# ---------------- BAM 收包重组 ----------------
class BamReassembler:
    """按源地址维护 BAM 会话, 乱序/超时 (1.25s) 丢弃。"""

    TIMEOUT = 1.25

    def __init__(self):
        self.sessions = {}   # sa -> dict(pgn, size, npkts, next_seq, buf, last)

    def on_cm(self, sa, data):
        if len(data) < 8 or data[0] != TP_CM_BAM:
            return None, None        # RTS/Abort 等点对点模式不处理
        size = data[1] | (data[2] << 8)
        npkts = data[3]
        pgn = data[5] | (data[6] << 8) | (data[7] << 16)
        self.sessions[sa] = dict(pgn=pgn, size=size, npkts=npkts,
                                 next_seq=1, buf=bytearray(),
                                 last=time.monotonic())
        return pgn, size

    def on_dt(self, sa, data):
        sess = self.sessions.get(sa)
        if sess is None or len(data) < 1:
            return None, None
        sess["last"] = time.monotonic()
        if data[0] != sess["next_seq"]:
            del self.sessions[sa]    # 乱序: 简化处理, 直接丢弃会话
            return None, None
        sess["buf"] += data[1:1 + 7]
        sess["next_seq"] += 1
        if sess["next_seq"] > sess["npkts"]:
            msg = bytes(sess["buf"][:sess["size"]])
            pgn = sess["pgn"]
            del self.sessions[sa]
            return pgn, msg
        return None, None

    def sweep(self):
        now = time.monotonic()
        for sa in [s for s, v in self.sessions.items()
                   if now - v["last"] > self.TIMEOUT]:
            del self.sessions[sa]


# ---------------- 主程序 ----------------
class Host:
    def __init__(self, iface, sa, name, no_claim=False, quiet=False):
        self.sa = sa
        self.name = name
        self.no_claim = no_claim
        self.quiet = quiet
        self.addr_valid = False
        self.rxq = queue.Queue()
        self.bam = BamReassembler()
        self.stats = {}
        self.t0 = time.monotonic()

        self.sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        # 只收 29 位扩展帧 (J1939 全部为扩展帧)
        self.sock.setsockopt(SOL_CAN_RAW, CAN_RAW_FILTER,
                             CAN_FILTER.pack(CAN_EFF_FLAG, CAN_EFF_FLAG))
        self.sock.bind((iface,))

    def log(self, msg):
        if not self.quiet:
            print("[%7.3f] %s" % (time.monotonic() - self.t0, msg))

    def send(self, prio, pgn, da, data):
        assert len(data) <= 8
        cid = make_id(prio, pgn, self.sa, da) | CAN_EFF_FLAG
        self.sock.send(CAN_FRAME.pack(cid, len(data),
                                      bytes(data).ljust(8, b"\xff")))

    def send_claim(self):
        self.send(6, PGN_ADDRESS_CLAIMED, SA_GLOBAL,
                  self.name.to_bytes(8, "little"))

    # ---- 地址声明 (与固件 j1939.c 相同的简化流程) ----
    def claim(self):
        sa = self.sa
        while sa < SA_NULL:
            self.claim_candidate = sa
            self.peer_seen = False

            # 1) 探询该地址是否已有主人
            self.send(6, PGN_REQUEST, sa,
                      PGN_ADDRESS_CLAIMED.to_bytes(3, "little"))

            # 2) 竞争窗口 0.5~1.5s
            time.sleep(0.5 + (time.monotonic() * 1000 % 1000) / 1000)
            self.drain_rx()

            # 3) 发出声明
            self.send_claim()

            # 4) 观察期 250ms
            time.sleep(0.25)
            self.drain_rx()

            if self.peer_seen and self.peer_name < self.name:
                self.log("SA %d busy, yielding" % sa)
                if sa == 0:
                    break
                sa -= 1
                continue

            self.sa = sa
            self.addr_valid = True
            self.log("address claimed: SA=%d" % sa)
            return True

        self.sa = SA_NULL
        self.log("address claim failed (exhausted)")
        return False

    # ---- RX ----
    def rx_thread(self):
        while True:
            raw = self.sock.recv(16)
            can_id, dlc, data = CAN_FRAME.unpack(raw)
            if not can_id & CAN_EFF_FLAG:
                continue
            self.rxq.put((can_id & CAN_EFF_MASK, data[:dlc]))

    def drain_rx(self):
        """claim 期间同步处理积压帧 (仅提取冲突信息)。"""
        try:
            while True:
                cid, data = self.rxq.get_nowait()
                self.handle_frame(cid, data, claiming=True)
        except queue.Empty:
            pass

    def handle_frame(self, cid, data, claiming=False):
        pgn = id_to_pgn(cid)
        sa = id_to_sa(cid)
        da = id_to_da(cid)

        if pgn == PGN_ADDRESS_CLAIMED:
            name = int.from_bytes(data[:8], "little") if len(data) >= 8 else -1
            if claiming and sa == self.claim_candidate:
                self.peer_seen = True
                self.peer_name = name
            if sa == self.sa and self.addr_valid and 0 <= name != self.name:
                if name < self.name:
                    self.log("SA %d lost to smaller NAME, re-claiming" % sa)
                    self.addr_valid = False
                # 对方 NAME 更大: 我方持有地址, 忽略
            self.log("18EEFF%02x Address Claimed SA=%d  %s"
                     % (sa, sa, fmt_name(name)))
            return

        if pgn == PGN_REQUEST and len(data) >= 3:
            req = data[0] | (data[1] << 8) | (data[2] << 16)
            if da in (self.sa, SA_GLOBAL) and req == PGN_ADDRESS_CLAIMED \
                    and self.addr_valid:
                self.send_claim()      # 应答对我方声明的请求
            self.log("18EA%02x%02x Request PGN %d (0x%x) SA=%d -> DA=%d"
                     % (da, sa, req, req, sa, da))
            return

        if pgn == PGN_TP_CM:
            pgn2, size = self.bam.on_cm(sa, data)
            if size is not None:
                self.log("1CECFF%02x TP.CM BAM SA=%d: %d bytes / %d pkts "
                         "-> PGN %d" % (sa, sa, size, (size + 6) // 7, pgn2))
            else:
                ctrl = data[0] if data else -1
                self.log("1CECFF%02x TP.CM ctrl=0x%02x SA=%d (非 BAM, 忽略)"
                         % (sa, ctrl, sa))
            return

        if pgn == PGN_TP_DT:
            pgn2, msg = self.bam.on_dt(sa, data)
            if pgn2 is None:
                return
            self.dispatch(pgn2, sa, msg, completed_bam=True)
            return

        self.dispatch(pgn, sa, data)

    def dispatch(self, pgn, sa, data, completed_bam=False):
        self.stats[pgn] = self.stats.get(pgn, 0) + 1
        name = PGN_NAMES.get(pgn, "?")

        if pgn == 65280:
            self.log("18FF00%02x PGN 65280 (%s) SA=%d  %s"
                     % (sa, name, sa, decode_io_status(data)))
        elif pgn == 65281:
            tag = "BAM 完成" if completed_bam else "frame"
            self.log("PGN 65281 (%s) [%s] SA=%d  %s"
                     % (name, tag, sa, decode_io_status_ext(data)))
        else:
            self.log("PGN %d (0x%x) SA=%d [%dB] %s"
                     % (pgn, pgn, sa, len(data), data.hex(" ")))

    def run(self, duration, requests, node_sa):
        threading.Thread(target=self.rx_thread, daemon=True).start()

        if self.no_claim:
            self.log("monitor only, SA=%d (not claimed)" % self.sa)
        else:
            self.claim()

        for req in requests:
            self.log("requesting PGN %d from SA %d" % (req, node_sa))
            self.send(6, PGN_REQUEST, node_sa, req.to_bytes(3, "little"))
            time.sleep(0.2)           # 留出应答时间, 再发下一个

        deadline = None if duration == 0 else time.monotonic() + duration
        try:
            while True:
                if deadline is not None and time.monotonic() > deadline:
                    break
                try:
                    cid, data = self.rxq.get(timeout=0.25)
                except queue.Empty:
                    self.bam.sweep()
                    if not self.addr_valid and not self.no_claim:
                        self.claim()   # 地址被抢占后自动重声明
                    continue
                self.handle_frame(cid, data)
                self.bam.sweep()
        except KeyboardInterrupt:
            print()

        print("---- 统计 ----")
        for pgn in sorted(self.stats):
            print("  PGN %-6d (%s) x%d"
                  % (pgn, PGN_NAMES.get(pgn, "?"), self.stats[pgn]))


def auto_int(s):
    return int(s, 0)


def main(argv=None):
    ap = argparse.ArgumentParser(description="J1939 host for j1939-demo")
    ap.add_argument("-i", "--interface", default="can0", help="CAN 接口名")
    ap.add_argument("--sa", type=auto_int, default=200,
                    help="本机首选源地址 (默认 200)")
    ap.add_argument("--node", type=auto_int, default=128,
                    help="目标节点 SA (默认 128)")
    ap.add_argument("--req", type=auto_int, action="append", default=[],
                    metavar="PGN", help="向节点请求指定 PGN, 可重复")
    ap.add_argument("-t", "--duration", type=float, default=10,
                    help="监听秒数, 0=持续 (默认 10)")
    ap.add_argument("--no-claim", action="store_true",
                    help="只监听不声明地址")
    ap.add_argument("--identity", type=auto_int, default=0x0BE123,
                    help="NAME 中的 Identity Number (演示值)")
    args = ap.parse_args(argv)

    # NAME 演示值: 与固件默认同 mfg/function, identity 不同 -> 固件 NAME 更小
    name = pack_name(function=40, mfg=0x4D2, identity=args.identity)

    host = Host(args.interface, args.sa, name,
                no_claim=args.no_claim, quiet=False)
    signal.signal(signal.SIGINT, lambda *_: sys.exit(0))
    host.run(args.duration, args.req, args.node)
    return 0


if __name__ == "__main__":
    sys.exit(main())
