from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
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
    link = sub.add_parser("link")
    link.add_argument("--json", action="store_true", help="show protocol v2 recovery details")
    sub.add_parser("recover", help="rebuild both ESP-NOW sequence epochs without resetting MSPM0")
    program = sub.add_parser("program")
    program.add_argument("image")
    program.add_argument("--base", type=parse_int)
    program.add_argument("--operation-id", type=parse_int)
    program.add_argument("--wait", type=float, default=120.0)
    sub.add_parser("verify", help="show latest programmer progress/result")
    sub.add_parser("reset")
    sub.add_parser("abort")
    sub.add_parser("target-info")
    sub.add_parser("loader-test", help="upload and execute the non-destructive SRAM loader probe")
    backup = sub.add_parser("backup", help="read and verify the complete 128 KiB MAIN flash")
    backup.add_argument("output")
    backup.add_argument("--passes", type=int, default=2,
                        help="complete reads that must match (default: 2)")
    backup.add_argument("--wait", type=float, default=180.0,
                        help="timeout for each pass in seconds")
    backup.add_argument("--force", action="store_true",
                        help="replace an existing output file")
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


STATUS_NAMES = {
    -1: "argument", -2: "state", -3: "crc", -4: "range",
    -5: "timeout", -6: "link", -7: "storage",
    -8: "target_power/VTref", -9: "swd_ack_wait",
    -10: "swd_ack_fault", -11: "swd_parity", -12: "target_id",
    -13: "target_locked", -14: "loader", -15: "verify", -16: "aborted",
    -17: "protocol_version",
}

STAGE_NAMES = {
    1: "vtref", 2: "dpidr", 3: "abort_clear", 4: "dp_select",
    5: "power_request", 6: "power_ack", 7: "ap_idr", 8: "ap_csw",
    9: "halt", 10: "identify", 11: "sram_test", 12: "reset",
    13: "loader_upload", 14: "loader_execute",
}


def decode_target_result(data: bytes, operation: str) -> dict[str, object]:
    if len(data) < 4:
        raise ValueError(f"invalid {operation} result")
    status, = struct.unpack_from("<i", data)
    if status != 0:
        detail = ""
        if len(data) >= 20:
            stage, dpidr, ap_idr, clock = struct.unpack_from("<4I", data, 4)
            detail = (f", stage={STAGE_NAMES.get(stage, 'unknown')}"
                      f", swd={clock}kHz, dpidr=0x{dpidr:08x},"
                      f" ap_idr=0x{ap_idr:08x}")
        raise RuntimeError(
            f"{operation} failed: {STATUS_NAMES.get(status, 'unknown')}"
            f" ({status}){detail}")
    if len(data) != 4 + 28:
        raise ValueError(f"invalid {operation} payload")
    values = struct.unpack_from("<7I", data, 4)
    names = ("dpidr", "ap_idr", "cpuid", "factory_device_id",
             "factory_user_id", "factory_sramflash", "swd_clock_khz")
    return {name: f"0x{value:08x}" if name != "swd_clock_khz" else value
            for name, value in zip(names, values)}


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
                status = client.link_status()
                if args.json:
                    print(json.dumps(status, indent=2))
                else:
                    suffix = " (recovering)" if status["recovering"] else ""
                    if status["last_error"] == -17:
                        suffix = " (protocol version mismatch; update Gateway and Probe together)"
                    print(("up" if status["up"] else "down") + suffix)
                return 0 if status["up"] else 1
            elif args.command == "recover":
                print(json.dumps(client.recover(), indent=2))
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
                try:
                    client.request(protocol.USB_TARGET_READ_INFO)
                    data = client.wait_event(protocol.MSG_TARGET_INFO)
                except TimeoutError:
                    client.recover()
                    client.request(protocol.USB_TARGET_READ_INFO)
                    data = client.wait_event(protocol.MSG_TARGET_INFO)
                print(json.dumps(decode_target_result(data, "target probe"), indent=2))
            elif args.command == "loader-test":
                try:
                    client.request(protocol.USB_LOADER_TEST)
                    data = client.wait_event(protocol.MSG_LOADER_TEST)
                except TimeoutError:
                    client.recover()
                    client.request(protocol.USB_LOADER_TEST)
                    data = client.wait_event(protocol.MSG_LOADER_TEST)
                result = decode_target_result(data, "SRAM loader test")
                result["loader_test"] = "passed"
                result["flash_modified"] = False
                print(json.dumps(result, indent=2))
            elif args.command == "backup":
                if args.passes < 2:
                    raise ValueError("backup requires at least two verification passes")
                output = Path(args.output).expanduser().resolve()
                if output.exists() and not args.force:
                    raise ValueError(f"backup already exists: {output} (use --force to replace)")
                first_data: bytes | None = None
                first_sha: bytes | None = None
                for pass_index in range(1, args.passes + 1):
                    operation_id = secrets.randbits(32) or 1
                    printed = -8192

                    def show_progress(completed: int, total: int) -> None:
                        nonlocal printed
                        if completed == total or completed - printed >= 8192:
                            print(f"pass {pass_index}/{args.passes}: "
                                  f"{completed}/{total} bytes", flush=True)
                            printed = completed

                    result = client.backup_main(operation_id, args.wait, show_progress)
                    print(f"pass {pass_index}/{args.passes}: SHA-256 "
                          f"{result.sha256.hex()} @ {result.swd_clock_khz}kHz")
                    if first_data is None:
                        first_data, first_sha = result.data, result.sha256
                    elif result.sha256 != first_sha or result.data != first_data:
                        raise ValueError("backup passes differ; no output file was finalized")

                assert first_data is not None and first_sha is not None
                output.parent.mkdir(parents=True, exist_ok=True)
                partial = output.with_name(output.name + ".partial")
                partial.write_bytes(first_data)
                os.replace(partial, output)
                print(json.dumps({
                    "output": str(output),
                    "length": len(first_data),
                    "sha256": first_sha.hex(),
                    "passes": args.passes,
                    "flash_modified": False,
                }, indent=2))
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
