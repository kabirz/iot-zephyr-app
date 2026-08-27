"""Modbus RTU (RS485, 9600 8N1): 寄存器读写 / DO 控制 / 错误从机号 / 与 TCP 同源."""
import pytest

from common.modbus_client import MbClient, ModbusError
import config
from config import HOLDING, INPUT


def test_rtu_read_holding_all(rtu, rtu_slave_id):
    regs = rtu.read_holding(0x00, config.HOLDING_COUNT, slave=rtu_slave_id)
    assert len(regs) == config.HOLDING_COUNT
    assert regs[HOLDING["SLAVE_ID"]] & 0xFF == rtu_slave_id
    assert regs[HOLDING["RS485_BAUDRATE"]] in \
        (1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200)


@pytest.mark.write
def test_rtu_do_write_and_coil_mirror(rtu, rtu_slave_id, restore_holding):
    old = rtu.read_holding(HOLDING["DO"], slave=rtu_slave_id)[0]
    try:
        rtu.write_holding(HOLDING["DO"], 0x0042, slave=rtu_slave_id)
        assert rtu.read_holding(HOLDING["DO"], slave=rtu_slave_id)[0] == 0x0042
        bits = rtu.read_coils(0x00, 8, slave=rtu_slave_id)
        assert bits == [(0x42 >> i) & 1 == 1 for i in range(8)]
        # TCP 通道同步可见 (单一数据源)
    finally:
        rtu.write_holding(HOLDING["DO"], old, slave=rtu_slave_id)


def test_rtu_input_registers(rtu, rtu_slave_id):
    regs = rtu.read_input(0x00, config.INPUT_COUNT, slave=rtu_slave_id)
    assert len(regs) == config.INPUT_COUNT


def test_rtu_tcp_state_consistent(device_ip, rtu, rtu_slave_id):
    """RTU 读与 TCP 读同源一致."""
    with MbClient(ip=device_ip) as tcp:
        a = rtu.read_holding(HOLDING["DI_SAMPLE_MS"], slave=rtu_slave_id)[0]
        b = tcp.read_holding(HOLDING["DI_SAMPLE_MS"])[0]
        assert a == b


def test_rtu_wrong_slave_id_no_response(request):
    """错误从机地址: 应超时无响应 (8 位地址域不匹配)."""
    serial_port = request.config.getoption("--rtu-port") or config.MODBUS_RTU_PORT
    baud = request.config.getoption("--rtu-baud") or config.MODBUS_RTU_BAUDRATE
    bad = 2 if config.MODBUS_RTU_SLAVE_ID != 2 else 3

    cli = MbClient(transport="rtu", serial_port=serial_port, baudrate=baud,
                   timeout=0.6)
    if not cli.connect():
        pytest.skip("无法打开 RTU 串口")
    try:
        with pytest.raises(ModbusError):
            cli.read_holding(0x00, 1, slave=bad)
    finally:
        cli.close()


def test_rtu_out_of_range_rejected(rtu, rtu_slave_id):
    with pytest.raises(ModbusError):
        rtu.read_holding(config.HOLDING_COUNT, 1, slave=rtu_slave_id)
