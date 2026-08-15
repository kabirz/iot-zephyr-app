"""FC05/FC15 写 DO (0x00 holding 的 coil 视图 + LED 联动).

Modbus coil 地址空间: 0x0000-0x0007 对应 DO1-DO8.
注意: 设备把 DO 映射到 holding 0x00 的低 8 位, coil 写等价于改 holding 0x00.
"""
import pytest


@pytest.mark.write
def test_fc05_write_single_do(modbus, restore_holding):
    """FC05 写 DO1 (coil 0) = ON, 读 holding 0x00 验证 bit0=1."""
    modbus.write_coil(0, True)
    val = modbus.read_holding(0, 1)[0]
    assert val & 0x01, f"DO1 写 ON 后 holding 0x00=0x{val:04X}, bit0 应为 1"


@pytest.mark.write
def test_fc05_write_all_8_do(modbus, restore_holding):
    """FC05 逐个写 DO1-DO8, 读 holding 0x00 验证对应 bit."""
    for i in range(8):
        modbus.write_coil(i, True)
        val = modbus.read_holding(0, 1)[0]
        assert val & (1 << i), f"DO{i+1} 写 ON 后 bit{i} 应为 1, holding=0x{val:04X}"
        modbus.write_coil(i, False)
        val = modbus.read_holding(0, 1)[0]
        assert not (val & (1 << i)), f"DO{i+1} 写 OFF 后 bit{i} 应为 0, holding=0x{val:04X}"


@pytest.mark.write
def test_fc15_write_multi_do(modbus, restore_holding):
    """FC15 一次写 8 个 DO: coil0,2,4,6=ON → 0x55."""
    pattern = [True, False, True, False, True, False, True, False]
    modbus.write_coils(0, pattern)
    val = modbus.read_holding(0, 1)[0]
    assert val == 0x55, f"FC15 写 0101.. 后 holding=0x{val:04X}, 期望 0x0055"


@pytest.mark.write
def test_fc01_read_coils(modbus, restore_holding):
    """FC01 读 8 个 coil, 与 holding 0x00 低 8 位一致."""
    modbus.write_holdings(0, [0x55])  # 01010101
    bits = modbus.read_coils(0, 8)
    expected = [bool(b) for b in [1, 0, 1, 0, 1, 0, 1, 0]]
    assert bits == expected, f"FC01 读 coils = {bits}, 期望 {expected}"


@pytest.mark.write
def test_fc02_read_discrete_inputs(modbus):
    """FC02 读 16 个 DI (discrete input). DI 由外部硬件电平决定, 仅验证可读."""
    bits = modbus.read_discrete_inputs(0, 16)
    assert len(bits) == 16
    for b in bits:
        assert b in (True, False)
