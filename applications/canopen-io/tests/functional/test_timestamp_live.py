"""TIMESTAMP_HI/LO (0x0C/0x0D) 读时返回实时系统时间.

FC03 读回调对这两个地址调 time(NULL); SET_TIME 校准后与上位机误差 <2s.
"""
import time

import pytest

from config import HOLDING


def test_timestamp_returns_live_time(modbus, device_ip):
    """先 UDP 对时 (RTC 漂移可能超窗), 再读高低位组合应落在上位机窗口内."""
    from common.udp_client import UdpClient

    with UdpClient(ip=device_ip) as u:
        u.set_time(int(time.time()))
    t_before = int(time.time())
    hi = modbus.read_holding(HOLDING["TIMESTAMP_HI"], 1)[0]
    lo = modbus.read_holding(HOLDING["TIMESTAMP_LO"], 1)[0]
    t_after = int(time.time())

    device_ts = ((hi << 16) | lo) & 0xFFFFFFFF
    assert t_before - 2 <= device_ts <= t_after + 2, (
        f"设备时间戳 {device_ts} 不在上位机窗口 [{t_before}, {t_after}] 内")


def test_timestamp_changes_over_time(modbus):
    """隔 2 秒两次读: 单调非递减且推进 >=1s."""
    def now_ts():
        hi = modbus.read_holding(HOLDING["TIMESTAMP_HI"], 1)[0]
        lo = modbus.read_holding(HOLDING["TIMESTAMP_LO"], 1)[0]
        return ((hi << 16) | lo) & 0xFFFFFFFF

    t1 = now_ts()
    time.sleep(2.0)
    t2 = now_ts()

    assert t2 >= t1, f"2s 后时间戳回退: t1={t1}, t2={t2}"
    assert t2 - t1 >= 1, f"两次读时间戳差 < 1s: t1={t1}, t2={t2}"


@pytest.mark.write
def test_timestamp_via_udp_set_time(modbus, udp):
    target = int(time.time())
    assert udp.set_time(target)

    hi = modbus.read_holding(HOLDING["TIMESTAMP_HI"], 1)[0]
    lo = modbus.read_holding(HOLDING["TIMESTAMP_LO"], 1)[0]
    device_ts = ((hi << 16) | lo) & 0xFFFFFFFF
    assert abs(device_ts - target) <= 2


@pytest.mark.write
def test_web_regs_and_fc03_agree_on_timestamp(modbus):
    """Web /api/regs 与 FC03 读到同一实时值 (两通道共用 io_read_holding)."""
    import requests
    import config

    hi_fc = modbus.read_holding(HOLDING["TIMESTAMP_HI"], 2)
    d = requests.get(f"http://{config.DEVICE_IP}/api/regs", timeout=4).json()
    ts_fc = ((hi_fc[0] << 16) | hi_fc[1]) & 0xFFFFFFFF
    ts_web = ((d["holding"][HOLDING["TIMESTAMP_HI"]] << 16)
              | d["holding"][HOLDING["TIMESTAMP_LO"]]) & 0xFFFFFFFF
    assert abs(int(ts_fc) - int(ts_web)) <= 3
