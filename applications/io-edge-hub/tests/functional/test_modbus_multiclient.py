"""Modbus TCP 多客户端并发: 最多 3 客户端, 第 4 个应被拒绝/超时.

session 级 modbus fixture 常驻占 1 个槽位, 故以 "fixture + 2 新客户端"
组成 3 路并发, 保证单独运行与套件内运行行为一致.
"""
import socket
import threading
import time

import pytest

from common.modbus_client import MbClient
from config import DEVICE_IP, MODBUS_TCP_PORT


def _wait_slots_free():
    """等待设备回收先前测试关闭的连接 (select 周期 1s)."""
    time.sleep(1.5)


def test_three_clients_concurrent(device_ip, modbus):
    """3 路并发读 (session fixture + 2 个新客户端) 都正常."""
    _wait_slots_free()
    clients = []
    try:
        for i in range(2):
            c = MbClient(ip=device_ip)
            assert c.connect(), f"客户端 {i+1} 连接失败"
            clients.append(c)

        # 并发读: fixture 客户端 + 2 个新客户端
        readers = [modbus] + clients
        results = [None] * 3
        errors = [None] * 3

        def work(idx):
            try:
                results[idx] = readers[idx].read_holding(0, 1)[0]
            except Exception as e:
                errors[idx] = e

        threads = [threading.Thread(target=work, args=(i,)) for i in range(3)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        for i in range(3):
            assert errors[i] is None, f"客户端 {i+1} 读失败: {errors[i]}"
            assert results[i] is not None
    finally:
        for c in clients:
            c.close()


def test_fourth_client_rejected(device_ip, modbus):
    """3 槽占满 (fixture + 2 新连接) 后, 第 4 个连接应在应用层被拒绝.
    TCP 握手可能因 backlog 成功, 故用一次 FC03 读验证设备不响应第 4 路."""
    _wait_slots_free()
    clients = []
    try:
        for i in range(2):
            c = MbClient(ip=device_ip)
            if not c.connect():
                pytest.fail(f"客户端 {i+1} 连接失败 (前置)")
            clients.append(c)

        # 第 4 个连接: 即使三次握手成功, 应用层应关闭它 (读不到响应)
        sock4 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock4.settimeout(3.0)
        try:
            sock4.connect((device_ip, MODBUS_TCP_PORT))
            req = bytes.fromhex("000100000006010300000001")  # FC03 addr=0 count=1
            try:
                sock4.sendall(req)
                resp = sock4.recv(64)
            except (socket.timeout, ConnectionError, OSError):
                return  # 预期: 无响应 / 连接被关闭
            assert not resp, "第 4 个客户端得到了响应, 设备未按 3 连接上限拒绝"
        finally:
            sock4.close()
    finally:
        for c in clients:
            c.close()


def test_client_disconnect_does_not_crash_server(device_ip):
    """客户端连接后立即断开, 不应影响后续连接."""
    for _ in range(5):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(2.0)
        try:
            sock.connect((device_ip, MODBUS_TCP_PORT))
            sock.close()
        except OSError:
            pytest.fail("短连接失败, 服务端可能不稳定")
        time.sleep(0.05)
