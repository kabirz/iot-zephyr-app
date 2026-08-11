"""UDP 设备发现 (GET_IP 0x11, broadcast-allowed).

依赖: 上位机与设备同子网 (跨子网回复走 8601 端口).
"""
import re

from common.udp_client import discover


def test_discover_finds_target(device_ip):
    """广播发现应至少找到目标设备 IP."""
    found = discover(timeout_ms=2000)
    assert device_ip in found, (
        f"广播发现未找到目标 {device_ip}. 发现列表: {found}"
    )


def test_discover_returns_valid_ips():
    """发现的所有回复应是合法 IPv4 字符串."""
    found = discover(timeout_ms=2000)
    ip_re = re.compile(r"^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$")
    for ip in found:
        assert ip_re.match(ip), f"非法 IP 格式: {ip}"
        parts = [int(x) for x in ip.split(".")]
        for p in parts:
            assert 0 <= p <= 255


def test_discover_deduplicates():
    """多次回复应去重 (同一 IP 只出现一次)."""
    found = discover(timeout_ms=2000)
    assert len(found) == len(set(found)), "发现列表存在重复 IP"
