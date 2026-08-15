"""Holding 写副作用: CONFIG_SAVE / REBOOT / HISTORY_ENABLE / SET_TIME 持久化."""
import time

import pytest

from config import HOLDING


@pytest.mark.write
def test_fc06_write_each_holding(modbus, restore_holding):
    """FC06 逐个写 holding (避开 DO/CONFIG_SAVE/REBOOT 这种有副作用的)."""
    safe_addrs = [
        HOLDING["DI_ENABLE"],
        HOLDING["AI_ENABLE"],
        HOLDING["DI_SAMPLE_MS"],
        HOLDING["AI_SAMPLE_MS"],
        HOLDING["CAN_ID"],
        HOLDING["CAN_BAUDRATE"],
    ]
    for addr in safe_addrs:
        original = modbus.read_holding(addr, 1)[0]
        new_val = (original + 1) & 0xFFFF
        modbus.write_holding(addr, new_val)
        readback = modbus.read_holding(addr, 1)[0]
        assert readback == new_val, (
            f"FC06 写 0x{addr:02X}=0x{new_val:04X}, 读回 0x{readback:04X}"
        )


@pytest.mark.write
def test_fc16_write_multiple_holding(modbus, restore_holding):
    """FC16 写多个 holding."""
    modbus.write_holdings(HOLDING["DI_SAMPLE_MS"], [150, 250])
    regs = modbus.read_holding(HOLDING["DI_SAMPLE_MS"], 2)
    assert regs == [150, 250], f"FC16 写后读回 {regs}"


@pytest.mark.write
def test_config_save_triggers_persist(modbus, restore_holding):
    """写 0x10 (CONFIG_SAVE) 非零值应触发 settings_save(). 设备应自动清零该寄存器."""
    modbus.write_holding(HOLDING["CONFIG_SAVE"], 1)
    time.sleep(0.5)  # 给 settings_save() 完成
    val = modbus.read_holding(HOLDING["CONFIG_SAVE"], 1)[0]
    assert val == 0, f"CONFIG_SAVE 写后应自动恢复 0, 实际 0x{val:04X}"


@pytest.mark.write
def test_history_enable_toggle(modbus, restore_holding):
    """写 0x05 (HISTORY_ENABLE) = 1 应开启历史, =0 关闭."""
    modbus.write_holding(HOLDING["HISTORY_ENABLE"], 1)
    assert modbus.read_holding(HOLDING["HISTORY_ENABLE"], 1)[0] == 1
    modbus.write_holding(HOLDING["HISTORY_ENABLE"], 0)
    assert modbus.read_holding(HOLDING["HISTORY_ENABLE"], 1)[0] == 0


@pytest.mark.write
def test_reboot_register_writes_then_device_resets(modbus, device_ip):
    """写 0x11 (REBOOT) = 1, 设备应重启. (此测试会让设备失联 ~5s)"""
    import socket
    from common.wait_helpers import wait_device_online

    try:
        modbus.write_holding(HOLDING["REBOOT"], 1)
    except Exception:
        pass  # 写后连接可能立即断
    time.sleep(1)
    assert wait_device_online(device_ip, timeout_s=30), "设备重启后未上线"


@pytest.mark.write
def test_settings_persist_across_reboot(modbus, udp, device_ip):
    """写 holding 0x03 (DI_SAMPLE_MS) = 333 → settings_save → 重启 → 读应仍是 333."""
    from common.wait_helpers import wait_device_online

    modbus.write_holding(HOLDING["DI_SAMPLE_MS"], 333)
    modbus.write_holding(HOLDING["CONFIG_SAVE"], 1)
    time.sleep(0.5)

    try:
        modbus.write_holding(HOLDING["REBOOT"], 1)
    except Exception:
        pass
    time.sleep(1)
    assert wait_device_online(device_ip, timeout_s=30), "设备重启后未上线"

    val = modbus.read_holding(HOLDING["DI_SAMPLE_MS"], 1)[0]
    assert val == 333, f"持久化参数重启后丢失: DI_SAMPLE_MS 期望 333, 实际 {val}"

    # 恢复出厂值并持久化, 避免污染下一轮套件的 test_default_values
    modbus.write_holding(HOLDING["DI_SAMPLE_MS"], 200)
    modbus.write_holding(HOLDING["CONFIG_SAVE"], 1)
    time.sleep(0.5)
