"""Modbus TCP 连接/断开循环压力: 1000 次 connect-close, 设备应全程稳定.

关注点 (配合串口 log 检查):
  - 每次 connect 后设备端 accept + 分配槽位, close 后 FIN 回收
  - 周期性 FC03 读确认服务持续响应 (不只是 accept)
  - 结束后再读一次, 排除"耗尽 net_context / 槽位泄漏后假活"
  - 串口不应出现 HardFault / 断言 / 看门狗重启 / "wait timeout"
"""
import socket
import struct
import time

import pytest

from config import MODBUS_TCP_PORT

CYCLES = 1000
CHECK_INTERVAL = 100  # 每 N 次做一次 FC03 读校验
# 连接间隔: 设备协议栈在连接背靠背 (<3ms) 到达时偶发丢 SYN
# (客户端 1s 重传, 拖慢整体 ~25%). 5ms 间隔可完全避开, 仍有 200 conn/s
CONN_INTERVAL_S = 0.005


def _recv_full(sock: socket.socket, want: int) -> bytes:
    """循环读满 want 字节 (固件分两次 send 头/数据, 单次 recv 可能短读)."""
    buf = b""
    while len(buf) < want:
        chunk = sock.recv(want - len(buf))
        if not chunk:
            raise AssertionError(f"连接被设备关闭 (已收 {len(buf)}/{want}B)")
        buf += chunk
    return buf


def _fc03(sock: socket.socket, trans_id: int):
    """原始 MBAP FC03 读 holding 0..0, 返回响应 bytes (异常时抛 AssertionError)."""
    req = struct.pack(">HHHBBHH", trans_id & 0xFFFF, 0, 6, 1, 3, 0, 1)
    sock.sendall(req)
    header = _recv_full(sock, 6)
    assert header[0:2] == struct.pack(">H", trans_id & 0xFFFF), \
        f"trans_id 不匹配: 发 {trans_id}, 收 {header[0:2].hex()}"
    length = struct.unpack(">H", header[4:6])[0]
    assert 2 <= length <= 260, f"MBAP length 异常: {length}"
    pdu = _recv_full(sock, length)
    assert pdu[1] == 3, f"FC 异常 (应为 3): {pdu[1]}"


def test_tcp_connect_disconnect_1000(device_ip):
    """1000 次连接-断开: 全部成功连接, 周期读正常, 结束后设备仍可服务."""
    t0 = time.monotonic()
    for i in range(CYCLES):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3.0)
        try:
            sock.connect((device_ip, MODBUS_TCP_PORT))
            if i % CHECK_INTERVAL == 0:
                _fc03(sock, i)
        except OSError as e:
            pytest.fail(f"cycle {i + 1}/{CYCLES} 失败: {e}")
        finally:
            sock.close()
        time.sleep(CONN_INTERVAL_S)
    dt = time.monotonic() - t0

    # 结束后再独立连接读一次, 确认没有资源泄漏导致的假活
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3.0)
    try:
        sock.connect((device_ip, MODBUS_TCP_PORT))
        _fc03(sock, CYCLES)
    finally:
        sock.close()

    rate = CYCLES / dt if dt > 0 else 0
    print(f"\n{CYCLES} 次 connect/close 完成: {dt:.1f}s ({rate:.0f} conn/s)")
