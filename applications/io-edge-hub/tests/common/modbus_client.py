"""pymodbus 客户端包装: 自动重连 + 友好错误信息 + TCP/RTU 双传输.

pymodbus 3.x 同步 API:
    # TCP
    client = ModbusTcpClient('192.168.12.101', port=502)
    # RTU (串口)
    client = ModbusSerialClient('/dev/ttyUSB0', baudrate=9600, parity='N',
                                 stopbits=1, bytesize=8)
    client.connect()
    rr = client.read_holding_registers(addr=0, count=18, slave=1)
    if rr.isError(): ...
    regs = rr.registers  # list[int]
"""
from contextlib import contextmanager

from pymodbus.client import ModbusTcpClient, ModbusSerialClient

from config import (
    DEVICE_IP, MODBUS_TCP_PORT, MODBUS_TIMEOUT,
    MODBUS_RTU_PORT, MODBUS_RTU_BAUDRATE, MODBUS_RTU_PARITY,
    MODBUS_RTU_STOPBITS, MODBUS_RTU_BYTESIZE, MODBUS_RTU_TIMEOUT,
)


class ModbusError(Exception):
    """Modbus 通信错误."""


class MbClient:
    """Modbus 客户端包装 (pymodbus 3.x, 支持 TCP 与 RTU 两种传输)."""

    def __init__(self,
                 # TCP 参数
                 ip: str = DEVICE_IP, port: int = MODBUS_TCP_PORT,
                 # RTU 参数 (transport='rtu' 时使用)
                 serial_port: str = MODBUS_RTU_PORT,
                 baudrate: int = MODBUS_RTU_BAUDRATE,
                 parity: str = MODBUS_RTU_PARITY,
                 stopbits: int = MODBUS_RTU_STOPBITS,
                 bytesize: int = MODBUS_RTU_BYTESIZE,
                 # 通用
                 transport: str = "tcp",
                 timeout: float = None):
        self.transport = transport
        if timeout is None:
            timeout = MODBUS_RTU_TIMEOUT if transport == "rtu" else MODBUS_TIMEOUT
        self.timeout = timeout

        if transport == "tcp":
            self._cli = ModbusTcpClient(hostname=ip, port=port, timeout=timeout)
            self._id_str = f"tcp://{ip}:{port}"
        elif transport == "rtu":
            self._cli = ModbusSerialClient(
                port=serial_port,
                baudrate=baudrate,
                parity=parity,
                stopbits=stopbits,
                bytesize=bytesize,
                timeout=timeout,
            )
            self._id_str = f"rtu://{serial_port}@{baudrate}{parity}{bytesize}{stopbits}"
        else:
            raise ValueError(f"未知 transport: {transport!r} (支持 'tcp' / 'rtu')")

    @property
    def id_str(self) -> str:
        return self._id_str

    def connect(self) -> bool:
        return self._cli.connect()

    def close(self):
        try:
            self._cli.close()
        except Exception:
            pass

    def __enter__(self):
        if not self.connect():
            raise ModbusError(f"无法连接 Modbus {self._id_str}")
        return self

    def __exit__(self, *exc):
        self.close()

    # ====== 读 ======

    def read_holding(self, addr: int, count: int = 1, slave: int = 1):
        """FC03 读 holding. 返回 list[int]. 出错抛 ModbusError."""
        rr = self._cli.read_holding_registers(address=addr, count=count, slave=slave)
        if rr.isError():
            raise ModbusError(f"FC03 read_holding(addr={addr}, count={count}) 失败: {rr}")
        return list(rr.registers)

    def read_input(self, addr: int, count: int = 1, slave: int = 1):
        """FC04 读 input."""
        rr = self._cli.read_input_registers(address=addr, count=count, slave=slave)
        if rr.isError():
            raise ModbusError(f"FC04 read_input(addr={addr}, count={count}) 失败: {rr}")
        return list(rr.registers)

    def read_coils(self, addr: int, count: int = 1, slave: int = 1):
        """FC01 读 coils (DO)."""
        rr = self._cli.read_coils(address=addr, count=count, slave=slave)
        if rr.isError():
            raise ModbusError(f"FC01 read_coils(addr={addr}, count={count}) 失败: {rr}")
        return list(rr.bits)[:count]

    def read_discrete_inputs(self, addr: int, count: int = 1, slave: int = 1):
        """FC02 读 discrete inputs (DI)."""
        rr = self._cli.read_discrete_inputs(address=addr, count=count, slave=slave)
        if rr.isError():
            raise ModbusError(f"FC02 read_discrete_inputs(addr={addr}, count={count}) 失败: {rr}")
        return list(rr.bits)[:count]

    # ====== 写 ======

    def write_holding(self, addr: int, value: int, slave: int = 1):
        """FC06 写单个 holding."""
        rr = self._cli.write_register(address=addr, value=value, slave=slave)
        if rr.isError():
            raise ModbusError(f"FC06 write_holding(addr={addr}, value={value}) 失败: {rr}")

    def write_holdings(self, addr: int, values, slave: int = 1):
        """FC16 写多个 holding."""
        rr = self._cli.write_registers(address=addr, values=list(values), slave=slave)
        if rr.isError():
            raise ModbusError(f"FC16 write_holdings(addr={addr}, values={values}) 失败: {rr}")

    def write_coil(self, addr: int, value: bool, slave: int = 1):
        """FC05 写单个 coil (DO)."""
        rr = self._cli.write_coil(address=addr, value=bool(value), slave=slave)
        if rr.isError():
            raise ModbusError(f"FC05 write_coil(addr={addr}, value={value}) 失败: {rr}")

    def write_coils(self, addr: int, values, slave: int = 1):
        """FC15 写多个 coils."""
        rr = self._cli.write_coils(address=addr, values=[bool(v) for v in values], slave=slave)
        if rr.isError():
            raise ModbusError(f"FC15 write_coils(addr={addr}, values={values}) 失败: {rr}")


@contextmanager
def modbus_tcp_client(ip: str = DEVICE_IP, port: int = MODBUS_TCP_PORT,
                      timeout: float = MODBUS_TIMEOUT):
    """TCP 上下文管理器."""
    cli = MbClient(ip=ip, port=port, transport="tcp", timeout=timeout)
    if not cli.connect():
        raise ModbusError(f"无法连接 Modbus TCP {ip}:{port}")
    try:
        yield cli
    finally:
        cli.close()


@contextmanager
def modbus_rtu_client(serial_port: str = MODBUS_RTU_PORT,
                      baudrate: int = MODBUS_RTU_BAUDRATE,
                      parity: str = MODBUS_RTU_PARITY,
                      stopbits: int = MODBUS_RTU_STOPBITS,
                      bytesize: int = MODBUS_RTU_BYTESIZE,
                      timeout: float = MODBUS_RTU_TIMEOUT):
    """RTU 上下文管理器 (串口)."""
    cli = MbClient(transport="rtu", serial_port=serial_port, baudrate=baudrate,
                   parity=parity, stopbits=stopbits, bytesize=bytesize, timeout=timeout)
    if not cli.connect():
        raise ModbusError(f"无法打开 Modbus RTU 串口 {serial_port}@{baudrate}")
    try:
        yield cli
    finally:
        cli.close()


# 向后兼容旧 API
modbus_client = modbus_tcp_client
