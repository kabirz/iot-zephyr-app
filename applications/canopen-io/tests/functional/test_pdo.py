"""PDO: TPDO1 周期随 AI 采样; TPDO2 定时兜底; RPDO1 写 DO 生效并同步寄存器."""
import struct
import time

import pytest

import config


@pytest.mark.can
def test_tpdo1_periodic(canopen_node):
    frames = []
    canopen_node.network.subscribe(0x180 + config.NODE_ID,
                                   lambda cob_id, data, ts: frames.append(bytes(data)))
    time.sleep(3.0)
    # AI 默认间隔 100ms: 3s 内 >= 15 帧, 载荷 8B (AI1-4, i16 小端)
    assert len(frames) >= 15, f"3s 内仅 {len(frames)} 帧 TPDO1"
    assert all(len(f) == 8 for f in frames)


@pytest.mark.can
def test_tpdo2_timer_fallback(canopen_node):
    frames = []
    canopen_node.network.subscribe(0x280 + config.NODE_ID,
                                   lambda cob_id, data, ts: frames.append(bytes(data)))
    time.sleep(3.0)
    # DI/DO 无变化靠 event timer 兜底: >= 2 帧 / 3s, 载荷 4B (DI+DO 回读)
    assert len(frames) >= 2, f"3s 内仅 {len(frames)} 帧 TPDO2"
    assert all(len(f) == 4 for f in frames)


@pytest.mark.can
@pytest.mark.write
def test_rpdo1_do_control_and_tpdo_echo(canopen_node, restore_od):
    """RPDO1 写 DO → TPDO2 回读帧反映新值; DO 恢复零."""
    net = canopen_node.network
    echoes = []
    net.subscribe(0x280 + config.NODE_ID,
                  lambda cob_id, data, ts: echoes.append(bytes(data)))
    time.sleep(1.5)
    base = len(echoes)

    net.send_message(0x200 + config.NODE_ID, struct.pack("<H", 0x00FF))
    # 回读 OD 确认写入生效
    deadline = time.monotonic() + 2.0
    val = None
    while time.monotonic() < deadline:
        v = canopen_node.sdo.upload(0x2002, 0)
        if isinstance(v, (bytes, bytearray)):
            v = int.from_bytes(v, "little")
        val = v
        if val == 0x00FF:
            break
        time.sleep(0.05)
    assert val == 0x00FF, f"RPDO1 后 OD 读回 {val:#06x}"

    # TPDO2 兜底周期内应有含该位图的回读帧
    end = time.monotonic() + 3.0
    found = any(len(f) >= 4 and (f[2] | (f[3] << 8)) & 0xFF == 0xFF
                for f in echoes[base:])
    while not found and time.monotonic() < end:
        time.sleep(0.1)
        found = any(len(f) >= 4 and (f[2] | (f[3] << 8)) & 0xFF == 0xFF
                    for f in echoes[base:])
    assert found, "TPDO2 未回读出 RPDO1 写入的 DO 值"

    net.send_message(0x200 + config.NODE_ID, struct.pack("<H", 0))
    time.sleep(0.5)
