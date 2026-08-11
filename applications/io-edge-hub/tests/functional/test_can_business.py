"""CAN 业务帧测试.

固件 mod_can_app_rx 只对匹配 holding 0x06 (CAN_ID) 的帧 LOG_DBG 返回 true,
不响应. 测试只能验证:
  - 发业务帧, 设备不应崩溃 (后续 Modbus FC03 仍可读)
  - 发未注册 ID, 设备忽略

业务帧无回执, 测试以"设备后续仍可通信"作为通过判据.
"""
import time

import pytest

from config import CAN_DEFAULT_BUSINESS_ID

pytestmark = pytest.mark.can


@pytest.mark.write
def test_business_frame_no_crash(can, modbus):
    """发 5 次业务帧, 验证设备后续 Modbus 仍可用."""
    for i in range(5):
        # 业务帧 payload 任意 (固件只 LOG)
        can.send_business(CAN_DEFAULT_BUSINESS_ID, bytes([i, 0xAA, 0xBB, 0xCC]))
        time.sleep(0.05)
    time.sleep(0.3)  # 等 RX 线程处理

    # 验证设备仍响应 Modbus
    val = modbus.read_holding(0, 1)[0]
    assert val is not None


@pytest.mark.write
def test_business_frame_max_dlc(can, modbus):
    """发 8B (max DLC) 业务帧, 设备应正常接收不崩."""
    can.send_business(CAN_DEFAULT_BUSINESS_ID, bytes(range(8)))
    time.sleep(0.2)
    # 设备仍响应
    assert modbus.read_holding(0, 1) is not None


@pytest.mark.write
def test_unregistered_id_ignored(can, modbus):
    """发未注册的 ID (0x200), 设备应忽略, 不影响后续通信."""
    can.send_business(0x200, b"\x01\x02\x03\x04")
    time.sleep(0.2)
    assert modbus.read_holding(0, 1) is not None


@pytest.mark.write
def test_change_business_id(can, modbus):
    """通过 Modbus 改 CAN_ID 不立即生效 (固件 holding_reg_wr 无副作用).
    设备保持原 ID 接收."""
    from config import HOLDING
    original = modbus.read_holding(HOLDING["CAN_ID"], 1)[0]
    try:
        # 改成另一个 ID
        modbus.write_holding(HOLDING["CAN_ID"], 0x0222)
        # 设备仍接收原 ID (启动时读 holding, 运行时不切换)
        can.send_business(original, bytes([0x12, 0x34]))
        time.sleep(0.1)
        # Modbus 仍可用
        assert modbus.read_holding(0, 1) is not None
    finally:
        # 恢复
        modbus.write_holding(HOLDING["CAN_ID"], original)
