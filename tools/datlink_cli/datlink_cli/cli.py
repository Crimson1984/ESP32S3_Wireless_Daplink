from __future__ import annotations

import argparse
import json
import secrets
import struct
import sys

from . import protocol
from .client import DatlinkClient, Progress
from .image import load_image


def parse_int(value: str) -> int:
    return int(value, 0)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="datlink")
    parser.add_argument("--port", help="Gateway TinyUSB CDC COM port")
    parser.add_argument("--timeout", type=float, default=3.0)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("ports", help="list serial ports")
    sub.add_parser("info")
    sub.add_parser("link")
    program = sub.add_parser("program")
    program.add_argument("image")
    program.add_argument("--base", type=parse_int)
    program.add_argument("--operation-id", type=parse_int)
    program.add_argument("--wait", type=float, default=120.0)
    sub.add_parser("verify", help="show latest programmer progress/result")
    sub.add_parser("reset")
    sub.add_parser("abort")
    sub.add_parser("target-info")
    return parser


def list_ports() -> int:
    try:
        from serial.tools import list_ports
    except ImportError:
        print("pyserial is required: python -m pip install pyserial", file=sys.stderr)
        return 2
    for item in list_ports.comports():
        print(f"{item.device:8} {item.vid or 0:04x}:{item.pid or 0:04x} {item.description}")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "ports":
        return list_ports()
    if not args.port:
        print("--port is required (use 'datlink ports' to locate Gateway OTG CDC)",
              file=sys.stderr)
        return 2
    try:
        with DatlinkClient(args.port, timeout=args.timeout) as client:
            if args.command == "info":
                print(json.dumps(client.info(), indent=2))
            elif args.command == "link":
                linked = client.link()
                print("up" if linked else "down")
                return 0 if linked else 1
            elif args.command == "program":
                operation_id = args.operation_id or secrets.randbits(32) or 1
                manifest, image = protocol.encode_manifest(
                    operation_id, load_image(args.image, args.base))
                print(f"staging {len(image)} bytes, operation {operation_id:#010x}")
                client.stage(manifest, image)
                if not client.link():
                    raise RuntimeError("ESP-NOW link is down; target was not erased")
                client.start(operation_id)
                result = client.wait_result(args.wait)
                return 0 if result.status == 0 else 1
            elif args.command == "verify":
                data = client.request(protocol.USB_GET_PROGRESS)
                print(Progress.decode(data))
            elif args.command == "reset":
                client.request(protocol.USB_TARGET_RESET)
                print("reset requested")
            elif args.command == "abort":
                client.request(protocol.USB_PROGRAM_ABORT)
                print("abort requested")
            elif args.command == "target-info":
                client.request(protocol.USB_TARGET_READ_INFO)
                data = client.wait_event(protocol.MSG_TARGET_INFO)
                if len(data) != 28:
                    raise ValueError("invalid target-info payload")
                values = struct.unpack("<7I", data)
                names = ("dpidr", "ap_idr", "cpuid", "factory_device_id",
                         "factory_user_id", "factory_sramflash", "swd_clock_khz")
                print(json.dumps({name: f"0x{value:08x}" if name != "swd_clock_khz"
                                  else value for name, value in zip(names, values)}, indent=2))
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
