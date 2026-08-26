"""SDO 读写: 版本串 / DO 控制 / 配置项钳位与回读.

无 EDS 时用 SdoClient 原始接口: upload 对 <=4B 对象返回 int (或 bytes,
统一转 int); download 传 bytes (struct.pack 显式小端).
"""
import struct

import pytest

import config
from common.canopen_node import NodeHandle

pytestmark = pytest.mark.skipif(not config.HAS_HW,
                                reason="set CANOPEN_CHANNEL to run HW tests")


def sdo_rd_u16(node, index, subindex):
    val = node.sdo.upload(index, subindex)
    if isinstance(val, (bytes, bytearray)):
        val = int.from_bytes(val, "little")
    assert 0 <= val <= 0xFFFF
    return val


def test_version_objects():
    with NodeHandle() as h:
        name = h.node.sdo.upload(0x1008, 0)
        version = h.node.sdo.upload(0x100A, 0)
        if isinstance(name, int):
            name = struct.pack("<Q", name & 0xFFFFFFFFFFFFFFFF)[:4]
        if isinstance(version, int):
            pytest.fail("0x100A string upload returned int, expected bytes")
        assert bytes(name).decode(errors="replace").startswith("canopen-io")
        assert bytes(version).decode(errors="replace").startswith("v")


def test_do_write_readback():
    with NodeHandle() as h:
        h.node.sdo.download(0x2002, 0, struct.pack("<H", 0x0005))
        assert sdo_rd_u16(h.node, 0x2002, 0) == 0x0005
        h.node.sdo.download(0x2002, 0, struct.pack("<H", 0x0000))  # 恢复默认, 关 DO


def test_sample_interval_clamp():
    with NodeHandle() as h:
        # 0x2004 是 ARRAY, 读写须带子索引: 3=DI 间隔, 4=AI 间隔
        h.node.sdo.download(0x2004, 3, struct.pack("<H", 9999))  # -> 钳到 MAX
        assert sdo_rd_u16(h.node, 0x2004, 3) == 5000
        h.node.sdo.download(0x2004, 4, struct.pack("<H", 5))  # -> 钳到 MIN
        assert sdo_rd_u16(h.node, 0x2004, 4) == 10
        h.node.sdo.download(0x2004, 3, struct.pack("<H", 100))  # 恢复默认
        h.node.sdo.download(0x2004, 4, struct.pack("<H", 100))
