"""心跳: 0x1017 默认 1000ms, 1s 周期帧可观测; NMT Stop 后心跳仍持续."""
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
                            lambda msg: stamps.append(time.monotonic()))
        time.sleep(6.0)

    assert len(stamps) >= 4, f"only {len(stamps)} heartbeats in 6s"
    deltas = [b - a for a, b in zip(stamps, stamps[1:])]
    avg = sum(deltas) / len(deltas)
    assert 0.5 <= avg <= 1.5, f"average heartbeat period {avg:.3f}s out of range"


def test_heartbeat_survives_nmt_stop():
    stamps = []

    with NodeHandle() as h:
        h.network.subscribe(0x700 + config.NODE_ID,
                            lambda msg: stamps.append(time.monotonic()))
        h.network.nmt.send_command(0x02)  # stop all nodes
        time.sleep(3.0)

    assert len(stamps) >= 2, "heartbeat stopped after NMT Stop (must continue)"
