"""FC05/FC15 写 DO (holding 0x00 的 coil 视图, 与 FC06/SDO/RPDO 同一执行层)."""
import pytest

from config import HOLDING


@pytest.mark.write
def test_fc05_write_single_do(modbus, restore_holding):
    modbus.write_coil(0, True)
    val = modbus.read_holding(HOLDING["DO"], 1)[0]
    assert val & 0x01, f"DO1 ON 后 holding=0x{val:04X}, bit0 应为 1"
    modbus.write_coil(0, False)
    val = modbus.read_holding(HOLDING["DO"], 1)[0]
    assert not (val & 0x01), f"DO1 OFF 后 holding=0x{val:04X}, bit0 应为 0"


@pytest.mark.write
def test_fc05_write_all_8_do_sequentially(modbus, restore_holding):
    """逐个点亮 DO1-DO8 再逐一熄灭 (LED 联动同路径)."""
    for i in range(8):
        modbus.write_coil(i, True)
        val = modbus.read_holding(HOLDING["DO"], 1)[0]
        assert val & (1 << i), f"DO{i+1} ON 后 bit{i} 应为 1, holding=0x{val:04X}"
    for i in range(8):
        modbus.write_coil(i, False)
        val = modbus.read_holding(HOLDING["DO"], 1)[0]
        assert not (val & (1 << i)), f"DO{i+1} OFF 后 bit{i} 应为 0, holding=0x{val:04X}"


@pytest.mark.write
def test_fc05_preserves_other_bits(modbus, restore_holding):
    """FC05 单 bit 写是加锁读-改-写: 其余位保持不变."""
    modbus.write_holdings(HOLDING["DO"], [0b00000011])
    modbus.write_coil(6, True)   # 只动 DO7
    val = modbus.read_holding(HOLDING["DO"], 1)[0]
    assert val == 0b01000011, f"期望 0x43, 实际 0x{val:04X}"


@pytest.mark.write
def test_fc15_write_multi_do_pattern(modbus, restore_holding):
    pattern = [True, False] * 4
    modbus.write_coils(0, pattern)
    val = modbus.read_holding(HOLDING["DO"], 1)[0]
    assert val == 0x55, f"FC15 写 0101.. 后 holding=0x{val:04X}, 期望 0x0055"


@pytest.mark.write
def test_fc01_read_coils_mirrors_holding(modbus, restore_holding):
    modbus.write_holdings(HOLDING["DO"], [0xA5])
    bits = modbus.read_coils(0, 8)
    expected = [(0xA5 >> i) & 1 == 1 for i in range(8)]
    assert bits == expected, f"FC01 读 coils = {bits}"


def test_fc02_read_discrete_inputs(modbus):
    """FC02 读 16 路 DI (外部电平决定值, 仅验证可读且数量正确)."""
    bits = modbus.read_discrete_inputs(0, 16)
    assert len(bits) == 16
    for b in bits:
        assert b in (True, False)


@pytest.mark.write
def test_out_of_range_coil_rejected(modbus, restore_holding):
    from common.modbus_client import ModbusError
    import config as cfg

    with pytest.raises(ModbusError):
        modbus.write_coil(cfg.DO_NUM if hasattr(cfg, "DO_NUM") else 8, True)
