"""CAN 固件升级协议 - VERSION 查询测试.

发 0x101 cmd=VERSION → 收 0x102 + N 帧 0x105 拼接版本字符串.

前置: SocketCAN 接口已配置 + 与设备同波特率 (250kbps).
"""
import re

import pytest

from common.can_client import CanClient, CanError

pytestmark = pytest.mark.can


def test_can_version_query(can):
    """CAN VERSION 查询应返回非空字符串."""
    ver = can.query_version(timeout=3.0)
    assert ver, "CAN VERSION 查询返回空"
    # 期望格式 vX.Y.Z_6hex (与 UDP GET_VERSION 一致)
    assert re.match(r"^v?\d+\.\d+\.\d+", ver), f"CAN VERSION 格式异常: {ver!r}"


def test_can_version_matches_udp(can, udp):
    """CAN VERSION 与 UDP GET_VERSION 应一致 (固件使用同一 fw_gitver.h)."""
    import socket
    # UDP GET_VERSION 用 0x04 命令
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2.0)
    try:
        sock.sendto(bytes([0x04]), (udp.ip, udp.port))
        data, _ = sock.recvfrom(256)
        udp_ver = data[1:].decode("ascii", errors="replace")
    finally:
        sock.close()

    can_ver = can.query_version(timeout=3.0)
    # 完全相同 (两通道读同一 fw_gitver.h 字符串)
    assert can_ver == udp_ver or can_ver.rstrip("\0") == udp_ver.rstrip("\0"), (
        f"CAN({can_ver!r}) != UDP({udp_ver!r})"
    )


def test_can_version_repeatable(can):
    """连续 5 次 VERSION 查询应都成功且结果一致."""
    versions = []
    for _ in range(5):
        versions.append(can.query_version(timeout=3.0))
    assert all(v == versions[0] for v in versions), f"VERSION 不稳定: {versions}"
