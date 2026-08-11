"""通用辅助: 等待重启完成 / 等待链路 / 解析 holding 字段."""
import socket
import time

from common.udp_client import UdpClient
from common.modbus_client import MbClient


def wait_device_online(ip: str, timeout_s: float = 30.0,
                       modbus_port: int = 502) -> bool:
    """轮询 TCP 端口直到设备接受连接 (重启后用). 返回是否就绪."""
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        try:
            with socket.create_connection((ip, modbus_port), timeout=2):
                return True
        except OSError:
            time.sleep(0.5)
    return False


def save_settings(mb: MbClient):
    """写 holding 0x10 触发 settings_save()."""
    mb.write_holding(0x10, 1)


def reboot_and_wait(mb: MbClient, ip: str, timeout_s: float = 30.0) -> bool:
    """写 holding 0x11 触发重启, 然后等待设备重新上线."""
    try:
        mb.write_holding(0x11, 1)
    except Exception:
        pass  # 重启过程中连接断开, 忽略
    time.sleep(1)  # 让设备完成重启动作
    return wait_device_online(ip, timeout_s)


def holding_to_ip(regs) -> str:
    """4 个 holding (前 4 个低字节) 拼成 IP 字符串."""
    if len(regs) < 4:
        raise ValueError("需要至少 4 个寄存器")
    return ".".join(str(r & 0xFF) for r in regs[:4])


def read_device_ip(mb: MbClient) -> str:
    """从 holding 0x0A-0x0D 读设备 IP."""
    regs = mb.read_holding(0x0A, 4)
    return holding_to_ip(regs)
