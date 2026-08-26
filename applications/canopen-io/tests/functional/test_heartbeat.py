"""心跳: 0x1017 默认 1000ms, 1s 周期帧可观测; NMT Stop 后心跳仍持续且 TPDO 静默.

python-canopen 的 Network.subscribe 回调签名为 (cob_id, data, timestamp).
"""
import time

import pytest

import config
from common.canopen_node import NodeHandle

pytestmark = pytest.mark.skipif(not config.HAS_HW,
                                reason="set CANOPEN_CHANNEL to run HW tests")


def test_heartbeat_period():
    stamps = []

    with NodeHandle() as h:
        h.network.subscribe(0x700 + config.NODE_ID,
                            lambda cob_id, data, ts: stamps.append(time.monotonic()))
        time.sleep(6.0)

    assert len(stamps) >= 4, f"only {len(stamps)} heartbeats in 6s"
    deltas = [b - a for a, b in zip(stamps, stamps[1:])]
    avg = sum(deltas) / len(deltas)
    assert 0.5 <= avg <= 1.5, f"average heartbeat period {avg:.3f}s out of range"


def test_heartbeat_survives_nmt_stop():
    stamps = []
    tpdo1 = []

    with NodeHandle() as h:
        h.network.subscribe(0x700 + config.NODE_ID,
                            lambda cob_id, data, ts: stamps.append(time.monotonic()))
        h.network.subscribe(0x180 + config.NODE_ID,
                            lambda cob_id, data, ts: tpdo1.append(data))

        h.network.nmt.send_command(0x02)  # stop all nodes
        time.sleep(0.5)
        tpdo_base = len(tpdo1)  # 进入 Stopped 后 TPDO 应静默
        time.sleep(2.5)
        assert len(tpdo1) == tpdo_base, (
            f"TPDO1 still transmitting in NMT Stopped "
            f"(+{len(tpdo1) - tpdo_base} frames)")

        h.network.nmt.send_command(0x01)  # 恢复 Operational, 避免影响后续测试
        time.sleep(0.2)

    assert len(stamps) >= 2, "heartbeat stopped after NMT Stop (must continue)"
