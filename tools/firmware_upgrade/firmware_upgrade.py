#!/usr/bin/env python3
"""io-edge-hub 固件升级 CLI (与固件 libs/can_fw_upgrade + udp_fw_upgrade 协议对齐).

支持两条通道:
  udp  - UDP 配置端口 8600 (FW_START/DATA/END, 跨子网可用, 适合常规运维)
  can  - Linux SocketCAN (帧 0x101-0x105, 适合 UDP 不可达场景)
         -b 模式: 0x106/0x107 引导握手, 升级全程在 MCUboot 内完成 (掉底牌也能升)

镜像格式:
  MCUboot 签名镜像 (zephyr.signed.bin). 脚本自动从 TLV 区提取 KEYHASH (32B),
  升级前发到设备, 设备对比自己编译时的 fw_keyhash.h, 不匹配则拒绝.

CRC:
  CRC16-CCITT (poly 0x1021, init 0x0000, bit-reflected), 与 Zephyr crc16_ccitt 一致.

用法:
  # UDP 升级 (常规)
  python firmware_upgrade.py udp -i 192.168.12.101 -f app.signed.bin

  # CAN 升级 (需先 ip link set can0 up type can bitrate 250000)
  python firmware_upgrade.py can -c can0 -f app.signed.bin

  # CAN bootloader 模式 (设备应用损坏也能升级)
  python firmware_upgrade.py upgrade -c can0 -b -f app.signed.bin

  # 查询当前版本 (UDP/CAN 二选一)
  python firmware_upgrade.py version -i 192.168.12.101
  python firmware_upgrade.py version -c can0

  # 测试模式 (test=1, 升级后重启但不永久, MCUboot 下次启动自动回滚)
  python firmware_upgrade.py udp -i 192.168.12.101 -f app.signed.bin --test

  # 跳过 keyhash 校验 (兼容无签名 TLV 的旧镜像, 不推荐)
  python firmware_upgrade.py udp -i 192.168.12.101 -f app.bin --no-keyhash

退出码:
  0 = 升级成功
  1 = 参数/镜像错误
  2 = 通信失败 (设备无响应/超时)
  3 = 设备拒绝 (keyhash 不匹配 / CRC 错误 / 存储不足)
"""
import argparse
import os
import socket
import struct
import sys
import time
from dataclasses import dataclass

# 固件协议常量 (与 libs/udp_fw_upgrade + libs/can_fw_upgrade 对齐)
UDP_FW_PORT_DEFAULT = 8600
UDP_FW_TIMEOUT_DEFAULT = 2.0       # 单帧默认超时
UDP_FW_START_TIMEOUT = 5.0         # FW_START 等擦 slot1
UDP_FW_END_TIMEOUT = 10.0          # FW_END 等 flush + 读回 CRC
UDP_CHUNK_SIZE = 511               # legacy FW_DATA (固件 RX 缓冲 512B - 1B cmd)
UDP_CHUNK_SIZE_V2_MAX = 1400       # DATA_V2 上位机上限 (实际取设备协商值)
UDP_FW_WINDOW = 8                  # DATA_V2 go-back-N 窗口帧数
UDP_FW_V2_ACK_TIMEOUT = 1.0        # DATA_V2 窗口级 ACK 超时 (覆盖渐进擦除的扇区擦停顿 ~400ms)
UDP_FW_V2_MAX_RETRIES = 8          # 单窗口停滞重试上限

CAN_DEFAULT_CHANNEL = "can0"
CAN_DEFAULT_BUSINESS_ID = 0x0111
CAN_ID_FW_CMD = 0x101
CAN_ID_FW_REPLY = 0x102
CAN_ID_FW_DATA = 0x103
CAN_ID_FW_KEYHASH = 0x104
CAN_ID_FW_VERSION = 0x105
CAN_ID_FW_BOOT_PROBE = 0x106
CAN_ID_FW_BOOT_ACK = 0x107
CAN_FW_CMD_START_UPDATE = 0
CAN_FW_CMD_CONFIRM = 1
CAN_FW_CMD_VERSION = 2
CAN_FW_CMD_REBOOT = 3
CAN_FW_CODE_OFFSET = 0
CAN_FW_CODE_UPDATE_SUCCESS = 1
CAN_FW_CODE_VERSION = 2
CAN_FW_CODE_CONFIRM = 3
CAN_FW_CODE_FLASH_ERROR = 4
CAN_FW_CODE_TRANSFER_ERROR = 5
CAN_FW_CODE_KEYHASH_ERROR = 6
CAN_FW_CONFIRM_MAGIC = 0x55AA55AA
CAN_FRAME_TIMEOUT = 3.0
CAN_KEYHASH_FRAMES = 5             # 5 x 7B = 35B (有效 32B SHA-256)
CAN_DATA_FRAME_PAYLOAD = 8
CAN_OFFSET_REPLY_INTERVAL = 8      # 每 8 帧 (64B) 设备回一次 OFFSET
CAN_BOOT_PROBE_MAGIC = 0x42544F31  # 'BTO1'
CAN_BOOT_PROBE_WAIT = 5.0          # 等 MCUboot 探测帧窗口 (含重启时间)

# MCUboot 镜像格式
IMG_MAGIC = 0x96F3B83D
IMG_TLV_INFO_MAGIC = 0x6907
IMG_TLV_KEYHASH = 0x01
IMG_KEYHASH_LEN = 32


class UpgradeError(Exception):
    """升级失败 (通信/校验/拒绝)."""


class ImageFormatError(Exception):
    """镜像格式错误."""


# ================================================================
# 进度展示
# ================================================================

class Progress:
    """简易文本进度条 (无外部依赖)."""

    def __init__(self, total: int, label: str = "", width: int = 40):
        self.total = max(total, 1)
        self.label = label
        self.width = width
        self.last_pct = -1

    def update(self, current: int):
        pct = int(current * 100 / self.total)
        if pct == self.last_pct:
            return
        self.last_pct = pct
        filled = int(self.width * pct / 100)
        bar = "=" * filled + " " * (self.width - filled)
        sys.stdout.write(f"\r{self.label} [{bar}] {pct:3d}%")
        sys.stdout.flush()
        if pct >= 100:
            sys.stdout.write("\n")

    def message(self, msg: str):
        # 进度条未完成时换行后再打印消息
        if self.last_pct >= 0 and self.last_pct < 100:
            sys.stdout.write("\n")
        self.last_pct = -1
        print(msg)


# ================================================================
# 镜像解析
# ================================================================

@dataclass
class FirmwareImage:
    data: bytes
    size: int
    keyhash: bytes = None  # 32B 或 None (无 TLV)


def crc16_ccitt(data: bytes, seed: int = 0x0000) -> int:
    """CRC16-CCITT (poly 0x1021, init 0x0000, bit-reflected).
    与 Zephyr crc16_ccitt (subsys/crc/crc16_sw.c) 完全一致."""
    for b in data:
        e = (seed & 0xFF) ^ b
        f = e ^ (e << 4)
        seed = ((seed >> 8) ^ (f << 8) ^ (f << 3) ^ (f >> 4)) & 0xFFFF
    return seed


def parse_image(path: str, require_keyhash: bool = True) -> FirmwareImage:
    """解析 MCUboot 签名镜像: 校验头 + 提取 keyhash."""
    if not os.path.isfile(path):
        raise ImageFormatError(f"文件不存在: {path}")
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 32:
        raise ImageFormatError(f"文件过短 ({len(data)}B), 不像 MCUboot 镜像")

    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != IMG_MAGIC:
        raise ImageFormatError(
            f"magic 不匹配: 期望 0x{IMG_MAGIC:08X}, 实际 0x{magic:08X} (非 MCUboot 镜像)"
        )

    hdr_size = struct.unpack_from("<H", data, 8)[0]
    img_size = struct.unpack_from("<I", data, 12)[0]

    if hdr_size < 32 or (hdr_size & 0x3):
        raise ImageFormatError(f"hdr_size 异常: {hdr_size}")
    if img_size > len(data):
        raise ImageFormatError(f"img_size {img_size} > 文件大小 {len(data)}")

    tlv_off = hdr_size + img_size
    if tlv_off + 4 > len(data):
        raise ImageFormatError("TLV 区起始越界, 镜像可能被截断")

    tlv_magic = struct.unpack_from("<H", data, tlv_off)[0]
    if tlv_magic != IMG_TLV_INFO_MAGIC:
        raise ImageFormatError(
            f"TLV info magic 不匹配: 期望 0x{IMG_TLV_INFO_MAGIC:04X}, "
            f"实际 0x{tlv_magic:04X}"
        )

    # 找 KEYHASH TLV
    keyhash = None
    off = tlv_off + 4
    while off + 4 <= len(data):
        tp, tlv_len = struct.unpack_from("<HH", data, off)
        if tp == 0 or tlv_len == 0 or off + 4 + tlv_len > len(data):
            break
        if tp == IMG_TLV_KEYHASH and tlv_len == IMG_KEYHASH_LEN:
            keyhash = data[off + 4:off + 4 + IMG_KEYHASH_LEN]
            break
        off += 4 + tlv_len

    if require_keyhash and keyhash is None:
        raise ImageFormatError(
            "镜像无 KEYHASH TLV (未签名?). 用 --no-keyhash 显式跳过校验"
        )

    return FirmwareImage(data=data, size=len(data), keyhash=keyhash)


# ================================================================
# UDP 升级通道
# ================================================================

class UdpUpgrade:
    """UDP 固件升级客户端."""

    def __init__(self, ip: str, port: int = UDP_FW_PORT_DEFAULT):
        self.ip = ip
        self.port = port
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.settimeout(UDP_FW_TIMEOUT_DEFAULT)

    def close(self):
        try:
            self._sock.close()
        except OSError:
            pass

    def _send_recv(self, cmd: int, payload: bytes, timeout: float,
                   min_reply_len: int) -> bytes:
        """发 [cmd][payload], 等回复 (首字节必须 == cmd). 返回 payload 部分."""
        req = bytes([cmd]) + payload
        self._sock.settimeout(timeout)
        try:
            self._sock.sendto(req, (self.ip, self.port))
            data, _ = self._sock.recvfrom(1024)
        except socket.timeout as e:
            raise UpgradeError(f"cmd 0x{cmd:02X}: 设备无响应 (timeout {timeout}s)") from e
        except OSError as e:
            raise UpgradeError(f"cmd 0x{cmd:02X}: socket 错误: {e}") from e
        if not data or data[0] != cmd:
            raise UpgradeError(
                f"cmd 0x{cmd:02X}: 回复不匹配 (收到 0x{data[0]:02X} "
                f"若非空)" if data else f"cmd 0x{cmd:02X}: 空回复"
            )
        if len(data) - 1 < min_reply_len:
            raise UpgradeError(
                f"cmd 0x{cmd:02X}: 回复过短 ({len(data)-1}B, 期望至少 {min_reply_len}B)"
            )
        return data[1:]

    def get_version(self, timeout: float = UDP_FW_TIMEOUT_DEFAULT) -> str:
        """GET_VERSION (0x04)."""
        payload = self._send_recv(0x04, b"", timeout, 1)
        return payload.decode("ascii", errors="replace")

    def reboot(self):
        """REBOOT (0x05). 设备收到即重启, 回复不可靠."""
        try:
            self._send_recv(0x05, b"", UDP_FW_TIMEOUT_DEFAULT, 0)
        except UpgradeError:
            pass  # 重启过程中常失联, 忽略

    def fw_start(self, img_size: int, keyhash: bytes):
        """FW_START (0x01): 擦 slot1 + 初始化 flash_img.
        返回 (status, v2_chunk): status 0=失败 1=成功 2=keyhash 不匹配;
        v2_chunk 为设备 DATA_V2 单帧最大数据量 (老固件回复无此字段 → 0)."""
        payload = struct.pack("<I", img_size)
        if keyhash:
            payload += keyhash
        reply = self._send_recv(0x01, payload, UDP_FW_START_TIMEOUT, 1)
        v2_chunk = struct.unpack("<H", reply[1:3])[0] if len(reply) >= 3 else 0
        return reply[0], v2_chunk

    def fw_data(self, chunk: bytes) -> int:
        """FW_DATA (0x02): 写入 ≤511B 数据. 返回当前 offset."""
        if len(chunk) > UDP_CHUNK_SIZE:
            raise ValueError(f"chunk 超过 {UDP_CHUNK_SIZE}B")
        reply = self._send_recv(0x02, chunk, UDP_FW_TIMEOUT_DEFAULT, 4)
        return struct.unpack("<I", reply[:4])[0]

    def fw_data_v2_stream(self, data: bytes, chunk: int, progress_cb) -> None:
        """FW_DATA_V2 (0x06) 窗口 go-back-N 流式发送.
        连发 UDP_FW_WINDOW 帧不等回复, 按回复中的期望 offset 推进;
        超时/丢帧从最后确认处重传, 设备端按 offset 去重."""
        total = len(data)
        off = 0
        retries = 0
        while off < total:
            # 发送一个窗口: [off, win_end)
            win_end = min(off + UDP_FW_WINDOW * chunk, total)
            w = off
            while w < win_end:
                n = min(chunk, total - w)
                req = bytes([0x06]) + struct.pack("<I", w) + data[w:w + n]
                self._sock.sendto(req, (self.ip, self.port))
                w += n

            # 收窗口内 ACK, 追踪最大确认 offset (回复始终是设备期望 offset)
            deadline = time.monotonic() + UDP_FW_V2_ACK_TIMEOUT
            confirmed = off
            while confirmed < win_end:
                remain = deadline - time.monotonic()
                if remain <= 0:
                    break
                try:
                    self._sock.settimeout(remain)
                    r, _ = self._sock.recvfrom(64)
                except socket.timeout:
                    break
                if len(r) >= 5 and r[0] == 0x06:
                    roff = struct.unpack("<I", r[1:5])[0]
                    if roff > confirmed:
                        confirmed = min(roff, total)
                        retries = 0   # 有推进即重置停滞计数
            progress_cb(confirmed)

            if confirmed >= win_end:
                off = confirmed
                continue
            # 窗口未完全确认: 从确认处 go-back-N 重传 (重复帧设备自动丢弃)
            retries += 1
            if retries > UDP_FW_V2_MAX_RETRIES:
                raise UpgradeError(f"FW_DATA_V2 窗口重试超限 (offset={confirmed}, "
                                   f"设备停滞或链路中断)")
            off = confirmed

    def fw_end(self, test: bool, crc16: int) -> int:
        """FW_END (0x03): flush + 校验 CRC. 返回 result (0=失败 1=成功)."""
        payload = struct.pack("<BH", 1 if test else 0, crc16 & 0xFFFF)
        reply = self._send_recv(0x03, payload, UDP_FW_END_TIMEOUT, 1)
        return reply[0]


def upgrade_udp(ip: str, port: int, image_path: str, test: bool,
                no_keyhash: bool, progress: Progress):
    """完整 UDP 升级流程."""
    img = parse_image(image_path, require_keyhash=not no_keyhash)
    progress.message(f"镜像: {image_path} ({img.size:,}B)"
                     + (f"  keyhash={'有' if img.keyhash else '无'}" if not no_keyhash
                        else "  keyhash=跳过"))

    up = UdpUpgrade(ip, port)
    try:
        # 1. FW_START
        progress.message("[1/4] FW_START (擦写 slot1, 请稍候 ~5s)...")
        status, v2_chunk = up.fw_start(img.size, img.keyhash)
        if status == 2:
            raise UpgradeError("设备拒绝: keyhash 不匹配 (镜像签名密钥与设备 fw_keyhash.h 不一致)")
        if status != 1:
            raise UpgradeError(f"设备拒绝 FW_START (status={status}, 设备忙或存储不足)")
        progress.message("[1/4] FW_START OK")

        # 2. FW_DATA 流式 (设备支持 V2 走窗口流水线, 否则回退停等)
        if v2_chunk >= 512:
            chunk = min(v2_chunk, UDP_CHUNK_SIZE_V2_MAX)
            progress.message(f"[2/4] FW_DATA_V2 发送 {img.size:,}B "
                             f"(窗口 {UDP_FW_WINDOW}×{chunk}B)...")
            sub = Progress(img.size, "      数据")
            up.fw_data_v2_stream(img.data, chunk, sub.update)
        else:
            progress.message(f"[2/4] FW_DATA 发送 {img.size:,}B (停等 511B)...")
            sub = Progress(img.size, "      数据")
            off = 0
            while off < img.size:
                n = min(UDP_CHUNK_SIZE, img.size - off)
                chunk = img.data[off:off + n]
                roff = up.fw_data(chunk)
                # 部分固件实现可能聚合回复, roff 与本地 off 可能不完全同步, 仅校验非回退
                if roff < off:
                    raise UpgradeError(f"FW_DATA offset 回退: 设备={roff}, 本地={off}")
                off += n
                sub.update(off)
        progress.message("[2/4] FW_DATA 完成")

        # 3. FW_END
        crc = crc16_ccitt(img.data)
        progress.message(f"[3/4] FW_END (CRC=0x{crc:04X}, flush + 读回校验 ~10s)...")
        result = up.fw_end(test, crc)
        if result != 1:
            raise UpgradeError(f"FW_END 失败 (result={result}, CRC 不匹配或写 flash 失败)")
        progress.message("[3/4] FW_END OK")

        # 4. 设备自动重启
        mode = "测试模式 (重启后回滚)" if test else "永久模式"
        progress.message(f"[4/4] 升级完成 ({mode}), 设备将重启进行 MCUboot 交换")
    finally:
        up.close()


# ================================================================
# CAN 升级通道
# ================================================================

class CanUpgrade:
    """CAN 固件升级客户端 (基于 python-can, Linux SocketCAN)."""

    def __init__(self, channel: str = CAN_DEFAULT_CHANNEL):
        try:
            import can  # noqa: F401
        except ImportError as e:
            raise UpgradeError(
                "CAN 通道需要 python-can: pip install python-can  (Linux SocketCAN)"
            ) from e
        import can
        self._can = can
        self._bus = can.interface.Bus(interface="socketcan", channel=channel,
                                       receive_own_messages=False)
        self.channel = channel

    def close(self):
        try:
            self._bus.shutdown()
        except Exception:
            pass

    def _send(self, can_id: int, data: bytes):
        if len(data) > 8:
            raise ValueError(f"CAN 数据 ≤8B, 收到 {len(data)}B")
        msg = self._can.Message(arbitration_id=can_id, data=data, is_extended_id=False)
        try:
            self._bus.send(msg, timeout=1.0)
        except self._can.CanError as e:
            raise UpgradeError(f"发送 0x{can_id:03X} 失败: {e}") from e

    def _recv(self, can_id: int = None, timeout: float = CAN_FRAME_TIMEOUT) -> tuple:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            msg = self._bus.recv(timeout=max(0.05, end - time.monotonic()))
            if msg is None:
                continue
            if msg.is_extended_id or msg.is_error_frame or msg.is_remote_frame:
                continue
            if can_id is not None and msg.arbitration_id != can_id:
                continue
            return (msg.arbitration_id, bytes(msg.data))
        raise UpgradeError(
            f"等待 0x{can_id:03X} 超时 ({timeout}s)" if can_id
            else f"等待任意帧超时 ({timeout}s)"
        )

    def _flush_rx(self, duration_s: float = 0.2):
        end = time.monotonic() + duration_s
        while time.monotonic() < end:
            msg = self._bus.recv(timeout=0.05)
            if msg is None:
                break

    def _send_fw_cmd(self, cmd: int, arg: int = 0):
        data = struct.pack("<II", cmd & 0xFFFFFFFF, arg & 0xFFFFFFFF)
        self._send(CAN_ID_FW_CMD, data)

    def _wait_fw_reply(self, timeout: float = CAN_FRAME_TIMEOUT) -> tuple:
        _, data = self._recv(can_id=CAN_ID_FW_REPLY, timeout=timeout)
        if len(data) < 8:
            raise UpgradeError(f"0x102 回复 DLC<8 ({len(data)}B)")
        return struct.unpack("<II", data[:8])

    def get_version(self, timeout: float = 5.0) -> str:
        """CAN VERSION 查询 (0x101 cmd=VERSION → 0x102 + N×0x105)."""
        self._flush_rx()
        self._send_fw_cmd(CAN_FW_CMD_VERSION, 0)
        code, arg = self._wait_fw_reply(timeout=timeout)
        if code != CAN_FW_CODE_VERSION:
            raise UpgradeError(f"VERSION 回复 code 异常: {code}")
        total_len = arg
        if total_len == 0 or total_len > 63:
            raise UpgradeError(f"VERSION 长度异常: {total_len}")

        expected = (total_len + 6) // 7
        version = bytearray()
        end = time.monotonic() + timeout
        for _ in range(expected):
            remaining = max(0.1, end - time.monotonic())
            _, data = self._recv(can_id=CAN_ID_FW_VERSION, timeout=remaining)
            version.extend(data[1:8])
        nul = version.find(0)
        if nul >= 0:
            version = version[:nul]
        else:
            version = version[:total_len]
        return version.decode("ascii", errors="replace")

    def send_keyhash(self, keyhash: bytes, progress=None):
        """0x104 发 5 帧 keyhash 分片 (data[0]=seq, data[1..7]=7B)."""
        if len(keyhash) != IMG_KEYHASH_LEN:
            raise ValueError(f"keyhash 必须 {IMG_KEYHASH_LEN}B")
        for seq in range(CAN_KEYHASH_FRAMES):
            chunk = keyhash[seq * 7:(seq + 1) * 7]
            # 末帧不足 7B 时用 0 填充 (固件按 32B 累积, 多余丢弃)
            chunk = chunk + b"\0" * (7 - len(chunk))
            self._send(CAN_ID_FW_KEYHASH, bytes([seq]) + chunk)
            # 固件收到 keyhash 不回 ACK, 仅 START_UPDATE 时反馈匹配结果
            time.sleep(0.005)  # 给固件 RX 线程喘息, 避免溢出
        if progress:
            progress.message(f"      keyhash 5 帧已发送 ({IMG_KEYHASH_LEN}B)")

    def start_update(self, img_size: int):
        """0x101 START_UPDATE (cmd=0, arg=size). 设备校验 keyhash + 擦 slot1."""
        self._send_fw_cmd(CAN_FW_CMD_START_UPDATE, img_size)
        # 擦 slot1 耗时, 给长超时
        code, arg = self._wait_fw_reply(timeout=10.0)
        if code == CAN_FW_CODE_KEYHASH_ERROR:
            raise UpgradeError("设备拒绝: keyhash 不匹配")
        if code == CAN_FW_CODE_FLASH_ERROR:
            raise UpgradeError(f"设备拒绝: flash 擦除失败 (arg={arg})")
        if code != CAN_FW_CODE_OFFSET:
            raise UpgradeError(f"START_UPDATE 意外回复: code={code} arg={arg}")
        if arg != 0:
            raise UpgradeError(f"START_UPDATE 初始 offset 非 0: {arg}")

    def send_data(self, data: bytes, total: int, progress: Progress):
        """0x103 流式发固件数据 (8B/帧, 每 64B 设备回 OFFSET 做流控)."""
        if len(data) % CAN_DATA_FRAME_PAYLOAD != 0:
            # 固件按 8B 累积, 不足补 0 (末尾不影响, 总量在 START 时确定)
            pad = CAN_DATA_FRAME_PAYLOAD - (len(data) % CAN_DATA_FRAME_PAYLOAD)
            data = data + b"\xff" * pad
        off = 0
        seq_in_block = 0
        while off < len(data):
            chunk = data[off:off + CAN_DATA_FRAME_PAYLOAD]
            self._send(CAN_ID_FW_DATA, chunk)
            off += CAN_DATA_FRAME_PAYLOAD
            seq_in_block += 1
            progress.update(min(off, total))
            if seq_in_block >= CAN_OFFSET_REPLY_INTERVAL or off >= len(data):
                # 等设备 OFFSET 回复做流控
                code, arg = self._wait_fw_reply(timeout=5.0)
                if code == CAN_FW_CODE_FLASH_ERROR:
                    raise UpgradeError(f"FW_DATA flash 写失败 (arg={arg})")
                if code == CAN_FW_CODE_TRANSFER_ERROR:
                    raise UpgradeError(f"FW_DATA 传输错误 (arg={arg})")
                if code == CAN_FW_CODE_UPDATE_SUCCESS:
                    # 全部数据写完, 设备确认
                    break
                if code != CAN_FW_CODE_OFFSET:
                    raise UpgradeError(f"FW_DATA 意外回复: code={code} arg={arg}")
                seq_in_block = 0

    def confirm(self, permanent: bool):
        """0x101 CONFIRM (cmd=1, arg=permanent?1:0). 设备 boot_request_upgrade."""
        self._send_fw_cmd(CAN_FW_CMD_CONFIRM, 1 if permanent else 0)
        code, arg = self._wait_fw_reply(timeout=5.0)
        if code == CAN_FW_CODE_TRANSFER_ERROR:
            raise UpgradeError(f"CONFIRM 传输错误 (arg={arg})")
        if code != CAN_FW_CODE_CONFIRM:
            raise UpgradeError(f"CONFIRM 意外回复: code={code} arg={arg}")
        if arg != CAN_FW_CONFIRM_MAGIC:
            raise UpgradeError(f"CONFIRM magic 不匹配: 0x{arg:08X}")

    def reboot(self):
        """0x101 REBOOT (cmd=3). 设备收到即重启, 回复不可靠."""
        try:
            self._send_fw_cmd(CAN_FW_CMD_REBOOT, 0)
        except UpgradeError:
            pass

    def enter_bootloader(self, progress=None):
        """-b 模式: REBOOT → 等 MCUboot 0x106 探测帧 → 回 0x107.
        之后 keyhash/START/DATA/CONFIRM 全部由 bootloader 应答."""
        self._flush_rx()
        if progress:
            progress.message("      REBOOT 已发送 (等待设备进 bootloader)...")
        self._send_fw_cmd(CAN_FW_CMD_REBOOT, 0)

        _, data = self._recv(can_id=CAN_ID_FW_BOOT_PROBE,
                             timeout=CAN_BOOT_PROBE_WAIT)
        if len(data) < 8 or struct.unpack("<I", data[:4])[0] != CAN_BOOT_PROBE_MAGIC:
            raise UpgradeError(f"探测帧格式异常 (DLC={len(data)})")
        if progress:
            progress.message(f"      bootloader 就绪: v{data[4]}.{data[5]}.{data[6]}")
        self._send(CAN_ID_FW_BOOT_ACK, b"\x5a")


def upgrade_can(channel: str, image_path: str, test: bool,
                no_keyhash: bool, progress: Progress, boot: bool = False):
    """完整 CAN 升级流程. boot=True 时升级全程在 MCUboot 内完成."""
    img = parse_image(image_path, require_keyhash=not no_keyhash)
    progress.message(f"镜像: {image_path} ({img.size:,}B)"
                     + (f"  keyhash={'有' if img.keyhash else '无'}" if not no_keyhash
                         else "  keyhash=跳过"))

    up = CanUpgrade(channel)
    try:
        # 0. bootloader 模式: 重启设备 → 应答 MCUboot 探测帧
        if boot:
            progress.message("[0/4] bootloader 模式: 等待 MCUboot 探测 (0x106)...")
            up.enter_bootloader(progress=progress)
            progress.message("[0/4] bootloader 已应答, 升级将在 bootloader 内进行")

        # 1. 发 keyhash (可选)
        if img.keyhash:
            progress.message("[1/4] 发送 keyhash (5 帧 0x104)...")
            up.send_keyhash(img.keyhash, progress=progress)
            progress.message("[1/4] keyhash 已发送")
        else:
            progress.message("[1/4] 跳过 keyhash (--no-keyhash)")

        # 2. START_UPDATE
        progress.message(f"[2/4] START_UPDATE (size={img.size:,}B, 擦 slot1 ~5s)...")
        up.start_update(img.size)
        progress.message("[2/4] START_UPDATE OK")

        # 3. 数据流
        progress.message(f"[3/4] FW_DATA 发送 {img.size:,}B (8B/帧, 每 64B 流控)...")
        sub = Progress(img.size, "      数据")
        up.send_data(img.data, img.size, sub)
        progress.message("[3/4] FW_DATA 完成")

        # 4. CONFIRM
        progress.message(f"[4/4] CONFIRM ({'测试' if test else '永久'}模式)...")
        up.confirm(permanent=not test)
        if boot:
            progress.message("[4/4] CONFIRM OK, 设备在本会话内交换 (~30-40s),"
                             " 等待新固件...")
        else:
            up.reboot()
            mode = "测试模式 (重启后回滚)" if test else "永久模式"
            progress.message(f"[4/4] CONFIRM OK ({mode}), REBOOT 已发送,"
                             " MCUboot 重启后交换")
    finally:
        up.close()


# ================================================================
# CLI 入口
# ================================================================

def cmd_version(args, progress: Progress):
    """版本查询 (UDP 或 CAN)."""
    if args.ip:
        up = UdpUpgrade(args.ip, args.port)
        try:
            ver = up.get_version()
        finally:
            up.close()
        progress.message(f"UDP 版本: {ver}")
    elif args.channel:
        up = CanUpgrade(args.channel)
        try:
            ver = up.get_version()
        finally:
            up.close()
        progress.message(f"CAN 版本: {ver}")
    else:
        print("错误: 需要 --ip 或 --channel", file=sys.stderr)
        return 1
    return 0


def cmd_upgrade(args):
    """升级主入口 (根据 --ip / --channel 选择通道)."""
    progress = Progress(1, "")  # 占位, 子流程自建 sub-progress
    if getattr(args, "boot", False) and not args.channel:
        print("错误: --boot 仅支持 CAN 通道 (需 --channel)", file=sys.stderr)
        return 1
    try:
        if args.ip:
            upgrade_udp(args.ip, args.port, args.file, args.test,
                        args.no_keyhash, progress)
        elif args.channel:
            upgrade_can(args.channel, args.file, args.test,
                        args.no_keyhash, progress, boot=args.boot)
        else:
            print("错误: 需要 --ip (UDP) 或 --channel (CAN)", file=sys.stderr)
            return 1
        return 0
    except ImageFormatError as e:
        print(f"\n镜像错误: {e}", file=sys.stderr)
        return 1
    except UpgradeError as e:
        print(f"\n升级失败: {e}", file=sys.stderr)
        return 3 if "拒绝" in str(e) or "不匹配" in str(e) or "失败" in str(e) else 2


def main():
    ap = argparse.ArgumentParser(
        description="io-edge-hub 固件升级 (UDP / CAN 双通道)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("用法:")[1] if "用法:" in __doc__ else "",
    )
    sub = ap.add_subparsers(dest="cmd", required=True, metavar="<subcommand>")

    # upgrade 子命令
    p_up = sub.add_parser("upgrade", help="执行固件升级")
    p_up.add_argument("-f", "--file", required=True, help="固件镜像路径 (.signed.bin)")
    p_up.add_argument("-i", "--ip", default=None,
                      help="UDP 目标 IP (不指定则用 CAN 通道)")
    p_up.add_argument("-p", "--port", type=int, default=UDP_FW_PORT_DEFAULT,
                      help=f"UDP 端口 (默认 {UDP_FW_PORT_DEFAULT})")
    p_up.add_argument("-c", "--channel", default=None,
                      help=f"SocketCAN 通道 (如 {CAN_DEFAULT_CHANNEL}, 仅 Linux)")
    p_up.add_argument("--test", action="store_true",
                      help="测试模式 (升级后重启但不永久, MCUboot 下次启动回滚)")
    p_up.add_argument("--no-keyhash", action="store_true",
                      help="跳过 keyhash 校验 (兼容无签名 TLV 旧镜像, 不推荐)")
    p_up.add_argument("-b", "--boot", action="store_true",
                      help="bootloader 升级模式 (仅 CAN): 命令设备重启进 MCUboot"
                           " 并应答其探测帧, 升级全程在 bootloader 内完成")
    p_up.set_defaults(func=cmd_upgrade)

    # version 子命令
    p_ver = sub.add_parser("version", help="查询设备当前版本")
    p_ver.add_argument("-i", "--ip", default=None, help="UDP 目标 IP")
    p_ver.add_argument("-p", "--port", type=int, default=UDP_FW_PORT_DEFAULT,
                       help=f"UDP 端口 (默认 {UDP_FW_PORT_DEFAULT})")
    p_ver.add_argument("-c", "--channel", default=None, help="SocketCAN 通道")
    p_ver.set_defaults(func=lambda a: cmd_version(a, Progress(1, "")))

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
