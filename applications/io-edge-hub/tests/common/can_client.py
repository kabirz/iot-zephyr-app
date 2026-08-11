"""CAN 客户端: 基于 python-can 的 SocketCAN 封装.

依赖: python-can>=4.0 (Linux 下走 socketcan 后端).

协议 (与固件 libs/can_fw_upgrade 对齐):
  0x101 主→设 命令: data[0..3]=cmd LE32, data[4..7]=arg LE32 (DLC=8)
  0x102 设→主 回复: data[0..3]=code LE32, data[4..7]=arg LE32 (DLC=8)
  0x103 主→设 固件数据 (≤8B 原始)
  0x104 主→设 keyhash 分片: data[0]=seq, data[1..7]=7B (5 帧拼 32B SHA-256)
  0x105 设→主 版本分片: data[0]=seq, data[1..7]=ASCII (末帧 '\0' 填充)
"""
import struct
import time

from config import (
    CAN_CHANNEL, CAN_INTERFACE, CAN_FRAME_TIMEOUT,
    CAN_FW_CMD_VERSION, CAN_FW_CODE_VERSION,
    CAN_ID_FW_CMD, CAN_ID_FW_REPLY, CAN_ID_FW_VERSION,
)


class CanError(Exception):
    """CAN 通信错误."""


class CanClient:
    """python-can 客户端封装 (面向 io-edge-hub 固件协议)."""

    def __init__(self, channel: str = CAN_CHANNEL, interface: str = CAN_INTERFACE,
                 timeout: float = CAN_FRAME_TIMEOUT):
        try:
            import can  # python-can
        except ImportError as e:
            raise CanError(
                "需要 python-can: pip install python-can (Linux SocketCAN 后端)"
            ) from e
        self.channel = channel
        self.timeout = timeout
        self._bus = can.interface.Bus(interface=interface, channel=channel, receive_own_messages=False)
        # python-can 4.x: Notifier 可选, 这里直接同步读
        self._filters_set = False

    def close(self):
        try:
            self._bus.shutdown()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ====== 帧 I/O ======

    def send(self, can_id: int, data: bytes, timeout: float = 1.0):
        """发标准帧 (DLC = len(data), ≤8)."""
        import can
        if len(data) > 8:
            raise ValueError(f"CAN 数据 ≤8B, 收到 {len(data)}B")
        msg = can.Message(
            arbitration_id=can_id,
            data=data,
            is_extended_id=False,  # io-edge-hub 用 11 位标准帧
        )
        try:
            self._bus.send(msg, timeout=timeout)
        except can.CanError as e:
            raise CanError(f"发送 0x{can_id:03X} 失败: {e}") from e

    def recv(self, can_id: int = None, timeout: float = None) -> tuple:
        """等下一帧. can_id 指定时过滤该 ID. 返回 (id, data: bytes).

        timeout=None 用 self.timeout."""
        import can
        if timeout is None:
            timeout = self.timeout
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            msg = self._bus.recv(timeout=max(0, end - time.monotonic()))
            if msg is None:
                continue
            if msg.is_extended_id:
                continue  # 仅标准帧
            if msg.is_error_frame or msg.is_remote_frame:
                continue
            if can_id is not None and msg.arbitration_id != can_id:
                continue
            return (msg.arbitration_id, bytes(msg.data))
        raise CanError(f"等待 0x{can_id:03X} 帧超时 ({timeout}s)" if can_id
                       else f"等待任意帧超时 ({timeout}s)")

    def flush_rx(self, duration_s: float = 0.2):
        """清空接收缓冲 (丢弃已有帧)."""
        end = time.monotonic() + duration_s
        while time.monotonic() < end:
            msg = self._bus.recv(timeout=0.05)
            if msg is None:
                break

    # ====== 固件升级协议封装 ======

    def send_fw_cmd(self, cmd: int, arg: int = 0):
        """发 0x101 命令帧 (cmd, arg 都是 LE32)."""
        data = struct.pack("<II", cmd & 0xFFFFFFFF, arg & 0xFFFFFFFF)
        self.send(CAN_ID_FW_CMD, data)

    def wait_fw_reply(self, timeout: float = None) -> tuple:
        """等 0x102 回复, 返回 (code, arg)."""
        _, data = self.recv(can_id=CAN_ID_FW_REPLY, timeout=timeout)
        if len(data) < 8:
            raise CanError(f"0x102 回复 DLC<8 ({len(data)}B)")
        code, arg = struct.unpack("<II", data[:8])
        return (code, arg)

    def query_version(self, timeout: float = 3.0) -> str:
        """完整 VERSION 查询流程, 返回版本字符串.

        流程:
          1. 发 0x101 cmd=VERSION(2)
          2. 收 0x102 code=VERSION(2), arg=字符串总长
          3. 按 ceil(len/7) 收 N 帧 0x105, data[0]=seq, data[1..7]=ASCII
          4. 拼接直到 '\0' 或收齐
        """
        self.flush_rx()
        self.send_fw_cmd(CAN_FW_CMD_VERSION, 0)

        code, arg = self.wait_fw_reply(timeout=timeout)
        if code != CAN_FW_CODE_VERSION:
            raise CanError(f"VERSION 查询意外回复 code={code} arg={arg}")
        total_len = arg
        if total_len == 0 or total_len > 63:
            raise CanError(f"VERSION 字符串长度异常: {total_len}")

        expected_frames = (total_len + 6) // 7  # ceil(len/7)
        version_bytes = bytearray()
        end = time.monotonic() + timeout
        for i in range(expected_frames):
            remaining = max(0.1, end - time.monotonic())
            _, data = self.recv(can_id=CAN_ID_FW_VERSION, timeout=remaining)
            if len(data) < 1:
                continue
            seq = data[0]
            if seq != i:
                # 乱序, 容忍但记录
                pass
            version_bytes.extend(data[1:8])

        # 截到 '\0' 或 total_len
        nul = version_bytes.find(0)
        if nul >= 0:
            version_bytes = version_bytes[:nul]
        else:
            version_bytes = version_bytes[:total_len]
        return version_bytes.decode("ascii", errors="replace")

    # ====== 业务帧 ======

    def send_business(self, business_id: int, data: bytes):
        """发业务帧 (设备接受, 不响应)."""
        self.send(business_id, data)

    def change_business_id(self, modbus, new_id: int):
        """通过 Modbus 改 holding 0x06 CAN_ID."""
        modbus.write_holding(0x06, new_id)
