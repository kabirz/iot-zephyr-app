"""pytest 配置 + fixtures.

功能测试默认连接设备 (config.DEVICE_IP). 可用环境变量覆盖:
  IOEDGE_IP=192.168.12.101 pytest functional/ -v
"""
import os
import sys
from pathlib import Path

import pytest

# 把 tests/ 加入 sys.path, 让测试文件能 import config / common.*
TESTS_ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS_ROOT))

from common.modbus_client import MbClient, ModbusError  # noqa: E402
from common.udp_client import UdpClient  # noqa: E402
from config import DEVICE_IP, MODBUS_TCP_PORT, UDP_PORT  # noqa: E402
from config import CAN_CHANNEL, CAN_INTERFACE  # noqa: E402
from config import (  # noqa: E402
    MODBUS_RTU_PORT, MODBUS_RTU_BAUDRATE, MODBUS_RTU_PARITY,
    MODBUS_RTU_STOPBITS, MODBUS_RTU_BYTESIZE, MODBUS_RTU_SLAVE_ID,
)

# 写测试默认关闭, 防止误改设备参数. 用 --write 或 IOEDGE_WRITE=1 开启.
def pytest_addoption(parser):
    parser.addoption("--write", action="store_true", default=False,
                     help="启用写测试 (会修改设备参数)")
    parser.addoption("--ip", default=None,
                     help=f"覆盖设备 IP (默认 {DEVICE_IP})")
    parser.addoption("--can-channel", default=None,
                     help=f"覆盖 SocketCAN 通道 (默认 {CAN_CHANNEL})")
    parser.addoption("--no-can", action="store_true", default=False,
                     help="跳过所有 CAN 测试")
    parser.addoption("--rtu-port", default=None,
                     help=f"覆盖 Modbus RTU 串口 (默认 {MODBUS_RTU_PORT})")
    parser.addoption("--rtu-baud", type=int, default=None,
                     help=f"覆盖 Modbus RTU 波特率 (默认 {MODBUS_RTU_BAUDRATE})")
    parser.addoption("--rtu-slave", type=int, default=None,
                     help=f"覆盖 Modbus RTU slave_id (默认 {MODBUS_RTU_SLAVE_ID})")
    parser.addoption("--no-rtu", action="store_true", default=False,
                     help="跳过所有 Modbus RTU 测试")


def pytest_configure(config):
    # 标记写测试, 未启用 --write 时跳过
    config.addinivalue_line("markers", "write: 需要修改设备参数的测试 (用 --write 启用)")
    config.addinivalue_line("markers", "can: 需要 SocketCAN 接口与设备的 CAN 测试")
    config.addinivalue_line("markers", "rtu: 需要 RS485 串口适配器的 Modbus RTU 测试")


def pytest_collection_modifyitems(config, items):
    if config.getoption("--write"):
        skip_write_marker = None
    else:
        skip_write_marker = pytest.mark.skip(reason="需要 --write 启用 (会修改设备参数)")

    skip_can = None
    if config.getoption("--no-can"):
        skip_can = pytest.mark.skip(reason="--no-can 显式跳过 CAN 测试")

    skip_rtu = None
    if config.getoption("--no-rtu"):
        skip_rtu = pytest.mark.skip(reason="--no-rtu 显式跳过 Modbus RTU 测试")

    for item in items:
        if skip_write_marker and "write" in item.keywords:
            item.add_marker(skip_write_marker)
        if skip_can and "can" in item.keywords:
            item.add_marker(skip_can)
        if skip_rtu and "rtu" in item.keywords:
            item.add_marker(skip_rtu)


@pytest.fixture(scope="session", autouse=True)
def sync_device_time(device_ip):
    """套件开始前对时 (LSI RTC 漂移会超出 timestamp 测试 ±2s 窗口)."""
    import time as _time
    try:
        with UdpClient(ip=device_ip) as u:
            u.set_time(int(_time.time()))
    except Exception:
        pass


@pytest.fixture(scope="session")
def device_ip(request) -> str:
    ip = request.config.getoption("--ip") or os.environ.get("IOEDGE_IP", DEVICE_IP)
    return ip


@pytest.fixture(scope="session")
def modbus(device_ip) -> MbClient:
    """session 级 Modbus TCP 客户端."""
    cli = MbClient(ip=device_ip)
    if not cli.connect():
        pytest.fail(f"无法连接 Modbus TCP {device_ip}:{MODBUS_TCP_PORT}", pytrace=False)
    yield cli
    cli.close()


@pytest.fixture(scope="session")
def udp(device_ip) -> UdpClient:
    """session 级 UDP 客户端."""
    cli = UdpClient(ip=device_ip)
    yield cli
    cli.close()


@pytest.fixture(scope="session")
def can(request) -> "CanClient":
    """session 级 CAN 客户端 (python-can + SocketCAN). 无 CAN 接口时跳过."""
    from common.can_client import CanClient, CanError
    channel = request.config.getoption("--can-channel") or CAN_CHANNEL
    try:
        cli = CanClient(channel=channel, interface=CAN_INTERFACE)
    except CanError as e:
        pytest.skip(f"CAN 接口不可用 ({channel}): {e}", allow_module_level=False)
    yield cli
    cli.close()


@pytest.fixture(scope="session")
def rtu(request) -> MbClient:
    """session 级 Modbus RTU 客户端 (USB-RS485 适配器). 无串口时跳过."""
    serial_port = request.config.getoption("--rtu-port") or MODBUS_RTU_PORT
    baud = request.config.getoption("--rtu-baud") or MODBUS_RTU_BAUDRATE
    cli = MbClient(transport="rtu", serial_port=serial_port, baudrate=baud,
                   parity=MODBUS_RTU_PARITY, stopbits=MODBUS_RTU_STOPBITS,
                   bytesize=MODBUS_RTU_BYTESIZE)
    if not cli.connect():
        pytest.skip(f"无法打开 RTU 串口 {serial_port}@{baud}", allow_module_level=False)
    yield cli
    cli.close()


@pytest.fixture(scope="session")
def rtu_slave_id(request) -> int:
    return request.config.getoption("--rtu-slave") or MODBUS_RTU_SLAVE_ID


@pytest.fixture(scope="function")
def restore_holding(modbus: MbClient):
    """保存所有 holding, 测试结束后恢复. 重试避免设备重启后静默污染."""
    import time as _time
    original = modbus.read_holding(0, 18)
    yield
    for _ in range(3):
        try:
            modbus.write_holdings(0, original)
            return
        except Exception:
            _time.sleep(1)
