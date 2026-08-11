"""Holding 寄存器读测试 (只读).

验证 18 个 holding 寄存器 (0x00-0x11) 都可读, 且默认值符合预期.
"""
import pytest

from config import HOLDING_COUNT, HOLDING

DEFAULTS = {
    0x00: 0,       # DO
    0x01: 0xFFFF,  # DI_ENABLE
    0x02: 0x000F,  # AI_ENABLE
    0x03: 200,     # DI_SAMPLE_MS
    0x04: 200,     # AI_SAMPLE_MS
    0x05: 0,       # HISTORY_ENABLE
    0x06: 0x0111,  # CAN_ID
    0x07: 10,      # CAN_BAUDRATE
    0x08: 9600,    # RS485_BAUDRATE
    0x09: 1,       # SLAVE_ID
    0x0A: 192,     # IP_OCTET1
    0x0B: 168,
    0x0C: 12,
    0x0D: 101,
    0x10: 0,       # CONFIG_SAVE
    0x11: 0,       # REBOOT
    # 0x0E/0x0F (TIMESTAMP_HI/LO) 不在默认值表: 读时返回实时 time(NULL)
}


def test_read_all_holding_18(modbus):
    """FC03 一次读 18 个 holding, 应全部成功."""
    regs = modbus.read_holding(0, HOLDING_COUNT)
    assert len(regs) == HOLDING_COUNT


def test_default_values(modbus):
    """读 18 个 holding, 验证默认值 (TIMESTAMP_HI/LO 除外)."""
    regs = modbus.read_holding(0, HOLDING_COUNT)
    for addr, expected in DEFAULTS.items():
        assert regs[addr] == expected, (
            f"holding 0x{addr:02X} 默认值期望 0x{expected:04X}, 实际 0x{regs[addr]:04X}"
        )


def test_default_ip(modbus):
    """0x0A-0x0D 应为 192.168.12.101 (低字节)."""
    regs = modbus.read_holding(HOLDING["IP_OCTET1"], 4)
    ip = ".".join(str(r & 0xFF) for r in regs)
    assert ip == "192.168.12.101", f"默认 IP 期望 192.168.12.101, 实际 {ip}"


def test_read_each_single(modbus):
    """FC03 单独读每个 holding 地址."""
    for addr in range(HOLDING_COUNT):
        if addr in (0x0E, 0x0F):
            continue  # TIMESTAMP 跳过, 单测覆盖
        regs = modbus.read_holding(addr, 1)
        assert len(regs) == 1


def test_read_out_of_range(modbus):
    """读地址 >= 18 应返回异常 (Modbus 异常码 02 非法地址)."""
    from common.modbus_client import ModbusError
    with pytest.raises(ModbusError):
        modbus.read_holding(HOLDING_COUNT, 1)
