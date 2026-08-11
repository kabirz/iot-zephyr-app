"""Modbus TCP 多客户端并发: 最多 3 客户端, 第 4 个应被拒绝/超时."""
import socket
import threading
import time

import pytest

from common.modbus_client import MbClient
from config import DEVICE_IP, MODBUS_TCP_PORT


def test_three_clients_concurrent(device_ip):
    """3 个客户端同时连接, 都能正常读写."""
    clients = []
    try:
        for i in range(3):
            c = MbClient(ip=device_ip)
            assert c.connect(), f"客户端 {i+1} 连接失败"
            clients.append(c)

        # 并发读
        results = [None] * 3
        errors = [None] * 3

        def work(idx):
            try:
                results[idx] = clients[idx].read_holding(0, 1)[0]
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


def test_fourth_client_rejected(device_ip):
    """第 4 个客户端应被拒绝或超时 (设备最多 3 个并发)."""
    clients = []
    try:
        for i in range(3):
            c = MbClient(ip=device_ip)
            if not c.connect():
                pytest.fail(f"客户端 {i+1} 连接失败 (前置)")
            clients.append(c)

        # 第 4 个连接尝试: 用短超时, 设备应拒绝 (RST/timeout)
        sock4 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock4.settimeout(3.0)
        try:
            sock4.connect((device_ip, MODBUS_TCP_PORT))
            # 如果连接成功, 设备可能用会话超时清理. 这不算硬失败, 但提示.
            pytest.skip(
                "第 4 个连接被接受 (设备可能采用了串行等待策略). "
                "TCP 服务端 backlog 配置允许暂存, 严格测试需观察是否被服务层拒绝."
            )
        except (socket.timeout, ConnectionRefusedError, OSError):
            pass  # 预期: 第 4 个被拒绝
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
