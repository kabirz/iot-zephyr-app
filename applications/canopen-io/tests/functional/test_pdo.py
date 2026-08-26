"""PDO: TPDO1 周期随 AI 采样间隔; TPDO2 定时兜底; RPDO1 写 DO 生效.

python-canopen 的 Network.subscribe 回调签名为 (cob_id, data, timestamp);
无 EDS 时 SDO 读写用 sdo.upload / sdo.download 原始接口.
"""
import struct
import time

import pytest

import config
from common.canopen_node import NodeHandle

pytestmark = pytest.mark.skipif(not config.HAS_HW,
                                reason="set CANOPEN_CHANNEL to run HW tests")


def test_tpdo1_periodic():
    frames = []

    with NodeHandle() as h:
        h.network.subscribe(0x180 + config.NODE_ID,
                            lambda cob_id, data, ts: frames.append(data))
        time.sleep(3.0)

    # AI 默认间隔 100ms: 3s 内应 >= 15 帧, 载荷 8B (AI1-4)
    assert len(frames) >= 15, f"only {len(frames)} TPDO1 frames in 3s"
    assert all(len(f) == 8 for f in frames), "TPDO1 payload must be 8 bytes"


def test_tpdo2_timer_fallback():
    frames = []

    with NodeHandle() as h:
        h.network.subscribe(0x280 + config.NODE_ID,
                            lambda cob_id, data, ts: frames.append(data))
        time.sleep(3.0)

    # DI/DO 无变化时靠 1000ms event timer 兜底: 3s 至少 2 帧, 载荷 4B
    assert len(frames) >= 2, f"only {len(frames)} TPDO2 frames in 3s"
    assert all(len(f) == 4 for f in frames), "TPDO2 payload must be 4 bytes"


def test_rpdo1_do_control():
    def do_readback(node):
        val = node.sdo.upload(0x2002, 0)
        if isinstance(val, (bytes, bytearray)):
            val = int.from_bytes(val, "little")
        return val

    with NodeHandle() as h:
        h.network.send_message(0x200 + config.NODE_ID, struct.pack("<H", 0x00FF))
        time.sleep(0.5)
        assert do_readback(h.node) == 0x00FF
        h.network.send_message(0x200 + config.NODE_ID, struct.pack("<H", 0x0000))
        time.sleep(0.5)
        assert do_readback(h.node) == 0x0000
