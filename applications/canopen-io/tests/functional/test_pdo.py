"""PDO: TPDO1 周期随 AI 采样间隔; TPDO2 定时兜底; RPDO1 写 DO 生效."""
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
                            lambda msg: frames.append(msg))
        time.sleep(3.0)

    # AI 默认间隔 100ms: 3s 内应 >= 15 帧, DLC 8 (AI1-4)
    assert len(frames) >= 15, f"only {len(frames)} TPDO1 frames in 3s"
    assert all(f.dlc == 8 for f in frames)


def test_tpdo2_timer_fallback():
    frames = []

    with NodeHandle() as h:
        h.network.subscribe(0x280 + config.NODE_ID,
                            lambda msg: frames.append(msg))
        time.sleep(3.0)

    # DI/DO 无变化时靠 1000ms event timer 兜底: 3s 至少 2 帧, DLC 4
    assert len(frames) >= 2, f"only {len(frames)} TPDO2 frames in 3s"
    assert all(f.dlc == 4 for f in frames)


def test_rpdo1_do_control():
    with NodeHandle() as h:
        h.network.send_message(0x200 + config.NODE_ID, b"\xff\x00")
        time.sleep(0.5)
        assert h.node.sdo[0x2002].raw == 0x00FF
        h.network.send_message(0x200 + config.NODE_ID, b"\x00\x00")
        time.sleep(0.5)
        assert h.node.sdo[0x2002].raw == 0x0000
