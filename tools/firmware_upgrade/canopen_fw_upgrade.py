#!/usr/bin/env python3
"""CANopen (CiA 302-2) firmware upgrade tool for canopen-io devices.

Protocol (device side implemented in applications/canopen-io/src/fw_download.c):
  1. write OD 0x1F51:01 = 0   -> reset download state machine, open slot1
  2. SDO block download       -> stream signed MCUboot image into 0x1F50:01
  3. write OD 0x1F51:01 = 1   -> flush, boot_request_upgrade(), delayed reboot
  4. poll 0x100A              -> verify new firmware version after reboot

Requires python-canopen (>= 2.x recommended) and a python-can backend
(e.g. SocketCAN on Linux).

Exit codes: 0=ok, 1=image error, 2=communication failure, 3=device rejected.
"""

import argparse
import struct
import sys
import time

import canopen

# python-canopen 2.x 不在顶层导出 SdoError 基类 (只导出 SdoAbortedError /
# SdoCommunicationError 两个子类), 统一从这里取基类, 两类异常都能接住.
try:
    from canopen import SdoError
except ImportError:
    from canopen.sdo.exceptions import SdoError

EXIT_OK = 0
EXIT_IMAGE = 1
EXIT_COMM = 2
EXIT_REJECT = 3


def open_network(args):
    network = canopen.Network()
    network.connect(channel=args.channel, interface="socketcan",
                    bitrate=args.bitrate)
    return network


def read_version(node):
    """读 0x100A 版本串. 无 EDS, 用 SdoClient 原始上传 (<=4B 返回 int,
    字符串返回 bytes)."""
    raw = node.sdo.upload(0x100A, 0)
    if isinstance(raw, int):
        raw = raw.to_bytes(4, "little")
    return bytes(raw).decode(errors="replace").strip()


def cmd_version(args):
    try:
        network = open_network(args)
    except (SdoError, OSError) as e:
        print(f"cannot open CAN channel {args.channel!r}: {e}", file=sys.stderr)
        return EXIT_COMM
    try:
        node = network.add_node(args.node_id)
        print(read_version(node))
        return EXIT_OK
    except (SdoError, OSError) as e:
        print(f"communication failure: {e}", file=sys.stderr)
        return EXIT_COMM
    finally:
        network.disconnect()


def cmd_upgrade(args):
    try:
        with open(args.file, "rb") as f:
            blob = f.read()
    except OSError as e:
        print(f"cannot read image: {e}", file=sys.stderr)
        return EXIT_IMAGE
    if len(blob) < 32:
        print("image too small to be a MCUboot image", file=sys.stderr)
        return EXIT_IMAGE

    try:
        network = open_network(args)
    except (SdoError, OSError) as e:
        print(f"cannot open CAN channel {args.channel!r}: {e}", file=sys.stderr)
        return EXIT_COMM
    try:
        node = network.add_node(args.node_id)
        try:
            old_version = read_version(node)
            print(f"current version: {old_version}")
        except SdoError as e:
            print(f"cannot read version: {e}", file=sys.stderr)
            return EXIT_COMM

        print("[1/3] reset download state (0x1F51=0) ...")
        try:
            node.sdo.download(0x1F51, 0, struct.pack("<I", 0))
        except SdoError as e:
            print(f"device rejected 0x1F51=0: {e}", file=sys.stderr)
            return EXIT_REJECT

        print(f"[2/3] SDO download {len(blob)} bytes into 0x1F50 ...")
        try:
            node.sdo.download(0x1F50, 0, blob, block=True, timeout=args.timeout)
        except TypeError:
            # 老 python-canopen 无 block 参数, 退回分段传输 (慢但可用)
            try:
                node.sdo.download(0x1F50, 0, blob)
            except SdoError as e:
                print(f"SDO download (segmented fallback) aborted: {e}",
                      file=sys.stderr)
                return EXIT_REJECT
        except SdoError as e:
            try:
                state = node.sdo.upload(0x1F51, 0)
                if isinstance(state, (bytes, bytearray)):
                    state = int.from_bytes(state, "little")
            except (SdoError, OSError):
                state = "unknown"
            print(f"SDO download aborted: {e} (state={state})",
                  file=sys.stderr)
            return EXIT_REJECT

        print("[3/3] confirm (0x1F51=1), device reboots for MCUboot swap ...")
        try:
            node.sdo.download(0x1F51, 0, struct.pack("<I", 1))
        except SdoError as e:
            print(f"device rejected confirm: {e}", file=sys.stderr)
            return EXIT_REJECT

        # 确认后设备延迟重启 (MCUboot swap 需 10-30s): 期间 SDO 超时抛
        # SdoCommunicationError. 必须先看到节点离线或版本变化才算成功,
        # 否则读到的可能仍是旧固件应答的 0x100A.
        deadline = time.monotonic() + args.wait
        saw_offline = False
        while time.monotonic() < deadline:
            time.sleep(2.0)
            try:
                version = read_version(node)
            except (SdoError, OSError):
                saw_offline = True
                continue
            if saw_offline or version != old_version:
                print(f"new version: {version}")
                return EXIT_OK
        print("node did not come back / version unchanged, "
              "cannot verify upgrade", file=sys.stderr)
        return EXIT_COMM
    finally:
        network.disconnect()


def main():
    # 通用参数 (-c/--bitrate/--node-id) 挂在两个子命令上: README 用法把它们
    # 放在子命令之后. 不重复挂到顶层 parser — Python 3.9-3.12 子 parser 会用
    # 自己的默认值覆盖顶层已解析的同名值, 顶层+子级重复定义会静默丢弃
    # 顶层传入的选项值 (如 "canopen_fw_upgrade.py -c can1 upgrade ...").
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-c", "--channel", default="can0",
                        help="python-can channel (default: can0)")
    common.add_argument("--bitrate", type=int, default=250000,
                        help="CAN bitrate (default: 250000)")
    common.add_argument("--node-id", type=int, default=10,
                        help="target CANopen node ID (default: 10)")

    parser = argparse.ArgumentParser(
        description="CANopen firmware upgrade tool (CiA 302-2, canopen-io)")

    sub = parser.add_subparsers(dest="cmd", required=True)

    p_ver = sub.add_parser("version", help="read firmware version (0x100A)",
                           parents=[common])
    p_ver.set_defaults(func=cmd_version)

    p_upg = sub.add_parser("upgrade", help="download + confirm firmware",
                           parents=[common])
    p_upg.add_argument("-f", "--file", required=True,
                       help="signed MCUboot image (.bin)")
    p_upg.add_argument("--timeout", type=float, default=10.0,
                       help="SDO timeout in seconds (default: 10, "
                            "须容忍 slot1 扇区擦除尖峰)")
    p_upg.add_argument("--wait", type=float, default=120.0,
                       help="seconds to wait for reboot (default: 120, "
                            "MCUboot swap 需 10-30s)")
    p_upg.set_defaults(func=cmd_upgrade)

    args = parser.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
