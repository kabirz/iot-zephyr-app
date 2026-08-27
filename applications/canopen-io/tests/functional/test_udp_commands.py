"""UDP 应用命令 (端口 8600): IP 校验矩阵 / MODBUS 交叉验证 / SET_TIME 边界 /
异常路径. 移植自 io-edge-hub, 布局适配合并版 (IP=0x08, RS485=0x06, SLAVE=0x07).
"""
import socket
import struct
import time

import pytest

import config
from config import HOLDING


# ==================== GET_IP / SET_IP ====================

def test_get_ip_returns_4_bytes(udp):
    ip = udp.get_ip()
    assert len(ip) == 4
    for b in ip:
        assert 0 <= b <= 255


def test_get_ip_matches_modbus_holding(udp, modbus):
    """UDP GET_IP 应与 Modbus holding 0x08-0x0B 一致."""
    udp_ip = udp.get_ip()
    mb_regs = modbus.read_holding(HOLDING["IP_OCTET1"], 4)
    mb_ip = tuple(r & 0xFF for r in mb_regs)
    assert udp_ip == mb_ip


@pytest.mark.write
def test_set_ip_valid_same_value(udp, modbus):
    cur = udp.get_ip()
    assert udp.set_ip(bytes(cur)), "SET_IP 当前 IP 被拒绝"
    mb_regs = modbus.read_holding(HOLDING["IP_OCTET1"], 4)
    assert tuple(r & 0xFF for r in mb_regs) == cur


@pytest.mark.write
@pytest.mark.parametrize("last_octet", [0, 255], ids=["network", "broadcast"])
def test_set_ip_reject_last_octet(udp, last_octet):
    cur = udp.get_ip()
    bad = bytes([cur[0], cur[1], cur[2], last_octet])
    assert not udp.set_ip(bad)


@pytest.mark.write
@pytest.mark.parametrize("first", [224, 230, 239, 127, 0, 240, 250, 255],
                         ids=["multic224", "multic230", "multic239",
                              "loopback", "zero-net", "resv240", "resv250", "resv255"])
def test_set_ip_reject_first_octet(udp, first):
    cur = udp.get_ip()
    last = cur[3] if cur[3] not in (0, 255) else 1
    bad = bytes([first, cur[1], cur[2], last])
    assert not udp.set_ip(bad), f"SET_IP 首字节 {first} 应被拒绝"


# ==================== SET_MODBUS / GET_MODBUS ====================

def test_get_modbus(udp):
    slave, baud = udp.get_modbus()
    assert 1 <= slave <= 247
    assert baud > 0


def test_get_modbus_matches_holding(udp, modbus):
    slave, baud = udp.get_modbus()
    regs = modbus.read_holding(HOLDING["RS485_BAUDRATE"], 2)
    assert regs[0] == baud
    assert (regs[1] & 0xFF) == slave


@pytest.mark.write
def test_set_modbus_same_value_persists(udp, modbus):
    cur_slave, cur_baud = udp.get_modbus()
    assert udp.set_modbus(cur_slave, cur_baud)
    regs = modbus.read_holding(HOLDING["RS485_BAUDRATE"], 2)
    assert (regs[1] & 0xFF) == cur_slave and regs[0] == cur_baud


@pytest.mark.write
def test_set_modbus_change_baud_no_reboot(udp):
    """改 baud 只写 holding (重启后才作用于 RTU), 不应自动重启."""
    _, cur_baud = udp.get_modbus()
    new_baud = 19200 if cur_baud != 19200 else 9600
    try:
        assert udp.set_modbus(1, new_baud)
        assert udp.get_modbus() == (1, new_baud)
        # 设备仍在服务 (未重启)
        time.sleep(0.3)
        udp.get_ip()
    finally:
        udp.set_modbus(1, cur_baud)


# ==================== SET_TIME ====================

def test_set_time_current(udp):
    assert udp.set_time(int(time.time()))


def test_set_time_out_of_range_before_2000(udp):
    assert not udp.set_time(946684799), "< TS_MIN (2000-01-01) 应拒绝"


def test_set_time_out_of_range_after_2100(udp):
    assert not udp.set_time(4102444800), "> TS_MAX (2100-01-01) 应拒绝"


# ==================== 异常路径 ====================

def test_unknown_cmd_silent(udp):
    """未知 cmd (0x99) 无回复: app handler 返回 false, 库不回包."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    sock.sendto(bytes([0x99]), (udp.ip, udp.port))
    try:
        data, _ = sock.recvfrom(256)
        assert data[0] != 0x99, "未知 cmd 收到 echo"
    except socket.timeout:
        pass
    finally:
        sock.close()


def test_short_payload_set_ip(udp):
    """SET_IP payload <4B 应回 ok=0 或静默 (固件 len>=4 检查)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    sock.sendto(bytes([0x10, 192, 168]), (udp.ip, udp.port))
    try:
        data, _ = sock.recvfrom(256)
        if data and data[0] == 0x10:
            assert len(data) >= 2 and data[1] == 0
    except socket.timeout:
        pass
    finally:
        sock.close()


def test_fw_cmd_short_frame_no_crash(udp):
    """升级库命令 FW_START(0x01) 空 payload: 不应导致设备失联."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    sock.sendto(bytes([0x01]), (udp.ip, udp.port))
    try:
        try:
            sock.recvfrom(256)
        except socket.timeout:
            pass
    finally:
        sock.close()
    # 设备仍活着
    time.sleep(0.2)
    assert udp.get_ip() is not None
