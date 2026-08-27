"""UDP 广播发现 (GET_IP 0x11, 唯一放行的广播回复命令)."""
import re

from common.udp_client import discover


def test_discover_finds_target(device_ip):
    found = discover(timeout_ms=2000)
    assert device_ip in found, f"广播发现未找到目标 {device_ip}. 发现列表: {found}"


def test_discover_returns_valid_ips():
    found = discover(timeout_ms=2000)
    ip_re = re.compile(r"^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$")
    for ip in found:
        assert ip_re.match(ip), f"非法 IP 格式: {ip}"
        assert all(0 <= int(p) <= 255 for p in ip.split("."))


def test_discover_deduplicates():
    found = discover(timeout_ms=2000)
    assert len(found) == len(set(found))
