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
import sys
import time

import canopen

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
    return node.sdo[0x100A].raw.decode(errors="replace").strip()


def cmd_version(args):
    network = open_network(args)
    try:
        node = network.add_node(args.node_id)
        print(read_version(node))
        return EXIT_OK
    except (canopen.SdoAbortedError, OSError) as e:
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

    network = open_network(args)
    try:
        node = network.add_node(args.node_id)
        try:
            print(f"current version: {read_version(node)}")
        except canopen.SdoAbortedError as e:
            print(f"cannot read version: {e:#010x}", file=sys.stderr)
            return EXIT_COMM

        print("[1/3] reset download state (0x1F51=0) ...")
        try:
            node.sdo[0x1F51][1].raw = 0
        except canopen.SdoAbortedError as e:
            print(f"device rejected 0x1F51=0: {e:#010x}", file=sys.stderr)
            return EXIT_REJECT

        print(f"[2/3] SDO download {len(blob)} bytes into 0x1F50 ...")
        try:
            node.sdo.download(0x1F50, 1, blob, block=True, timeout=args.timeout)
        except TypeError:
            # 老 python-canopen 无 block 参数, 退回分段传输 (慢但可用)
            node.sdo[0x1F50][1].raw = blob
        except canopen.SdoAbortedError as e:
            print(f"SDO download aborted: {e:#010x} "
                  f"(state={node.sdo[0x1F51][1].raw})", file=sys.stderr)
            return EXIT_REJECT

        print("[3/3] confirm (0x1F51=1), device reboots for MCUboot swap ...")
        try:
            node.sdo[0x1F51][1].raw = 1
        except canopen.SdoAbortedError as e:
            print(f"device rejected confirm: {e:#010x}", file=sys.stderr)
            return EXIT_REJECT

        deadline = time.monotonic() + args.wait
        while time.monotonic() < deadline:
            time.sleep(2.0)
            try:
                version = read_version(node)
                print(f"new version: {version}")
                return EXIT_OK
            except (canopen.SdoAbortedError, OSError):
                continue
        print("node did not come back within timeout", file=sys.stderr)
        return EXIT_COMM
    finally:
        network.disconnect()


def main():
    parser = argparse.ArgumentParser(
        description="CANopen firmware upgrade tool (CiA 302-2, canopen-io)")
    parser.add_argument("-c", "--channel", default="can0",
                        help="python-can channel (default: can0)")
    parser.add_argument("--bitrate", type=int, default=250000,
                        help="CAN bitrate (default: 250000)")
    parser.add_argument("--node-id", type=int, default=10,
                        help="target CANopen node ID (default: 10)")

    sub = parser.add_subparsers(dest="cmd", required=True)

    p_ver = sub.add_parser("version", help="read firmware version (0x100A)")
    p_ver.set_defaults(func=cmd_version)

    p_upg = sub.add_parser("upgrade", help="download + confirm firmware")
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
