"""SDO 读写: 版本串 / DO 控制 / 配置项钳位与回读."""
import pytest

import config
from common.canopen_node import NodeHandle

pytestmark = pytest.mark.skipif(not config.HAS_HW,
                                reason="set CANOPEN_CHANNEL to run HW tests")


def test_version_objects():
    with NodeHandle() as h:
        assert h.node.sdo[0x1008].raw.decode().startswith("canopen-io")
        assert h.node.sdo[0x100A].raw.decode().startswith("v")


def test_do_write_readback():
    with NodeHandle() as h:
        h.node.sdo[0x2002].raw = 0x0005
        assert h.node.sdo[0x2002].raw == 0x0005
        h.node.sdo[0x2002].raw = 0x0000  # 恢复默认, 关 DO


def test_sample_interval_clamp():
    with NodeHandle() as h:
        h.node.sdo[0x2004][3].raw = 9999   # DI 间隔 -> 钳到 MAX
        assert h.node.sdo[0x2004][3].raw == config_max_helper()
        h.node.sdo[0x2004][4].raw = 5      # AI 间隔 -> 钳到 MIN (10)
        assert h.node.sdo[0x2004][4].raw == 10
        h.node.sdo[0x2004][3].raw = 100    # 恢复默认
        h.node.sdo[0x2004][4].raw = 100


def config_max_helper():
    return 5000  # CANOPEN_IO_SAMPLE_MAX_MS 默认值, 与应用 Kconfig 一致
