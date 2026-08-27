"""Holding 写副作用: CONFIG_SAVE 自动清零 + 持久化 / HISTORY 开关联动 /
REBOOT 触发重启. 时间戳设置走 test_timestamp_live (SET_TIME 路径).
"""
import time

import pytest

import config
from config import HOLDING


@pytest.mark.write
def test_fc16_write_multiple_holding(modbus, restore_holding):
    """FC16 写相邻两个采样间隔."""
    modbus.write_holdings(HOLDING["DI_SAMPLE_MS"], [150, 250])
    regs = modbus.read_holding(HOLDING["DI_SAMPLE_MS"], 2)
    assert regs == [150, 250], f"FC16 写后读回 {regs}"


@pytest.mark.write
def test_config_save_triggers_persist_and_auto_clears(modbus, restore_holding):
    """写 0x0E=1 → settings_save() → 寄存器自动清零; 持久化生效可用一次
    重启回读验证 (与 test_config_save_persists_after_reboot 配合)."""
    modbus.write_holding(HOLDING["CONFIG_SAVE"], 1)
    deadline = time.monotonic() + 2.0
    val = None
    while time.monotonic() < deadline:
        val = modbus.read_holding(HOLDING["CONFIG_SAVE"], 1)[0]
        if val == 0:
            break
        time.sleep(0.05)
    assert val == 0, f"CONFIG_SAVE 应在写后自动恢复 0, 实际 0x{val:04X}"


@pytest.mark.write
def test_config_save_persists_after_reboot(device_ip, modbus):
    """改采样间隔 → CONFIG_SAVE → 重启 → 值保留 (验证 FCB 持久化链路).

    全程通过 Modbus TCP 判定设备离线/上线, 结束后还原默认并保存.
    """
    from common.wait_helpers import reboot_and_wait

    modbus.write_holding(HOLDING["DI_SAMPLE_MS"], 137)
    modbus.write_holding(HOLDING["CONFIG_SAVE"], 1)
    time.sleep(0.5)

    assert reboot_and_wait(modbus, device_ip, timeout_s=40), "设备未在 40s 内恢复"
    # 重启后重建的 modbus fixture 连接可能失效, 用新短连接读取
    from common.modbus_client import MbClient

    cli = MbClient(ip=device_ip)
    try:
        assert cli.connect(), "重连失败"
        assert cli.read_holding(HOLDING["DI_SAMPLE_MS"], 1)[0] == 137
    finally:
        cli.close()

    # 还原默认并持久化
    cli2 = MbClient(ip=device_ip)
    try:
        cli2.connect()
        cli2.write_holding(HOLDING["DI_SAMPLE_MS"], 100)
        cli2.write_holding(HOLDING["CONFIG_SAVE"], 1)
    finally:
        cli2.close()


@pytest.mark.write
def test_history_enable_toggle_and_web_reflects(modbus, restore_holding):
    """HISTORY_ENABLE 开关: holding 回读 + Web /api/info 的 hist_en 字段联动."""
    import requests

    r = requests.get(f"http://{config.DEVICE_IP}/api/info", timeout=4)
    base_state = r.json()["hist_en"]

    modbus.write_holding(HOLDING["HISTORY_ENABLE"], 0 if base_state else 1)
    time.sleep(0.3)
    r = requests.get(f"http://{config.DEVICE_IP}/api/info", timeout=4)
    assert r.json()["hist_en"] == (not base_state)
    assert modbus.read_holding(HOLDING["HISTORY_ENABLE"], 1)[0] == \
        int(not base_state)

    # 还原到初始状态 (避免污染后续历史相关测试)
    modbus.write_holding(HOLDING["HISTORY_ENABLE"], int(base_state))


@pytest.mark.write
def test_reboot_register_triggers_device_reset(device_ip, modbus):
    """写 0x0F=1 → housekeeping 延迟冷重启 (约 3-8s 离线) → 自动恢复."""
    from common.wait_helpers import wait_device_online

    modbus.write_holding(HOLDING["REBOOT"], 1)
    time.sleep(3)
    assert wait_device_online(device_ip, timeout_s=40), "REBOOT 后设备未在 40s 内恢复"

    from common.modbus_client import MbClient

    cli = MbClient(ip=device_ip)
    try:
        assert cli.connect()
        assert cli.read_holding(HOLDING["REBOOT"], 1)[0] == 0, \
            "重启后 REBOOT 触发位应已清零"
    finally:
        cli.close()


@pytest.mark.write
def test_timestamp_write_via_holding_side_effect(modbus):
    """向 0x0D (TS_LO) 写值触发高低位组合 set_timestamp:
    组合出当前时间 → 成功; 高位清零的组合落在 1970 年 → 拒绝且时间不受影响.
    """
    now = int(time.time())

    def read_ts():
        hi = modbus.read_holding(HOLDING["TIMESTAMP_HI"], 1)[0]
        lo = modbus.read_holding(HOLDING["TIMESTAMP_LO"], 1)[0]
        return ((hi << 16) | lo) & 0xFFFFFFFF

    # 基线对时 (UDP); 注意 stored 的高位寄存器与实时读不同步, 先写已知高位,
    # 再写低位触发组合 set_timestamp(now)
    from common.udp_client import UdpClient

    with UdpClient(config.DEVICE_IP) as u:
        assert u.set_time(now)

    modbus.write_holding(HOLDING["TIMESTAMP_HI"], (now >> 16) & 0xFFFF)
    modbus.write_holding(HOLDING["TIMESTAMP_LO"], now & 0xFFFF)

    time.sleep(1.1)
    ts = read_ts()
    assert abs(ts - int(time.time())) <= 4, \
        f"holding 设时后 ts={ts}, 与上位机差 {ts - int(time.time())}s"

    # 越界组合 (高位=0 → 1970): 设备拒绝, 系统时间保持实时推进
    modbus.write_holding(HOLDING["TIMESTAMP_HI"], 0)
    modbus.write_holding(HOLDING["TIMESTAMP_LO"], 12345)
    time.sleep(1.2)
    ts_after = read_ts()      # 读回调现场返回实时时间, 拒绝写即不会被 1970 覆盖
    assert abs(ts_after - int(time.time())) <= 4, \
        f"非法组合被拒后系统时间应保持实时, 实际 {ts_after}"

    # 清理: 用 UDP 把时间重设回正确值
    with UdpClient(config.DEVICE_IP) as u:
        u.set_time(int(time.time()))
