"""pytest 配置 + fixtures (canopen-io 合并版布局).

用法示例:
  # 默认只跑只读用例
  CANOPEN_CHANNEL=can0 pytest tests/

  # 启用写参数用例 (--write 或 CANOPEN_WRITE=1)
  CANOPEN_CHANNEL=can0 pytest tests/functional --write

  # 全量 (含 CANopen 用例)
  CANOPEN_CHANNEL=can0 IOEDGE_RTU_PORT=/dev/ttyUSB0 pytest tests/
"""
import os
import sys
import time
from pathlib import Path

import pytest

TESTS_ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS_ROOT))

from common.modbus_client import MbClient  # noqa: E402
from common.udp_client import UdpClient  # noqa: E402
from config import (  # noqa: E402
    DEVICE_IP, MODBUS_TCP_PORT, UDP_PORT,
    CAN_CHANNEL, NODE_ID, BITRATE,
    MODBUS_RTU_PORT, MODBUS_RTU_BAUDRATE, MODBUS_RTU_PARITY,
    MODBUS_RTU_STOPBITS, MODBUS_RTU_BYTESIZE, MODBUS_RTU_SLAVE_ID,
    HOLDING_COUNT,
)


def pytest_addoption(parser):
    parser.addoption("--write", action="store_true", default=False,
                     help="启用写测试 (会修改设备参数)")
    parser.addoption("--ip", default=None,
                     help=f"覆盖设备 IP (默认 {DEVICE_IP})")
    parser.addoption("--can-channel", default=None,
                     help=f"覆盖 SocketCAN 通道 (默认 {CAN_CHANNEL}; 未设置时跳过 CANopen 用例)")
    parser.addoption("--node-id", type=int, default=None,
                     help=f"覆盖 CANopen 节点号 (默认 {NODE_ID})")
    parser.addoption("--no-can", action="store_true", default=False,
                     help="跳过所有 CANopen 测试")
    parser.addoption("--rtu-port", default=None,
                     help=f"覆盖 Modbus RTU 串口 (默认 {MODBUS_RTU_PORT})")
    parser.addoption("--rtu-baud", type=int, default=None,
                     help=f"覆盖 Modbus RTU 波特率 (默认 {MODBUS_RTU_BAUDRATE})")
    parser.addoption("--rtu-slave", type=int, default=None,
                     help=f"覆盖 Modbus RTU slave_id (默认 {MODBUS_RTU_SLAVE_ID})")
    parser.addoption("--no-rtu", action="store_true", default=False,
                     help="跳过所有 Modbus RTU 测试")


def pytest_configure(config):
    config.addinivalue_line("markers", "write: 需要修改设备参数的测试 (用 --write 启用)")
    config.addinivalue_line("markers", "can: 需要 SocketCAN 接口与设备的 CANopen 测试")
    config.addinivalue_line("markers", "rtu: 需要 RS485 串口适配器的 Modbus RTU 测试")


def pytest_collection_modifyitems(config, items):
    skip_write = None if config.getoption("--write") else \
        pytest.mark.skip(reason="需要 --write 启用 (会修改设备参数)")

    skip_can = None
    if config.getoption("--no-can"):
        skip_can = pytest.mark.skip(reason="--no-can 显式跳过 CANopen 测试")

    skip_rtu = None
    if config.getoption("--no-rtu"):
        skip_rtu = pytest.mark.skip(reason="--no-rtu 显式跳过 Modbus RTU 测试")

    for item in items:
        if skip_write and "write" in item.keywords:
            item.add_marker(skip_write)
        if skip_can and "can" in item.keywords:
            item.add_marker(skip_can)
        if skip_rtu and "rtu" in item.keywords:
            item.add_marker(skip_rtu)


def _resolve(request, option_name: str, env_name: str, default):
    val = request.config.getoption(option_name)
    return val if val is not None else os.environ.get(env_name, default)


@pytest.fixture(scope="session", autouse=True)
def sync_device_time(device_ip):
    """套件开始前对时 (RTC 漂移会超出 timestamp 测试 ±2s 窗口)."""
    try:
        with UdpClient(ip=device_ip) as u:
            u.set_time(int(time.time()))
    except Exception:
        pass


@pytest.fixture(scope="session")
def device_ip(request) -> str:
    return _resolve(request, "--ip", "IOEDGE_IP", DEVICE_IP)


@pytest.fixture(scope="session")
def can_channel(request):
    """SocketCAN 通道; 未启用 CANopen 硬件时为 None (CANopen 用例自动跳过)."""
    ch = _resolve(request, "--can-channel", "CANOPEN_CHANNEL", CAN_CHANNEL)
    return ch or None


@pytest.fixture(scope="session")
def node_id(request) -> int:
    val = request.config.getoption("--node-id")
    return val if val is not None else int(os.environ.get("CANOPEN_NODE_ID", NODE_ID))


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
    cli = UdpClient(ip=device_ip)
    yield cli
    cli.close()


@pytest.fixture(scope="session")
def canopen_node(can_channel, node_id):
    """session 级 CANopen 节点句柄 (python-canopen). 无通道时跳过用例."""
    if not can_channel:
        pytest.skip("未设置 CANOPEN_CHANNEL (跳过 CANopen 用例)", allow_module_level=False)
    import canopen

    net = canopen.Network()
    net.connect(channel=can_channel, interface="socketcan", bitrate=BITRATE)
    node = net.add_node(node_id)
    yield node
    net.disconnect()


@pytest.fixture()
def restore_od(canopen_node):
    """快照 0x2002 (DO) 与 0x2004 配置数组, 用例结束恢复."""
    snap = {}

    def _u16(idx, sub):
        v = canopen_node.sdo.upload(idx, sub)
        if isinstance(v, (bytes, bytearray)):
            v = int.from_bytes(v, "little")
        return v

    snap["do"] = _u16(0x2002, 0)
    snap["cfg"] = [_u16(0x2004, i) for i in (1, 2, 3, 4)]
    yield
    try:
        canopen_node.sdo.download(0x2002, 0, snap["do"].to_bytes(2, "little"))
        for i, val in zip((1, 2, 3, 4), snap["cfg"]):
            canopen_node.sdo.download(0x2004, i, int(val).to_bytes(2, "little"))
    except Exception:
        pass


@pytest.fixture(scope="session")
def rtu(request) -> MbClient:
    serial_port = _resolve(request, "--rtu-port", "IOEDGE_RTU_PORT", MODBUS_RTU_PORT)
    if not os.path.exists(serial_port):
        pytest.skip(f"RTU 串口不存在 {serial_port}", allow_module_level=False)
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
    val = request.config.getoption("--rtu-slave")
    return val if val is not None else MODBUS_RTU_SLAVE_ID


@pytest.fixture(scope="function")
def restore_holding(modbus: MbClient):
    """保存全部 holding, 结束后恢复 (重试避免设备重启后静默污染)."""
    original = modbus.read_holding(0, HOLDING_COUNT)
    yield
    for _ in range(3):
        try:
            modbus.write_holdings(0, original)
            return
        except Exception:
            time.sleep(1)
