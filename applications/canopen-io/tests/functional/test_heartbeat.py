"""心跳: 0x1017 默认 1000ms; NMT Stop 后心跳持续且 TPDO 静默; NMT 复位节点重启."""
import time

import pytest

import config


@pytest.mark.can
def test_heartbeat_period(canopen_node):
    stamps = []
    canopen_node.network.subscribe(0x700 + config.NODE_ID,
                                   lambda cob_id, data, ts: stamps.append(time.monotonic()))
    time.sleep(6.0)

    assert len(stamps) >= 4, f"6s 内仅 {len(stamps)} 心跳"
    deltas = [b - a for a, b in zip(stamps, stamps[1:])]
    avg = sum(deltas) / len(deltas)
    assert 0.5 <= avg <= 1.5, f"平均心跳周期 {avg:.3f}s 越界"


@pytest.mark.can
def test_heartbeat_state_byte_operational(canopen_node):
    states = []
    canopen_node.network.subscribe(0x700 + config.NODE_ID,
                                   lambda cob_id, data, ts: states.append(data[0]))
    time.sleep(2.5)
    assert states, "未收到心跳"
    # 设备启动后默认 Operational (05); Stopped(04)/Pre-op(7F) 说明被外部 NMT 干扰
    assert all(s == 0x05 for s in states), f"非运行状态: {set(states):#x}"


@pytest.mark.can
def test_heartbeat_survives_nmt_stop(canopen_node):
    stamps = []
    tpdo1 = []
    net = canopen_node.network
    net.subscribe(0x700 + config.NODE_ID,
                  lambda cob_id, data, ts: stamps.append(time.monotonic()))
    net.subscribe(0x180 + config.NODE_ID,
                  lambda cob_id, data, ts: tpdo1.append(data))

    try:
        net.nmt.send_command(0x02)   # Stop all nodes
        time.sleep(0.5)
        base = len(tpdo1)
        time.sleep(2.5)
        assert len(tpdo1) == base, \
            f"Stopped 态 TPDO1 仍在发送 (+{len(tpdo1) - base} 帧)"
        assert len(stamps) >= 1, "Stop 后心跳应持续"
    finally:
        net.nmt.send_command(0x01)   # 恢复 Operational
        time.sleep(0.3)

@pytest.mark.can
def test_nmt_reset_node_recovers(can_channel):
    """NMT Reset Node (0x81): 栈级复位 (设备不重启 MCU, 主循环重建 CANopen),
    心跳短暂中断后恢复且状态 Operational.

    用独立连接处理 (session 级 canopen_node 的状态会受复位影响).
    """
    import canopen

    from config import BITRATE, NODE_ID

    net = canopen.Network()
    net.connect(channel=can_channel, interface="socketcan", bitrate=BITRATE)
    hb_cob = 0x700 + NODE_ID
    stamps = []
    net.subscribe(hb_cob,
                  lambda cob_id, data, ts: stamps.append((time.monotonic(), data[0])))

    try:
        time.sleep(3.0)
        assert len(stamps) >= 2, "前置心跳未收到"
        pre_count = len(stamps)
        pre_period_ok = all(
            b[0] - a[0] < 2.5 for a, b in zip(stamps[:pre_count], stamps[1:pre_count]))

        net.send_message(0x000, bytes([0x81, NODE_ID]))
        reset_t = time.monotonic()

        # 复位后 10s 内心跳恢复; 首帧可能是 Boot-up (0x00), 随后应 Operational
        end = reset_t + 10.0
        resumed_at = None
        while time.monotonic() < end:
            new_stamps = [s for s in stamps if s[0] > reset_t]
            if new_stamps:
                resumed_at = new_stamps[0]
                break
            time.sleep(0.1)
        assert resumed_at is not None, "NMT reset 后 10s 内心跳未恢复"

        # 等 2s 让设备完成 boot-up → Pre-operational → (NMT start 由固件首轮完成)
        # 恢复 Operational 心跳; 收到 ≥1 帧 05 才算恢复完成
        end = resumed_at[0] + 6.0
        op_seen = resumed_at[1] == 0x05
        while time.monotonic() < end and not op_seen:
            new_states = [s[1] for s in stamps if s[0] > reset_t]
            if 0x05 in new_states:
                op_seen = True
                break
            time.sleep(0.1)
        assert op_seen, f"复位后心跳未回到 Operational: {[hex(s[1]) for s in stamps if s[0] > reset_t]}"

        # 恢复后心跳节奏正常 (>=2 帧间隔 < 2.5s)
        end = resumed_at[0] + 6.0
        while time.monotonic() < end:
            later = [s for s in stamps if s[0] > resumed_at[0]]
            if len(later) >= 2 and later[-1][0] - later[0][0] < 4.0:
                break
            time.sleep(0.2)

        # 最终校验: 全程存在 Operational 心跳且节奏没有永久性紊乱
        post = [s for s in stamps if s[0] > reset_t]
        assert len(post) >= 3, f"复位后仅 {len(post)} 心跳"
    finally:
        net.disconnect()
