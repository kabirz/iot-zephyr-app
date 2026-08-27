"""通用辅助: 等待设备上线 / 保存参数 / 重启并等待 (寄存器布局为合并版)."""
import socket
import time

from common.modbus_client import MbClient
import config


def wait_device_online(ip: str, timeout_s: float = 30.0,
                       modbus_port: int = config.MODBUS_TCP_PORT) -> bool:
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
    """写 holding 0x0E 触发 settings_save() (modbus/ 命名空间全量持久化)."""
    mb.write_holding(config.HOLDING["CONFIG_SAVE"], 1)


def reboot_and_wait(mb: MbClient, ip: str, timeout_s: float = 30.0) -> bool:
    """写 holding 0x0F 触发延迟重启 (housekeeping 刷日志后冷重启),
    然后轮询 Modbus TCP 端口等设备重新上线."""
    try:
        mb.write_holding(config.HOLDING["REBOOT"], 1)
    except Exception:
        pass  # 重启过程中连接断开, 忽略
    time.sleep(2)  # 延迟重启 + MCUboot 启动段
    return wait_device_online(ip, timeout_s)


def holding_to_ip(regs) -> str:
    """4 个 IP 段寄存器拼成点分十进制字符串."""
    if len(regs) < 4:
        raise ValueError("需要至少 4 个寄存器")
    return ".".join(str(r & 0xFF) for r in regs[:4])


def read_device_ip(mb: MbClient) -> str:
    """从 holding 0x08-0x0B 读设备 IP."""
    regs = mb.read_holding(config.HOLDING["IP_OCTET1"], 4)
    return holding_to_ip(regs)
