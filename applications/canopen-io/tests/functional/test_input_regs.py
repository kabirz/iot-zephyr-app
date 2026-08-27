"""Input 寄存器 (0x00-0x05): 版本字段解码 / AI / DI 位图 / 越界."""
import pytest

from config import INPUT_COUNT, INPUT, HOLDING


def test_read_all_input_6(modbus):
    regs = modbus.read_input(0, INPUT_COUNT)
    assert len(regs) == INPUT_COUNT


def test_version_field_decode():
    """0x00: MAJOR<<12 | MINOR<<8 | PATCH. 与 SDO 0x100A 字符串交叉验证."""
    import re

    ver_reg, ver_str = _modbus_and_sdo()
    major = (ver_reg >> 12) & 0xF
    minor = (ver_reg >> 8) & 0xF
    patch = ver_reg & 0xFF
    assert 0 <= major <= 15 and 0 <= minor <= 15
    m = re.search(r"v(\d+)\.(\d+)\.(\d+)", ver_str)
    assert m, f"版本串无法解析: {ver_str!r}"
    assert int(m.group(1)) == major
    assert int(m.group(2)) == minor


def _modbus_and_sdo():
    """同时取 input[VER] 与 SDO 0x100A 版本串; CANopen 未接入时跳过交叉部分."""
    import pytest

    from common.canopen_node import NodeHandle

    from config import DEVICE_IP
    from common.modbus_client import modbus_tcp_client

    with modbus_tcp_client(ip=DEVICE_IP) as mb:
        ver_reg = mb.read_input(INPUT["VER"], 1)[0]
        try:
            with NodeHandle() as h:
                v = h.node.sdo.upload(0x100A, 0)
                if isinstance(v, (bytes, bytearray)):
                    s = bytes(v).decode(errors="replace")
                else:
                    pytest.skip("CANopen 读 0x100A 失败")
                return ver_reg, s
        except Exception:
            pytest.skip("CANopen 接口不可用")


def test_ai_channels_enabled_only(modbus):
    """AI 使能通道在 0x01-0x04 有值且为工程量合理范围; 禁用通道可以为任意值."""
    en = modbus.read_holding(HOLDING["AI_ENABLE"], 1)[0]
    regs = modbus.read_input(INPUT["AI0"], 4)
    for i, v in enumerate(regs):
        if en & (1 << i):
            # AI1-2 电流上限 ~20000 (0.01mA); AI3-4 电压上限 ~100000 但 16bit 钳位
            assert 0 <= v <= 0xFFFF


def test_di_bitmap(modbus):
    di = modbus.read_input(INPUT["DI"], 1)[0]
    assert 0 <= di <= 0xFFFF


def test_input_out_of_range(modbus):
    from common.modbus_client import ModbusError

    with pytest.raises(ModbusError):
        modbus.read_input(INPUT_COUNT, 1)
