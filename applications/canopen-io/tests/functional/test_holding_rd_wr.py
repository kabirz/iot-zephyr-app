"""Holding 寄存器读测试 (只读, 合并版 16 寄存器布局 0x00-0x0F).

与 io-edge-hub 的差异: CAN 业务寄存器 (0x06/0x07) 已删除, IP 前移到
0x08-0x0B, 时间戳移到 0x0C/0x0D.
采样间隔默认值不强制断言精确值 (设备可能已被持久化修改), 仅验证钳位范围;
其余字段按出厂默认严格断言, 失败时先排除已保存过参数的可能.
"""
import pytest

from config import HOLDING_COUNT, HOLDING

# 出厂默认值 (与固件 src/modbus/function.c 静态初始化一致)
DEFAULTS = {
    0x00: 0,       # DO
    0x01: 0xFFFF,  # DI_ENABLE
    0x02: 0x000F,  # AI_ENABLE
    0x03: None,    # DI_SAMPLE_MS: 100..5000 范围断言 (见下)
    0x04: None,    # AI_SAMPLE_MS: 同上
    0x05: 0,       # HISTORY_ENABLE
    0x06: 9600,    # RS485_BAUDRATE
    0x07: 1,       # SLAVE_ID
    0x08: 192,     # IP_OCTET1-4: 192.168.12.101
    0x09: 168,
    0x0A: 12,
    0x0B: 101,
    0x0E: 0,       # CONFIG_SAVE 触发位
    0x0F: 0,       # REBOOT 触发位
    # 0x0C/0x0D 时间戳不在表内: 读时返回实时 time(NULL)
}


def test_read_all_holding_16(modbus):
    regs = modbus.read_holding(0, HOLDING_COUNT)
    assert len(regs) == HOLDING_COUNT


def test_default_values(modbus):
    """静态默认值 + 采样间隔范围 (时间戳寄存器除外)."""
    regs = modbus.read_holding(0, HOLDING_COUNT)
    for addr, expected in DEFAULTS.items():
        if expected is None:
            assert 10 <= regs[addr] <= 5000, \
                f"holding 0x{addr:02X} 采样间隔 {regs[addr]} 不在 [10,5000]"
            continue
        assert regs[addr] == expected, (
            f"holding 0x{addr:02X} 默认值期望 0x{expected:04X}, 实际 0x{regs[addr]:04X} "
            f"(若曾持久化修改过参数属正常)")


def test_default_ip(modbus):
    regs = modbus.read_holding(HOLDING["IP_OCTET1"], 4)
    ip = ".".join(str(r & 0xFF) for r in regs)
    assert ip == "192.168.12.101", f"默认 IP 期望 192.168.12.101, 实际 {ip}"


def test_no_can_registers_in_layout(modbus):
    """合并版布局校验: RS485/SALVE 占据原 CAN 寄存器位置."""
    regs = modbus.read_holding(HOLDING["RS485_BAUDRATE"], 2)
    assert regs[0] in (1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200)
    assert regs[1] & 0xFF == 1


def test_timestamp_registers_return_live_time(modbus):
    """0x0C/0x0D 单读返回实时系统时间而非数组陈旧值 (2000 年之后)."""
    hi = modbus.read_holding(HOLDING["TIMESTAMP_HI"], 1)[0]
    lo = modbus.read_holding(HOLDING["TIMESTAMP_LO"], 1)[0]
    ts = ((hi << 16) | lo) & 0xFFFFFFFF
    assert ts > 946684800, f"时间戳 {ts} 早于 2000-01-01"


def test_read_each_single(modbus):
    """FC03 单独读每个 holding 地址."""
    for addr in range(HOLDING_COUNT):
        if addr in (HOLDING["TIMESTAMP_HI"], HOLDING["TIMESTAMP_LO"]):
            continue
        regs = modbus.read_holding(addr, 1)
        assert len(regs) == 1


def test_read_out_of_range(modbus):
    """读地址 >= 16 应返回异常响应."""
    from common.modbus_client import ModbusError

    with pytest.raises(ModbusError):
        modbus.read_holding(HOLDING_COUNT, 1)


@pytest.mark.write
def test_fc06_write_single_and_readback(modbus, restore_holding):
    """FC06 逐个写安全 holding (避开 DO/触发位/时间戳/RTU 参数)."""
    safe_addrs = [
        HOLDING["DI_ENABLE"],
        HOLDING["AI_ENABLE"],
        HOLDING["DI_SAMPLE_MS"],
        HOLDING["AI_SAMPLE_MS"],
    ]
    for addr in safe_addrs:
        original = modbus.read_holding(addr, 1)[0]
        new_val = (original + 1) & 0xFFFF
        if addr in (HOLDING["DI_SAMPLE_MS"], HOLDING["AI_SAMPLE_MS"]):
            # 设备侧无写入钳位 (读取侧才钳), 保持合法窗口内自增
            new_val = 100 if original >= 5000 else max(10, min(5000, new_val))
        modbus.write_holding(addr, new_val)
        readback = modbus.read_holding(addr, 1)[0]
        assert readback == new_val, (
            f"FC06 写 0x{addr:02X}=0x{new_val:04X}, 读回 0x{readback:04X}")


@pytest.mark.write
def test_fc06_write_same_value_is_noop_ok(modbus):
    """写当前值: 等值短路仍返回成功 (io_write_holding 相等早退)."""
    cur = modbus.read_holding(0x05, 1)[0]
    modbus.write_holding(0x05, cur)  # 不抛异常即成功
