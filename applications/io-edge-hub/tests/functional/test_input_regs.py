"""Input 寄存器读测试 (只读).

验证 6 个 input 寄存器 (0x00-0x05):
- 0x00 固件版本 (major<<8 | minor)
- 0x01-0x04 AI1-AI4 工程量 (电流 0.01mA / 电压 0.01V)
- 0x05 DI1-16 bitmap
"""
import pytest

from config import INPUT_COUNT


def test_read_all_input_6(modbus):
    """FC04 一次读 6 个 input."""
    regs = modbus.read_input(0, INPUT_COUNT)
    assert len(regs) == INPUT_COUNT


def test_version_field(modbus):
    """0x00 版本字段: major 在高字节, minor 在低字节."""
    ver = modbus.read_input(0, 1)[0]
    major = (ver >> 8) & 0xFF
    minor = ver & 0xFF
    # 应用版本 v0.1.x (主版本可能在 0-99 范围)
    assert 0 <= major <= 99, f"主版本 {major} 异常"
    assert 0 <= minor <= 99, f"次版本 {minor} 异常"


def test_ai_channels(modbus):
    """0x01-0x04 AI 通道. 4-20mA 输入悬空时可能饱和到 ~2000 (20mA), 不强制断言范围."""
    regs = modbus.read_input(1, 4)
    assert len(regs) == 4
    for v in regs:
        assert 0 <= v <= 0xFFFF


def test_di_bitmap(modbus):
    """0x05 DI1-16 bitmap, 16-bit."""
    di = modbus.read_input(5, 1)[0]
    assert 0 <= di <= 0xFFFF


def test_input_out_of_range(modbus):
    """读地址 >= 6 应失败."""
    from common.modbus_client import ModbusError
    with pytest.raises(ModbusError):
        modbus.read_input(INPUT_COUNT, 1)
