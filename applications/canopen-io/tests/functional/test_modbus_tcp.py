"""Modbus TCP: 寄存器布局 / DO 双向控制 / 时间戳实时性 / 越界拒绝."""
import time

import pytest

import config
from common.modbus_client import modbus_tcp_client, ModbusError

pytestmark = pytest.mark.skipif(not config.DEVICE_IP, reason="no device ip")


def test_read_full_holding_layout():
    """FC03 读 16 个 holding 全量可读 (0x00-0x0F)."""
    with modbus_tcp_client() as mb:
        regs = mb.read_holding(0x00, config.HOLDING_COUNT)
        assert len(regs) == config.HOLDING_COUNT
        # 出厂默认抽查 (设备可能已改过参数, 只验证宽度与合理值)
        assert all(isinstance(v, int) and 0 <= v <= 0xFFFF for v in regs)


def test_input_registers():
    """FC04: 版本 / AI×4 / DI 位图, 数量与取值范围."""
    with modbus_tcp_client() as mb:
        regs = mb.read_input(0x00, config.INPUT_COUNT)
        assert len(regs) == config.INPUT_COUNT
        ver = regs[config.INPUT["VER"]]
        assert 0 < ver <= 0xFFFF


def test_do_word_write_and_coils_mirror():
    """FC06 写 DO 字 → FC01 读 coils 位镜像 → 恢复."""
    with modbus_tcp_client() as mb:
        old = mb.read_holding(config.HOLDING["DO"])[0]
        try:
            mb.write_holding(config.HOLDING["DO"], 0x00A5)  # DO1,3,6,8 on
            bits = mb.read_coils(0x00, 8)
            assert bits == [(0xA5 >> i) & 1 == 1 for i in range(8)]
            assert mb.read_holding(config.HOLDING["DO"])[0] == 0x00A5
        finally:
            mb.write_holding(config.HOLDING["DO"], old)


def test_fc05_single_bit_writes():
    """FC05 单 bit 写与读-改-写语义; 其余位保持."""
    with modbus_tcp_client() as mb:
        old = mb.read_holding(config.HOLDING["DO"])[0]
        try:
            mb.write_coil(2, True)   # DO3
            assert mb.read_holding(config.HOLDING["DO"])[0] & 0b100
            assert mb.read_coils(2, 1) == [True]
            mb.write_coil(2, False)
            assert not (mb.read_holding(config.HOLDING["DO"])[0] & 0b100)
            # FC15 多位写
            mb.write_coils(0, [True] * 8)
            assert mb.read_holding(config.HOLDING["DO"])[0] == 0xFF
        finally:
            mb.write_holding(config.HOLDING["DO"], old)


def test_timestamp_registers_live():
    """时间戳寄存器 0x0C/0x0D 读返回实时系统时间, 两次读取随时间推进."""
    def now_ts(mb):
        hi, lo = mb.read_holding(config.HOLDING["TIMESTAMP_HI"], 2)
        return ((hi << 16) | lo) & 0xFFFFFFFF

    with modbus_tcp_client() as mb:
        t1 = now_ts(mb)
        time.sleep(1.2)
        t2 = now_ts(mb)

    assert t1 > 946684800  # >= 2000-01-01
    assert t1 <= t2 <= t1 + 5  # 推进且未跳变


def test_out_of_range_register_rejected():
    """越界地址 (>=16 个 holding) 返回异常响应而非静默成功."""
    with modbus_tcp_client() as mb:
        with pytest.raises(ModbusError):
            mb.read_holding(config.HOLDING_COUNT, 1)


def test_di_via_discrete_inputs_and_input_reg():
    """FC02 离散输入位图 与 input reg[DI] 位图一致 (同源数据)."""
    with modbus_tcp_client() as mb:
        di_bits = mb.read_discrete_inputs(0, 16)
        di_word = sum((1 if b else 0) << i for i, b in enumerate(di_bits))
        reg = mb.read_input(config.INPUT["DI"])[0]
        assert di_word == reg
