"""Modbus RTU (RS485) 功能测试.

设备默认 9600bps 8N1, slave_id=1 (holding 0x09). 修改后需重启设备生效.

前置: USB-RS485 适配器接到设备 RS485 端子 (A/B), Linux 下暴露为 /dev/ttyUSB0.
跳过条件: 串口不可用时 fixture 自动 skip.

测试范围:
- FC03 读 holding / FC04 读 input (基本通信)
- RTU 与 TCP 数据一致性 (同一 holding)
- FC06 写 holding + RTU 回读
- 异常地址响应 (Modbus 异常码)
"""
import pytest

from config import HOLDING_COUNT

pytestmark = pytest.mark.rtu


def test_rtu_read_all_holding(rtu, rtu_slave_id):
    """FC03 RTU 读 18 个 holding."""
    regs = rtu.read_holding(0, HOLDING_COUNT, slave=rtu_slave_id)
    assert len(regs) == HOLDING_COUNT


def test_rtu_read_input(rtu, rtu_slave_id):
    """FC04 RTU 读 6 个 input."""
    regs = rtu.read_input(0, 6, slave=rtu_slave_id)
    assert len(regs) == 6
    # 版本字段 (input 0x00): MAJOR<<12 | MINOR<<8 | PATCH
    ver = regs[0]
    major = (ver >> 12) & 0xF
    assert 0 <= major <= 15


def test_rtu_matches_tcp(rtu, rtu_slave_id, modbus):
    """RTU 与 TCP 读同一份 holding, 数据应完全一致 (同一个 holding_reg[])."""
    rtu_regs = rtu.read_holding(0, HOLDING_COUNT, slave=rtu_slave_id)
    tcp_regs = modbus.read_holding(0, HOLDING_COUNT)

    # TIMESTAMP_HI/LO (0x0E/0x0F) 实时返回 time(NULL), 两次读跨秒可能不同, 跳过
    for addr in (0x0E, 0x0F):
        rtu_regs[addr] = tcp_regs[addr] = -1  # 强制相等

    assert rtu_regs == tcp_regs, (
        f"RTU 与 TCP holding 不一致\n"
        f"  RTU: {rtu_regs}\n  TCP: {tcp_regs}"
    )


@pytest.mark.write
def test_rtu_write_holding_readback(rtu, rtu_slave_id, restore_holding):
    """FC06 RTU 写 DI_SAMPLE_MS, FC03 RTU 读回验证."""
    from config import HOLDING
    original = rtu.read_holding(HOLDING["DI_SAMPLE_MS"], 1, slave=rtu_slave_id)[0]
    new_val = (original + 7) & 0xFFFF  # +7 让变化明显
    rtu.write_holding(HOLDING["DI_SAMPLE_MS"], new_val, slave=rtu_slave_id)
    readback = rtu.read_holding(HOLDING["DI_SAMPLE_MS"], 1, slave=rtu_slave_id)[0]
    assert readback == new_val, f"RTU 写 0x{new_val:04X}, 读回 0x{readback:04X}"


@pytest.mark.write
def test_rtu_write_do(rtu, rtu_slave_id, restore_holding):
    """FC05 RTU 写 DO1, 验证 holding 0x00 bit0 同步."""
    rtu.write_coil(0, True, slave=rtu_slave_id)
    val = rtu.read_holding(0, 1, slave=rtu_slave_id)[0]
    assert val & 0x01, f"RTU 写 DO1=ON 后 holding 0x00=0x{val:04X}, bit0 应为 1"


def test_rtu_wrong_slave_id(rtu):
    """用错误的 slave_id (例如 250) 读, 应超时无响应."""
    from common.modbus_client import ModbusError
    with pytest.raises(ModbusError):
        rtu.read_holding(0, 1, slave=250)


def test_rtu_out_of_range_addr(rtu, rtu_slave_id):
    """读地址 >= 18 应返回 Modbus 异常 (异常码 02 非法地址)."""
    from common.modbus_client import ModbusError
    with pytest.raises(ModbusError):
        rtu.read_holding(HOLDING_COUNT, 1, slave=rtu_slave_id)
