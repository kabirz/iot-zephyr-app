"""UDP 客户端: 与固件 v3.4 协议 (6 条应用命令) 对齐.

帧格式: [cmd 1B][data...], 回复 [cmd 1B][payload...].
- SET_IP (0x10): 发 [ip4], 回 [ok 1B]
- GET_IP (0x11): 发空, 回 [ip4]   (固件注册为广播允许命令)
- SET_MODBUS (0x12): 发 [slave 1B][baud BE16], 回 [ok 1B]
- GET_MODBUS (0x13): 发空, 回 [slave 1B][baud BE16]
- SET_TIME (0x14): 发 [unix_ts BE32], 回 [ok 1B]
- FACTORY_RESET (0x19): 发空, 回 [ok 1B]
"""
import socket
import struct
import time
from dataclasses import dataclass

from config import UDP_PORT, UDP_REPLY_PORT, UDP_TIMEOUT


class UdpError(Exception):
    """UDP 通信错误."""


@dataclass
class UdpReply:
    cmd: int
    payload: bytes
    rtt_ms: float


class UdpClient:
    """同步 req/resp UDP 客户端."""

    def __init__(self, ip: str, port: int = UDP_PORT, timeout: float = UDP_TIMEOUT):
        self.ip = ip
        self.port = port
        self.timeout = timeout
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.settimeout(timeout)

    def close(self):
        try:
            self._sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _send_recv(self, cmd: int, payload: bytes, expect_cmd: int = None) -> UdpReply:
        """发 [cmd][payload] 到 ip:port, 阻塞等回复. 校验首字节 == expect_cmd."""
        if expect_cmd is None:
            expect_cmd = cmd
        req = bytes([cmd]) + bytes(payload)
        t0 = time.perf_counter()
        try:
            self._sock.sendto(req, (self.ip, self.port))
            data, _ = self._sock.recvfrom(256)
        except socket.timeout as e:
            raise UdpError(f"cmd 0x{cmd:02X}: 设备无响应 (timeout {self.timeout}s)") from e
        except OSError as e:
            raise UdpError(f"cmd 0x{cmd:02X}: socket 错误: {e}") from e
        rtt_ms = (time.perf_counter() - t0) * 1000
        if not data:
            raise UdpError(f"cmd 0x{cmd:02X}: 空回复")
        if data[0] != expect_cmd:
            raise UdpError(
                f"cmd 0x{cmd:02X}: 回复 cmd 字节不匹配 (期望 0x{expect_cmd:02X}, 收到 0x{data[0]:02X})"
            )
        return UdpReply(cmd=data[0], payload=data[1:], rtt_ms=rtt_ms)

    # ====== 应用命令 (0x10+) ======

    def set_ip(self, ip4) -> bool:
        """SET_IP (0x10): 写入新 IP (持久化, 需手动重启生效). 返回 ok 标志."""
        if len(ip4) != 4:
            raise ValueError("ip4 必须为 4 字节")
        r = self._send_recv(0x10, ip4)
        if len(r.payload) < 1:
            raise UdpError("SET_IP 回复过短")
        return r.payload[0] != 0

    def get_ip(self) -> tuple:
        """GET_IP (0x11): 返回 (a, b, c, d)."""
        r = self._send_recv(0x11, b"")
        if len(r.payload) < 4:
            raise UdpError("GET_IP 回复过短")
        return tuple(r.payload[:4])

    def set_modbus(self, slave_id: int, baud: int) -> bool:
        """SET_MODBUS (0x12)."""
        payload = bytes([slave_id & 0xFF]) + struct.pack(">H", baud)
        r = self._send_recv(0x12, payload)
        if len(r.payload) < 1:
            raise UdpError("SET_MODBUS 回复过短")
        return r.payload[0] != 0

    def get_modbus(self) -> tuple:
        """GET_MODBUS (0x13): 返回 (slave_id, baud)."""
        r = self._send_recv(0x13, b"")
        if len(r.payload) < 3:
            raise UdpError("GET_MODBUS 回复过短")
        slave = r.payload[0]
        baud = struct.unpack(">H", r.payload[1:3])[0]
        return (slave, baud)

    def set_time(self, unix_ts: int) -> bool:
        """SET_TIME (0x14): 设备 RTC. 设备内部校验 [2000-01-01, 2100-01-01)."""
        payload = struct.pack(">I", unix_ts & 0xFFFFFFFF)
        r = self._send_recv(0x14, payload)
        if len(r.payload) < 1:
            raise UdpError("SET_TIME 回复过短")
        return r.payload[0] != 0

    def factory_reset(self) -> bool:
        """FACTORY_RESET (0x19): 擦 settings 分区 + 冷重启. 设备会失联."""
        r = self._send_recv(0x19, b"")
        if len(r.payload) < 1:
            raise UdpError("FACTORY_RESET 回复过短")
        return r.payload[0] != 0


def discover(timeout_ms: int = 1500, port: int = UDP_PORT, reply_port: int = UDP_REPLY_PORT):
    """广播 GET_IP (0x11) 发现同子网设备. 返回去重后的 IP 字符串列表.

    实现:
      - 枚举本机所有非回环网卡的子网定向广播地址
      - 向每个广播地址 :port 发 GET_IP
      - 同时监听 :reply_port (固件跨子网回复走有限广播到这里)
      - 收 [0x11][ip4] 回复, 格式化为 "a.b.c.d" 去重
    """
    import platform
    if platform.system() == "Windows":
        return _discover_windows(timeout_ms, port, reply_port)
    return _discover_posix(timeout_ms, port, reply_port)


def _discover_posix(timeout_ms: int, port: int, reply_port: int):
    """POSIX (Linux/macOS) 广播发现."""
    send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    send_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    bcasts = _get_broadcast_addrs_posix()
    if not bcasts:
        bcasts = ["255.255.255.255"]

    req = bytes([0x11])
    for bc in bcasts:
        try:
            send_sock.sendto(req, (bc, port))
        except OSError:
            pass

    # 监听回复: 主 socket (单播回复) + reply_port socket (跨子网广播回复)
    send_sock.settimeout(0.1)
    listen_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    except OSError:
        pass
    try:
        listen_sock.bind(("0.0.0.0", reply_port))
    except OSError:
        pass  # 端口被占用, 仅靠主 socket
    listen_sock.settimeout(0.1)

    found = set()
    import time
    end = time.monotonic() + timeout_ms / 1000
    while time.monotonic() < end:
        for s in (send_sock, listen_sock):
            try:
                data, _ = s.recvfrom(256)
            except socket.timeout:
                continue
            except OSError:
                continue
            if len(data) >= 5 and data[0] == 0x11:
                ip = ".".join(str(b) for b in data[1:5])
                found.add(ip)

    send_sock.close()
    listen_sock.close()
    return sorted(found)


def _discover_windows(timeout_ms: int, port: int, reply_port: int):
    """Windows 广播发现 (与 _discover_posix 行为一致)."""
    send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    send_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    bcasts = _get_broadcast_addrs_windows()
    if not bcasts:
        bcasts = ["255.255.255.255"]

    req = bytes([0x11])
    for bc in bcasts:
        try:
            send_sock.sendto(req, (bc, port))
        except OSError:
            pass

    send_sock.settimeout(0.1)
    listen_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listen_sock.bind(("0.0.0.0", reply_port))
    except OSError:
        pass
    listen_sock.settimeout(0.1)

    found = set()
    import time
    end = time.monotonic() + timeout_ms / 1000
    while time.monotonic() < end:
        for s in (send_sock, listen_sock):
            try:
                data, _ = s.recvfrom(256)
            except socket.timeout:
                continue
            except OSError:
                continue
            if len(data) >= 5 and data[0] == 0x11:
                ip = ".".join(str(b) for b in data[1:5])
                found.add(ip)

    send_sock.close()
    listen_sock.close()
    return sorted(found)


def _get_broadcast_addrs_posix():
    """枚举本机非回环网卡的子网定向广播地址 (Linux: /proc/net/if_inet6 + ioctl)."""
    bcasts = []
    try:
        import netifaces  # type: ignore
        for ifname in netifaces.interfaces():
            addrs = netifaces.ifaddresses(ifname)
            if netifaces.AF_INET not in addrs:
                continue
            for a in addrs[netifaces.AF_INET]:
                ip = a.get("addr")
                mask = a.get("netmask")
                if not ip or not mask or ip.startswith("127."):
                    continue
                bcast = _compute_bcast(ip, mask)
                if bcast:
                    bcasts.append(bcast)
    except ImportError:
        # 无 netifaces, fallback 到有限广播
        bcasts = []
    return bcasts


def _get_broadcast_addrs_windows():
    """Windows: 用 socket.if_nameconfig + ioctl 计算定向广播."""
    bcasts = []
    try:
        for ifname, info in socket.if_nameindex():
            try:
                ip_info = socket.getaddrinfo(ifname, None, socket.AF_INET)
            except OSError:
                continue
            # Windows 不支持 ioctl 取 mask; fallback 有限广播
        bcasts = ["255.255.255.255"]
    except (AttributeError, OSError):
        bcasts = ["255.255.255.255"]
    return bcasts


def _compute_bcast(ip: str, netmask: str):
    """(ip & mask) | ~mask → 定向广播地址."""
    try:
        ip_parts = [int(x) for x in ip.split(".")]
        mask_parts = [int(x) for x in netmask.split(".")]
        if len(ip_parts) != 4 or len(mask_parts) != 4:
            return None
        bcast = []
        for i in range(4):
            bcast.append((ip_parts[i] & mask_parts[i]) | (~mask_parts[i] & 0xFF))
        return ".".join(str(x) for x in bcast)
    except (ValueError, IndexError):
        return None
