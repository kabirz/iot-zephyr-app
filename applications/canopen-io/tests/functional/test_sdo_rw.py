"""SDO 读写: 版本对象 / DO 控制 / 配置钳位 / 触发子索引回读语义."""
import struct
import time

import pytest

import config
from config import OD_DO, OD_CFG_DI_MS, OD_CFG_AI_MS


def _u16(node, idx, sub=0):
    v = node.sdo.upload(idx, sub)
    if isinstance(v, (bytes, bytearray)):
        v = int.from_bytes(v, "little")
    return v


@pytest.mark.can
def test_version_objects(canopen_node):
    """0x1008/0x100A 信息串; 0x1017 心跳默认值范围 (读写权限验证)."""
    name = bytes(canopen_node.sdo.upload(0x1008, 0)).decode(errors="replace")
    ver = bytes(canopen_node.sdo.upload(0x100A, 0)).decode(errors="replace")
    assert name.startswith("canopen-io")
    assert ver.startswith("v")

    hb = _u16(canopen_node, 0x1017)
    assert 1 <= hb <= 60000
    canopen_node.sdo.download(0x1017, 0, hb.to_bytes(2, "little"))
    assert _u16(canopen_node, 0x1017) == hb


@pytest.mark.can
def test_do_write_readback(canopen_node, restore_od):
    node = canopen_node
    node.sdo.download(OD_DO, 0, struct.pack("<H", 0x0005))
    assert _u16(node, OD_DO) == 0x0005
    node.sdo.download(OD_DO, 0, struct.pack("<H", 0xFF00))
    assert _u16(node, OD_DO) == 0xFF00
    node.sdo.download(OD_DO, 0, struct.pack("<H", 0))


@pytest.mark.can
def test_sample_interval_clamp(canopen_node, restore_od):
    """0x2004 ARRAY 带子索引读写: 超界写入被设备端钳位到 [10,5000]."""
    idx_d, sub_d = OD_CFG_DI_MS
    idx_a, sub_a = OD_CFG_AI_MS

    canopen_node.sdo.download(idx_d, sub_d, struct.pack("<H", 9999))
    assert _u16(canopen_node, idx_d, sub_d) == 5000
    canopen_node.sdo.download(idx_a, sub_a, struct.pack("<H", 5))
    assert _u16(canopen_node, idx_a, sub_a) == 10

    # 边界内写入精确保持
    canopen_node.sdo.download(idx_d, sub_d, struct.pack("<H", 1234))
    assert _u16(canopen_node, idx_d, sub_d) == 1234


@pytest.mark.can
def test_trigger_subindices_readback_zero(canopen_node, restore_od):
    """:5/:6 触发位写 1 后回读恒 0 (cfg_write 清零语义)."""
    for idx, sub in ((0x2004, 5), (0x2004, 6)):
        canopen_node.sdo.download(idx, sub, struct.pack("<H", 1))
        time.sleep(0.2)
        assert _u16(canopen_node, idx, sub) == 0


@pytest.mark.can
def test_sdo_abort_unknown_object(canopen_node):
    """读不存在的对象 → 设备 abort 0x06020000 (object does not exist)."""
    with pytest.raises(Exception) as exc:
        canopen_node.sdo.upload(0x7123, 0)
    name = type(exc.value).__name__
    msg = str(exc.value)
    assert "SdoAbortedError" in name or "abort" in msg.lower(), \
        f"非设备 abort 异常: {name}: {msg}"
    assert "06020000" in msg or "exist" in msg.lower(), f"意外 abort 码: {msg}"


@pytest.mark.can
def test_save_trigger_rejected_semantics_via_modbus_guard(canopen_node):
    """:5 触发写 1 实际执行一次保存 (等价 holding 0x0E); 写后 :5 回读为 0.

    注意下载进行中会被拒 (ODR_DATA_DEV_STATE), 正常态允许.
    settings_save() 在设备主循环做外部 NOR 擦写 (~百毫秒级), 其后首次
    SDO 回读可能超时, 需带重试.
    """
    import config as cfg
    from common.modbus_client import modbus_tcp_client

    def u16_retry(idx, sub, attempts=8, delay=0.4):
        last_err = None

        for _ in range(attempts):
            try:
                return _u16(canopen_node, idx, sub)
            except Exception as e:  # noqa: BLE001
                last_err = e
                time.sleep(delay)
        raise AssertionError(f"SDO 读 {idx:#06x}:{sub} 持续失败: {last_err}")

    with modbus_tcp_client(ip=cfg.DEVICE_IP) as mb:
        before = mb.read_holding(cfg.HOLDING["DI_SAMPLE_MS"], 1)[0]

    canopen_node.sdo.download(0x2004, 5, struct.pack("<H", 1))
    assert u16_retry(0x2004, 5) == 0

    with modbus_tcp_client(ip=cfg.DEVICE_IP) as mb:
        after = mb.read_holding(cfg.HOLDING["DI_SAMPLE_MS"], 1)[0]
    assert after == before   # save 不改变参数本身
