from __future__ import annotations

import json
import hashlib
import secrets
import struct
import time
from dataclasses import dataclass
from typing import Callable

from . import protocol


@dataclass(frozen=True)
class Progress:
    status: int
    phase: int
    completed: int
    total: int
    detail: int

    @classmethod
    def decode(cls, data: bytes) -> "Progress":
        if len(data) != 20:
            raise ValueError("invalid progress payload")
        status, phase, completed, total, detail = struct.unpack("<iB3xIII", data)
        return cls(status, phase, completed, total, detail)


@dataclass(frozen=True)
class BackupResult:
    data: bytes
    sha256: bytes
    swd_clock_khz: int


class RemoteCommandError(RuntimeError):
    def __init__(self, payload: bytes):
        session, sequence, message_type, status, detail = \
            protocol.decode_command_error(payload)
        super().__init__(
            f"remote rejected message {message_type} at session=0x{session:08x} "
            f"sequence={sequence}: status={status}, detail=0x{detail:08x}")
        self.message_type = message_type
        self.status = status


class DatlinkClient:
    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 3.0):
        try:
            import serial
        except ImportError as error:
            raise RuntimeError("pyserial is required: python -m pip install pyserial") from error
        self.serial = serial.Serial(port, baudrate=baudrate, timeout=0.1,
                                    write_timeout=timeout)
        # usbser.sys may retain a completed response across close/open cycles.
        # Drop stale input and start from a non-repeating request ID so an old
        # frame cannot be mistaken for the first request of a new CLI process.
        self.serial.reset_input_buffer()
        self.timeout = timeout
        self.request_id = secrets.randbits(32)
        self.pending_events: list[tuple[int, bytes]] = []

    def close(self) -> None:
        self.serial.close()

    def __enter__(self) -> "DatlinkClient":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def _read_frame(self, timeout: float | None = None) -> protocol.UsbFrame:
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        encoded = bytearray()
        while time.monotonic() < deadline:
            value = self.serial.read(1)
            if not value:
                continue
            if value == b"\x00":
                if encoded:
                    return protocol.UsbFrame.decode(bytes(encoded))
            elif len(encoded) < 4200:
                encoded += value
            else:
                encoded.clear()
        raise TimeoutError("serial receive timeout")

    def request(self, command: int, payload: bytes = b"", timeout: float | None = None) -> bytes:
        self.request_id = (self.request_id + 1) & 0xFFFFFFFF or 1
        frame = protocol.UsbFrame(command, 0, self.request_id, payload)
        self.serial.write(frame.encode())
        self.serial.flush()
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            try:
                response = self._read_frame(max(0.01, deadline - time.monotonic()))
            except TimeoutError:
                continue
            if response.type == protocol.USB_EVENT:
                status, event = protocol.decode_status_payload(response.payload)
                if status == 0 and event:
                    if event[0] == protocol.MSG_COMMAND_ERROR:
                        raise RemoteCommandError(event[1:])
                    self.pending_events.append((event[0], event[1:]))
                continue
            if response.type != protocol.USB_RESPONSE or response.request_id != self.request_id:
                continue
            status, data = protocol.decode_status_payload(response.payload)
            if status != 0:
                raise RuntimeError(f"gateway command failed: {status:#x}")
            return data
        raise TimeoutError(
            "gateway response timeout; if the CDC port is correct, flash matching "
            "protocol v2 Gateway/Probe firmware (v1 does not answer v2 USB frames)")

    def info(self) -> dict[str, object]:
        return json.loads(self.request(protocol.USB_GET_INFO).decode("utf-8"))

    def link(self) -> bool:
        return bool(self.link_status()["up"])

    def link_status(self) -> dict[str, int | bool]:
        data = self.request(protocol.USB_GET_LINK_STATUS)
        if len(data) != 40:
            raise ValueError("invalid protocol v2 link status payload")
        local, peer, next_tx, rx_base = struct.unpack_from("<4I", data, 4)
        tx_pending, rx_pending = struct.unpack_from("<2H", data, 20)
        head_age, last_error, recoveries = struct.unpack_from("<IiI", data, 28)
        return {
            "up": bool(data[0]), "recovering": bool(data[1]),
            "local_session": local, "peer_session": peer,
            "next_tx_sequence": next_tx, "rx_base": rx_base,
            "tx_pending": tx_pending, "rx_pending": rx_pending,
            "head_state": data[24], "head_age_ms": head_age,
            "last_error": last_error, "recovery_count": recoveries,
        }

    def recover(self, timeout: float = 6.0) -> dict[str, int | bool]:
        before = self.link_status()["recovery_count"]
        self.request(protocol.USB_TRANSPORT_RECOVER, timeout=2.0)
        deadline = time.monotonic() + timeout
        status: dict[str, int | bool] = {}
        while time.monotonic() < deadline:
            time.sleep(0.2)
            status = self.link_status()
            if status["up"] and not status["recovering"] and \
                    status["recovery_count"] != before:
                return status
        raise TimeoutError(f"transport recovery did not complete: {status}")

    def stage(self, manifest: bytes, image: bytes) -> None:
        self.request(protocol.USB_IMAGE_BEGIN, manifest, 10.0)
        for offset in range(0, len(image), 1024):
            data = image[offset:offset + 1024]
            self.request(protocol.USB_IMAGE_DATA, struct.pack("<I", offset) + data, 10.0)
        self.request(protocol.USB_IMAGE_END, timeout=30.0)

    def start(self, operation_id: int) -> None:
        self.request(protocol.USB_PROGRAM_START, struct.pack("<I", operation_id), 5.0)

    def wait_result(self, timeout: float = 120.0) -> Progress:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.pending_events:
                event_type, data = self.pending_events.pop(0)
            else:
                try:
                    frame = self._read_frame(min(2.0, deadline - time.monotonic()))
                except TimeoutError:
                    # A SWD operation can legitimately take longer than one
                    # serial read interval. Keep waiting until the operation's
                    # overall deadline instead of reporting a Gateway failure.
                    continue
                if frame.type != protocol.USB_EVENT:
                    continue
                status, event = protocol.decode_status_payload(frame.payload)
                if status != 0 or not event:
                    continue
                event_type, data = event[0], event[1:]
            if event_type in (protocol.MSG_PROGRAM_PROGRESS, protocol.MSG_PROGRAM_RESULT):
                progress = Progress.decode(data)
                print(f"phase={progress.phase} {progress.completed}/{progress.total} "
                      f"status={progress.status} detail=0x{progress.detail:08x}")
                if event_type == protocol.MSG_PROGRAM_RESULT:
                    return progress
            if event_type == protocol.MSG_COMMAND_ERROR:
                raise RemoteCommandError(data)
        raise TimeoutError("program operation did not finish")

    def wait_event(self, expected_type: int, timeout: float = 10.0) -> bytes:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for index, (event_type, data) in enumerate(self.pending_events):
                if event_type == protocol.MSG_COMMAND_ERROR:
                    self.pending_events.pop(index)
                    raise RemoteCommandError(data)
                if event_type == expected_type:
                    self.pending_events.pop(index)
                    return data
            try:
                frame = self._read_frame(min(2.0, deadline - time.monotonic()))
            except TimeoutError:
                # The 2-second read is only a polling interval. TARGET_INFO
                # and loader operations have their own, longer deadline.
                continue
            if frame.type != protocol.USB_EVENT:
                continue
            status, event = protocol.decode_status_payload(frame.payload)
            if status == 0 and event:
                if event[0] == protocol.MSG_COMMAND_ERROR:
                    raise RemoteCommandError(event[1:])
                if event[0] == expected_type:
                    return event[1:]
                self.pending_events.append((event[0], event[1:]))
        raise TimeoutError(f"event {expected_type} did not arrive")

    def backup_main(self, operation_id: int, timeout: float = 180.0,
                    progress: Callable[[int, int], None] | None = None) -> BackupResult:
        if not operation_id:
            raise ValueError("backup operation ID must be non-zero")
        self.request(protocol.USB_TARGET_BACKUP_START,
                     struct.pack("<I", operation_id), 5.0)
        deadline = time.monotonic() + timeout
        image = bytearray()
        while time.monotonic() < deadline:
            if self.pending_events:
                event_type, payload = self.pending_events.pop(0)
            else:
                try:
                    frame = self._read_frame(min(2.0, deadline - time.monotonic()))
                except TimeoutError:
                    continue
                if frame.type != protocol.USB_EVENT:
                    continue
                status, event = protocol.decode_status_payload(frame.payload)
                if status != 0 or not event:
                    continue
                event_type, payload = event[0], event[1:]

            if event_type == protocol.MSG_TARGET_BACKUP_DATA:
                received_operation, offset, data = protocol.decode_backup_data(payload)
                if received_operation != operation_id:
                    continue
                if offset < len(image):
                    end = offset + len(data)
                    if end <= len(image) and image[offset:end] == data:
                        # A USB flush may report failure after bytes reached
                        # usbser.sys. The reliable radio layer then repeats the
                        # event; accept only an exact duplicate.
                        continue
                    raise ValueError(f"conflicting duplicate backup data at {offset:#x}")
                if offset != len(image) or len(image) + len(data) > protocol.BACKUP_MAIN_SIZE:
                    raise ValueError(
                        f"backup data is not contiguous: expected {len(image):#x}, got {offset:#x}")
                image.extend(data)
                if progress is not None:
                    progress(len(image), protocol.BACKUP_MAIN_SIZE)
                continue

            if event_type == protocol.MSG_COMMAND_ERROR:
                raise RemoteCommandError(payload)

            if event_type != protocol.MSG_TARGET_BACKUP_RESULT:
                continue
            received_operation, status, total, remote_sha, diagnostic = \
                protocol.decode_backup_result(payload)
            if received_operation != operation_id:
                continue
            stage, dpidr, ap_idr, clock = diagnostic
            if status != 0:
                raise RuntimeError(
                    f"target backup failed: status={status}, stage={stage},"
                    f" swd={clock}kHz, dpidr=0x{dpidr:08x}, ap_idr=0x{ap_idr:08x},"
                    f" received={total}")
            if total != protocol.BACKUP_MAIN_SIZE or len(image) != total:
                raise ValueError(
                    f"backup length mismatch: event={total}, received={len(image)}")
            local_sha = hashlib.sha256(image).digest()
            if local_sha != remote_sha:
                raise ValueError(
                    f"backup SHA-256 mismatch: probe={remote_sha.hex()}, host={local_sha.hex()}")
            return BackupResult(bytes(image), local_sha, clock)
        raise TimeoutError("target backup did not finish")
