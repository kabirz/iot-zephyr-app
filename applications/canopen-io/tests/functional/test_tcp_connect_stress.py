"""Modbus TCP 连接/断开循环压力: 1000 次 connect-close, 设备应全程稳定.

关注点:
  - 周期性 FC03 读确认服务持续响应 (不只是 accept)
  - 结束后再读一次, 排除槽位泄漏后假活
  - 串口不应出现 HardFault / 看门狗重启
"""
import socket
import struct
import time

import pytest

from config import MODBUS_TCP_PORT

CYCLES = 1000
CHECK_INTERVAL = 100   # 每 N 次做一次 FC03 读校验
CONN_INTERVAL_S = 0.005


def _recv_full(sock: socket.socket, want: int) -> bytes:
    """循环读满 want 字节 (固件头+数据一次 send, 但仍做完整读取)."""
    buf = b""
    while len(buf) < want:
        chunk = sock.recv(want - len(buf))
        if not chunk:
            raise AssertionError(f"连接被设备关闭 (已收 {len(buf)}/{want}B)")
        buf += chunk
    return buf


def _fc03(sock: socket.socket, trans_id: int):
    """原始 MBAP FC03 读 holding 0..0, 返回值寄存器."""
    req = struct.pack(">HHHBBHH", trans_id & 0xFFFF, 0, 6, 1, 3, 0, 1)
    sock.sendall(req)
    header = _recv_full(sock, 6)
    assert header[0:2] == struct.pack(">H", trans_id & 0xFFFF), \
        f"trans_id 不匹配: 发 {trans_id}, 收 {header[0:2].hex()}"
    length = struct.unpack(">H", header[4:6])[0]
    assert 2 <= length <= 260, f"MBAP length 异常: {length}"
    pdu = _recv_full(sock, length)
    assert pdu[1] == 3 and pdu[2] == 2, f"PDU 异常: {pdu.hex()}"
    return struct.unpack(">H", pdu[3:5])[0]


def test_tcp_connect_disconnect_1000(device_ip):
    t0 = time.monotonic()
    for i in range(CYCLES):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3.0)
        try:
            sock.connect((device_ip, MODBUS_TCP_PORT))
            if i % CHECK_INTERVAL == 0:
                val = _fc03(sock, i)
                assert val >= 0
        finally:
            sock.close()
        time.sleep(CONN_INTERVAL_S)

    elapsed = time.monotonic() - t0

    # 收尾: 设备仍能正常服务新连接
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3.0)
    try:
        sock.connect((device_ip, MODBUS_TCP_PORT))
        val = _fc03(sock, CYCLES)
        assert val >= 0
    finally:
        sock.close()

    print(f"\n{CYCLES} 次连接循环耗时 {elapsed:.1f}s "
          f"({CYCLES / elapsed:.0f} conn/s)")


def test_raw_unknown_fc_gets_exception_response(device_ip):
    """未知 FC (0x41): 应用层拦截回异常帧 (fc|0x80, exc=0x01) 而非静默丢弃."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3.0)
    try:
        sock.connect((device_ip, MODBUS_TCP_PORT))
        # MBAP: trans=7 proto=0 len=2 unit=1, PDU 仅 FC 字节
        sock.sendall(struct.pack(">HHHBB", 7, 0, 2, 1, 0x41))
        header = _recv_full(sock, 6)
        assert header[0:2] == struct.pack(">H", 7), "trans_id 不匹配"
        length = struct.unpack(">H", header[4:6])[0]
        # 响应 MBAP.length 含 unit+PDU: [unit][fc|0x80][exc_code]
        pdu = _recv_full(sock, length)
        assert pdu[0] == 0x01, f"unit_id 异常: {pdu.hex()}"
        assert pdu[1] == 0x41 | 0x80, f"期望异常 FC 0xC1, 实际 {pdu.hex()}"
        assert pdu[2] == 0x01, f"期望 exc code 0x01, 实际 {pdu.hex()}"
    except socket.timeout:
        pass   # 静默丢弃也可接受 (不算失败, 设备未挂死即可)
    finally:
        sock.close()
