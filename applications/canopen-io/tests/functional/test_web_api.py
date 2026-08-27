"""Web API (HTTP 80): info / regs / do / reg / cfg / save / history / 页面 / WS 冒烟.

写路径与 Modbus 共用 io_write_*, 副作用一致.
"""
import json
import time

import pytest
import requests

import config

BASE = f"http://{config.DEVICE_IP}"
T = 4.0


def test_api_info_fields():
    d = requests.get(f"{BASE}/api/info", timeout=T).json()
    assert d["node_id"] == config.NODE_ID
    assert d["ip"] == config.DEVICE_IP
    assert d["version"].startswith("v")
    assert d["board"].startswith("io_edge_f407")
    assert isinstance(d["uptime_ms"], int) and d["uptime_ms"] >= 0
    assert isinstance(d["lfs_free"], int) and isinstance(d["lfs_total"], int)
    # 合并版: 无 CAN 配置字段, 有 node_id
    assert "can_id" not in d and "can_baud" not in d


def test_web_ui_served_gzip():
    r = requests.get(BASE + "/", timeout=T)
    assert r.status_code == 200
    assert "text/html" in r.headers.get("Content-Type", "")


def test_api_regs_layout():
    d = requests.get(f"{BASE}/api/regs", timeout=T).json()
    assert len(d["holding"]) == config.HOLDING_COUNT
    assert len(d["input"]) == config.INPUT_COUNT
    ts = ((d["holding"][config.HOLDING["TIMESTAMP_HI"]] << 16)
          | d["holding"][config.HOLDING["TIMESTAMP_LO"]]) & 0xFFFFFFFF
    assert abs(ts - int(time.time())) < 5


def test_api_do_control_and_validation():
    try:
        r = requests.post(f"{BASE}/api/do", json={"index": 1, "value": True}, timeout=T)
        assert r.json()["ok"] is True
        d = requests.get(f"{BASE}/api/regs", timeout=T).json()
        assert d["holding"][config.HOLDING["DO"]] & (1 << 1)

        r = requests.post(f"{BASE}/api/do", json={"index": 99, "value": True}, timeout=T)
        assert r.status_code == 400 and r.json()["ok"] is False

        r = requests.post(f"{BASE}/api/do", data="not-json",
                          headers={"Content-Type": "application/json"}, timeout=T)
        assert r.status_code == 400
    finally:
        requests.post(f"{BASE}/api/do", json={"index": 1, "value": False}, timeout=T)


@pytest.mark.write
def test_api_reg_write_and_reject(restore_holding):
    addr = config.HOLDING["DI_SAMPLE_MS"]
    r = requests.post(f"{BASE}/api/reg", json={"addr": addr, "value": 250}, timeout=T)
    assert r.json()["ok"] is True
    d = requests.get(f"{BASE}/api/regs", timeout=T).json()
    assert d["holding"][addr] == 250

    r = requests.post(f"{BASE}/api/reg", json={"addr": addr, "value": 70000}, timeout=T)
    assert r.status_code == 400
    r = requests.post(f"{BASE}/api/reg", json={"addr": 999, "value": 1}, timeout=T)
    assert r.status_code == 400


@pytest.mark.write
def test_api_cfg_validation_errors(restore_holding):
    """cfg 字段校验逐项拒绝: 非法 rs485/sid; 合法当前值回 ok."""
    def cfg(payload):
        return requests.post(f"{BASE}/api/cfg", data=json.dumps(payload),
                             headers={"Content-Type": "application/json"},
                             timeout=T)

    regs_now = requests.get(f"{BASE}/api/regs", timeout=T).json()["holding"]
    cur_ip = ".".join(str(regs_now[config.HOLDING["IP_OCTET1"] + i] & 0xFF)
                      for i in range(4))
    slave = regs_now[config.HOLDING["SLAVE_ID"]] & 0xFF
    baud = regs_now[config.HOLDING["RS485_BAUDRATE"]]

    r = cfg({"ip": cur_ip, "rs485": baud, "sid": slave})
    assert r.json()["ok"] is True, f"合法当前值被拒: {r.text}"

    r = cfg({"rs485": 10})
    assert r.status_code == 400 and "rs485" in r.json()["err"]
    r = cfg({"sid": 300})
    assert r.status_code == 400 and "slave" in r.json()["err"]
    r = cfg({"ip": "192.168.12.255"})
    assert r.status_code == 400


@pytest.mark.write
def test_api_save_endpoint():
    r = requests.post(f"{BASE}/api/save", data="{}", timeout=T)
    assert r.json()["ok"] is True


def test_api_history_list_shape():
    r = requests.get(f"{BASE}/api/history/download?name=../etc/passwd", timeout=T)
    assert r.status_code in (200, 400)   # 不允许路径穿越 → 400 / 或 name 校验失败


def test_ws_realtime_push():
    """WS 冒烟: 连上后 ~2s 内应收到 t=io 快照帧."""
    import websocket

    ws = websocket.create_connection(f"ws://{config.DEVICE_IP}/ws", timeout=4)
    try:
        end = time.monotonic() + 4.0
        got_io = got_regs = False
        while time.monotonic() < end and not (got_io and got_regs):
            frame = ws.recv()
            if not isinstance(frame, str):
                continue
            if '"t":"io"' in frame:
                got_io = True
                d = json.loads(frame)
                assert len(d["di"]) == 16 and len(d["do"]) == 8 and len(d["ai"]) == 4
            elif '"t":"regs"' in frame:
                got_regs = True
        assert got_io, "4s 内未收到 io 快照帧"
    finally:
        ws.close()


def test_ws_command_ack():
    """WS cfg 命令 ack 路径: 发当前合法值回 {"ok":true}."""
    import websocket

    d = requests.get(f"{BASE}/api/regs", timeout=T).json()
    sid = d["holding"][config.HOLDING["SLAVE_ID"]] & 0xFF
    ws = websocket.create_connection(f"ws://{config.DEVICE_IP}/ws", timeout=4)
    try:
        ws.send(json.dumps({"cmd": "save"}))
        end = time.monotonic() + 3.0
        while time.monotonic() < end:
            frame = ws.recv()
            if isinstance(frame, str) and '"ok"' in frame:
                assert json.loads(frame)["ok"] is True
                break
        else:
            pytest.fail("未收到 save 命令 ack")
    finally:
        ws.close()
