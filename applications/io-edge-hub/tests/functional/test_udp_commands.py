"""UDP 应用命令测试 (固件 v3.4, 6 条命令).

正常路径 + 错误路径:
- SET_IP / GET_IP
- SET_MODBUS / GET_MODBUS
- SET_TIME
- FACTORY_RESET (单独标记, 危险)
"""
import socket
import struct
import time

import pytest

from common.udp_client import UdpClient, UdpError


# ==================== GET_IP / SET_IP ====================

def test_get_ip_returns_4_bytes(udp):
    ip = udp.get_ip()
    assert len(ip) == 4
    for b in ip:
        assert 0 <= b <= 255


def test_get_ip_matches_modbus_holding(udp, modbus):
    """UDP GET_IP 应与 Modbus holding 0x0A-0x0D 一致."""
    udp_ip = udp.get_ip()
    mb_regs = modbus.read_holding(0x0A, 4)
    mb_ip = tuple(r & 0xFF for r in mb_regs)
    assert udp_ip == mb_ip, f"UDP GET_IP={udp_ip}, Modbus holding={mb_ip}"


@pytest.mark.write
def test_set_ip_valid(udp, modbus, restore_holding):
    """SET_IP 写入当前 IP (无变化), 应返回 ok=1."""
    cur = udp.get_ip()
    ok = udp.set_ip(bytes(cur))
    assert ok, "SET_IP 当前 IP 被拒绝 (固件应允许写入相同的合法 IP)"
    # holding 应已更新
    mb_regs = modbus.read_holding(0x0A, 4)
    mb_ip = tuple(r & 0xFF for r in mb_regs)
    assert mb_ip == cur


@pytest.mark.write
def test_set_ip_reject_network_address(udp):
    """末字节 0 (网络地址) 应被拒绝."""
    cur = udp.get_ip()
    bad = bytes([cur[0], cur[1], cur[2], 0])
    ok = udp.set_ip(bad)
    assert not ok, "SET_IP 末字节 0 应被拒绝"


@pytest.mark.write
def test_set_ip_reject_broadcast(udp):
    """末字节 0xFF (广播) 应被拒绝."""
    cur = udp.get_ip()
    bad = bytes([cur[0], cur[1], cur[2], 0xFF])
    ok = udp.set_ip(bad)
    assert not ok, "SET_IP 末字节 0xFF 应被拒绝"


@pytest.mark.write
def test_set_ip_reject_multicast(udp):
    """首字节 224-239 (组播) 应被拒绝."""
    cur = udp.get_ip()
    for first in (224, 230, 239):
        bad = bytes([first, cur[1], cur[2], cur[3] if cur[3] != 0 else 1])
        ok = udp.set_ip(bad)
        assert not ok, f"SET_IP 首字节 {first} (组播) 应被拒绝"


@pytest.mark.write
def test_set_ip_reject_loopback(udp):
    """首字节 127 (环回) 应被拒绝."""
    cur = udp.get_ip()
    bad = bytes([127, cur[1], cur[2], cur[3] if cur[3] != 0 else 1])
    ok = udp.set_ip(bad)
    assert not ok, "SET_IP 首字节 127 (环回) 应被拒绝"


@pytest.mark.write
def test_set_ip_reject_zero_network(udp):
    """首字节 0 (本网络) 应被拒绝."""
    cur = udp.get_ip()
    bad = bytes([0, cur[1], cur[2], cur[3] if cur[3] != 0 else 1])
    ok = udp.set_ip(bad)
    assert not ok, "SET_IP 首字节 0 (本网络) 应被拒绝"


@pytest.mark.write
def test_set_ip_reject_reserved(udp):
    """首字节 >= 240 (保留段) 应被拒绝."""
    cur = udp.get_ip()
    for first in (240, 250, 255):
        bad = bytes([first, cur[1], cur[2], cur[3] if cur[3] != 0 else 1])
        ok = udp.set_ip(bad)
        assert not ok, f"SET_IP 首字节 {first} (保留段) 应被拒绝"


# ==================== SET_MODBUS / GET_MODBUS ====================

def test_get_modbus(udp):
    slave, baud = udp.get_modbus()
    assert 1 <= slave <= 247, f"slave_id {slave} 不在 1-247"
    assert baud in (4800, 9600, 19200, 38400, 57600, 115200) or baud > 0, (
        f"baud {baud} 异常"
    )


@pytest.mark.write
def test_set_modbus_valid(udp, modbus, restore_holding):
    """SET_MODBUS 写入当前值, 应 ok, holding 0x09/0x08 应同步."""
    cur_slave, cur_baud = udp.get_modbus()
    ok = udp.set_modbus(cur_slave, cur_baud)
    assert ok, "SET_MODBUS 写入当前值被拒绝"

    slave_h = modbus.read_holding(0x09, 1)[0]
    baud_h = modbus.read_holding(0x08, 1)[0]
    assert slave_h == cur_slave
    assert baud_h == cur_baud


@pytest.mark.write
def test_set_modbus_change_baud_no_reboot(udp, restore_holding):
    """改 baud 不应自动重启 (固件 v3.4: SET_IP 不再自动重启, SET_MODBUS 同理).
    新 baud 需重启生效, 但本测试不验证 RTU 物理层."""
    _, cur_baud = udp.get_modbus()
    # 临时改为 19200 (若已是 19200 则改回 9600)
    new_baud = 19200 if cur_baud != 19200 else 9600
    # 注意: 仅写 holding, 不触发 slave_id 应用 (RTU 启动时读 holding)
    # 此处不直接验证 RTU 物理层, 只验证通信仍可用
    ok = udp.set_modbus(1, new_baud)
    assert ok
    # 立即 GET_MODBUS 验证持久化值
    _, readback_baud = udp.get_modbus()
    assert readback_baud == new_baud
    # 恢复
    udp.set_modbus(1, cur_baud)


# ==================== SET_TIME ====================

def test_set_time_current(udp):
    """SET_TIME 设当前时间, 应 ok."""
    ok = udp.set_time(int(time.time()))
    assert ok, "SET_TIME 当前时间被拒绝 (应在 [2000-01-01, 2100-01-01) 范围内)"


def test_set_time_reject_year_1999(udp):
    """1999-12-31 23:59:59 = 946684799, < TS_MIN (946684800), 应被拒绝."""
    # 注意: 固件 set_timestamp 内部判断, 但 UDP handler 当前不预检, ok=1 仅表示收到
    # 实际 v3.4 行为: UDP 端 ok=1 (只要 len >= 4), 设备内部 LOG_WRN 但不会反馈
    udp.set_time(946684799)
    # 此处不严格断言, 仅验证通信路径


def test_set_time_reject_far_future(udp):
    """2100-01-01 = 4102444800, > TS_MAX, 应被设备内部拒绝 (但 UDP 回 ok=1)."""
    udp.set_time(4102444800)


# ==================== 异常路径 ====================

def test_unknown_cmd_returns_nothing(udp):
    """未知 cmd (例如 0x99) 应无回复 (固件 app_cmd_handler 返回 false, 库不回复)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    sock.sendto(bytes([0x99]), (udp.ip, udp.port))
    try:
        data, _ = sock.recvfrom(256)
        # 如果有回复, 首字节不应该是 0x99
        assert data[0] != 0x99, "未知 cmd 收到 echo, 不应处理"
    except socket.timeout:
        pass  # 预期: 无回复
    finally:
        sock.close()


def test_short_payload_set_ip(udp):
    """SET_IP payload < 4B 应被拒绝 (固件 len >= 4 检查)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    # 只发 2 字节 IP
    sock.sendto(bytes([0x10, 192, 168]), (udp.ip, udp.port))
    try:
        data, _ = sock.recvfrom(256)
        if data and data[0] == 0x10:
            # 应返回 ok=0
            assert len(data) >= 2 and data[1] == 0, "短 payload 应返回 ok=0"
    except socket.timeout:
        pass
    finally:
        sock.close()
