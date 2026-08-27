"""Modbus TCP 多客户端并发.

合并版固件与 io-edge-hub 相同: RAW ADU + select() 多路复用, 理论无客户端上限
(受 Zephyr socket 槽位约束). 这里验证 "多客户端同时服务" 而非固定拒绝阈值.
"""
import threading
import time

import pytest

from common.modbus_client import MbClient
from config import DEVICE_IP, MODBUS_TCP_PORT


def test_four_clients_concurrent(device_ip):
    """4 个新客户端并发 FC03 全部正常应答."""
    clients = []
    try:
        for i in range(4):
            c = MbClient(ip=device_ip)
            assert c.connect(), f"客户端 {i+1} 连接失败"
            clients.append(c)

        results = [None] * 4
        errors = [None] * 4

        def work(idx):
            try:
                for _ in range(20):
                    results[idx] = clients[idx].read_holding(0x03, 1)[0]
            except Exception as e:  # noqa: BLE001
                errors[idx] = e

        threads = [threading.Thread(target=work, args=(i,)) for i in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        for i in range(4):
            assert errors[i] is None, f"客户端 {i+1} 读失败: {errors[i]}"
            # 读 holding 0x03 (DI 采样间隔): 合法窗口 [10,5000]
            assert results[i] is not None and 10 <= results[i] <= 5000
    finally:
        for c in clients:
            c.close()


def test_client_disconnect_does_not_crash_server(device_ip):
    """短连接 (connect 后立即 close) x20 不影响后续服务."""
    import socket

    for _ in range(20):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(2.0)
        try:
            sock.connect((device_ip, MODBUS_TCP_PORT))
            sock.close()
        except OSError as e:
            pytest.fail(f"短连接失败, 服务端可能不稳定: {e}")
        time.sleep(0.02)

    cli = MbClient(ip=device_ip)
    try:
        assert cli.connect() and cli.read_holding(0x07, 1)[0] >= 1
    finally:
        cli.close()


def test_slow_request_no_starvation(device_ip):
    """一客户端占用期间另一客户端仍能获得服务 (select 复用不互斥阻塞)."""
    a = MbClient(ip=device_ip)
    b = MbClient(ip=device_ip)
    try:
        assert a.connect() and b.connect()

        done = {}

        def slow():
            # 单元号广播 (0) 写 DO 同值 → 库执行副作用但不回复; 随后主动关闭
            try:
                for _ in range(10):
                    a.read_holding(0x03, 1)
                done["a"] = True
            except Exception as e:  # noqa: BLE001
                done["a"] = e

        t = threading.Thread(target=slow)
        t.start()
        time.sleep(0.2)
        t0 = time.perf_counter()
        val = b.read_holding(0x04, 1)[0]
        rtt_ms = (time.perf_counter() - t0) * 1000
        t.join(timeout=5)
        assert done.get("a") is True, f"第一客户端异常: {done.get('a')}"
        assert val <= 5000 or val == 0
        assert rtt_ms < 1500, f"并发下第二客户端 RTT 异常: {rtt_ms:.1f}ms"
    finally:
        a.close()
        b.close()
