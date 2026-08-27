"""集成接缝: CANopen OD 与 Modbus/Web 共用寄存器模型的双向镜像.

数据源统一为 holding/input 寄存器 (modbus/ settings 命名空间持久化):
  - SDO/RPDO 写 0x2002 → GPIO + holding[0x00]
  - Modbus 写 holding[0x00] → GPIO + OD 0x2002 回读 + TPDO2
  - 采样线程同时写 input_reg 与 OD 0x2001/0x2000
"""
import struct

import pytest

import config
from config import OD_AI_BASE, OD_DI, OD_DO, OD_CFG_DI_MS, OD_CFG_AI_EN


def _u16(node, idx, sub=0):
    v = node.sdo.upload(idx, sub)
    if isinstance(v, (bytes, bytearray)):
        v = int.from_bytes(v, "little")
    return v


@pytest.mark.can
@pytest.mark.write
def test_do_sdo_write_mirrors_to_modbus(canopen_node, modbus, restore_od):
    canopen_node.sdo.download(OD_DO, 0, struct.pack("<H", 0x0081))
    assert modbus.read_holding(config.HOLDING["DO"])[0] == 0x0081
    assert _u16(canopen_node, OD_DO) == 0x0081
    # 清零恢复
    canopen_node.sdo.download(OD_DO, 0, struct.pack("<H", 0))


@pytest.mark.can
@pytest.mark.write
def test_do_modbus_write_mirrors_to_od(canopen_node, modbus):
    try:
        modbus.write_holding(config.HOLDING["DO"], 0x0018)
        assert _u16(canopen_node, OD_DO) == 0x0018
        bits = modbus.read_coils(0x00, 8)
        assert bits == [(0x18 >> i) & 1 == 1 for i in range(8)]
    finally:
        modbus.write_holding(config.HOLDING["DO"], 0)


@pytest.mark.can
@pytest.mark.write
def test_rpdo1_drives_register(canopen_node, modbus, restore_od):
    """RPDO1 帧 (0x200+node) → GPIO + holding[DO] 同步."""
    net = canopen_node.network
    cob = 0x200 + config.NODE_ID
    net.send_message(cob, struct.pack("<H", 0x00F0))
    deadline = __import__("time").monotonic() + 2.0
    import time as t

    while t.monotonic() < deadline:
        if modbus.read_holding(config.HOLDING["DO"])[0] == 0x00F0:
            break
        t.sleep(0.05)
    assert modbus.read_holding(config.HOLDING["DO"])[0] == 0x00F0


@pytest.mark.can
@pytest.mark.write
def test_config_params_sdo_bridge_bidirectional(canopen_node, modbus):
    """0x2004:3 写 OD → 寄存器; 寄存器写 → OD 读镜像."""
    idx, sub = OD_CFG_DI_MS
    orig_modbus = modbus.read_holding(config.HOLDING["DI_SAMPLE_MS"])[0]
    try:
        canopen_node.sdo.download(idx, sub, struct.pack("<H", 777))
        assert modbus.read_holding(config.HOLDING["DI_SAMPLE_MS"])[0] == 777
        modbus.write_holding(config.HOLDING["DI_SAMPLE_MS"], 333)
        assert _u16(canopen_node, idx, sub) == 333
    finally:
        modbus.write_holding(config.HOLDING["DI_SAMPLE_MS"], orig_modbus)

    idx_en, sub_en = OD_CFG_AI_EN
    orig_en = modbus.read_holding(config.HOLDING["AI_ENABLE"])[0]
    try:
        canopen_node.sdo.download(idx_en, sub_en, struct.pack("<H", 0x0005))
        assert modbus.read_holding(config.HOLDING["AI_ENABLE"])[0] == 0x0005
    finally:
        modbus.write_holding(config.HOLDING["AI_ENABLE"], orig_en)


@pytest.mark.can
def test_di_bitmap_consistent_between_buses(canopen_node, modbus):
    """input reg[DI] 与 OD 0x2001 同源 (采样瞬间可能跨切换, 轮询对齐)."""
    import time

    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        di_modbus = modbus.read_input(config.INPUT["DI"])[0]
        di_can = _u16(canopen_node, OD_DI)
        if di_modbus == di_can:
            return
        time.sleep(0.1)
    pytest.fail("DI 位图双总线不一致")


@pytest.mark.can
def test_ai_values_consistent_between_buses(canopen_node, modbus):
    en = modbus.read_holding(config.HOLDING["AI_ENABLE"])[0]
    for i in range(4):
        if not (en & (1 << i)):
            continue
        ai_modbus = modbus.read_input(config.INPUT["AI0"] + i)[0]
        ai_can = _u16(canopen_node, OD_AI_BASE, i + 1) & 0xFFFF
        assert ai_modbus == ai_can, f"AI{i+1}: modbus={ai_modbus} od={ai_can}"
