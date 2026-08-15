"""TIMESTAMP_HI/LO (0x0E/0x0F) 读时返回实时系统时间 (v3.4 行为).

设备 holding_reg_rd_cb 对这两个地址调用 time(NULL), 拆成高低 16 位返回.
注意: 两次单读之间可能跨秒边界 (1e-6 概率), 测试用容差 2s.
"""
import time

import pytest

from common.modbus_client import ModbusError


def test_timestamp_returns_live_time(modbus, device_ip):
    """读 HI+LO, 组合的 32 位 Unix 时间戳应与上位机当前时间相近 (±2s).
    先 UDP 对时 (LSI RTC 漂移可能超窗)."""
    from common.udp_client import UdpClient
    with UdpClient(ip=device_ip) as u:
        u.set_time(int(time.time()))
    t_before = int(time.time())
    hi = modbus.read_holding(0x0E, 1)[0]
    lo = modbus.read_holding(0x0F, 1)[0]
    t_after = int(time.time())

    device_ts = (hi << 16) | lo

    # 设备时间应在上位机 [t_before, t_after + 1] 窗口内 (允许 1s 漂移)
    assert t_before - 2 <= device_ts <= t_after + 2, (
        f"设备时间戳 {device_ts} 不在上位机窗口 [{t_before}, {t_after}] 内"
    )


def test_timestamp_changes_over_time(modbus):
    """隔 2 秒读两次, 时间戳应单调非递减."""
    hi1 = modbus.read_holding(0x0E, 1)[0]
    lo1 = modbus.read_holding(0x0F, 1)[0]
    t1 = (hi1 << 16) | lo1

    time.sleep(2.0)

    hi2 = modbus.read_holding(0x0E, 1)[0]
    lo2 = modbus.read_holding(0x0F, 1)[0]
    t2 = (hi2 << 16) | lo2

    assert t2 >= t1, f"2s 后时间戳应 >= 第一次: t1={t1}, t2={t2}"
    # 时间间隔应 >= 1s (允许 1s 容差)
    assert (t2 - t1) >= 1, f"两次读时间戳差 < 1s: t1={t1}, t2={t2}, diff={t2-t1}"


@pytest.mark.write
def test_timestamp_via_udp_set_time(modbus, udp):
    """SET_TIME 设备时间 = 上位机时间, 立即读 holding 0x0E/0x0F 应一致."""
    import time as _time
    target = int(_time.time())
    assert udp.set_time(target), "SET_TIME 被设备拒绝"

    # 设备内部 set_timestamp 调 clock_settime; 读时返回 time(NULL) (刚被设)
    hi = modbus.read_holding(0x0E, 1)[0]
    lo = modbus.read_holding(0x0F, 1)[0]
    device_ts = (hi << 16) | lo

    assert abs(device_ts - target) <= 2, (
        f"SET_TIME({target}) 后读设备时间 {device_ts}, 差值 {device_ts-target}s"
    )
