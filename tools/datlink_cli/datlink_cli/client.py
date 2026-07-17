from __future__ import annotations

import json
import struct
import time
from dataclasses import dataclass

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


class DatlinkClient:
    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 3.0):
        try:
            import serial
        except ImportError as error:
            raise RuntimeError("pyserial is required: python -m pip install pyserial") from error
        self.serial = serial.Serial(port, baudrate=baudrate, timeout=0.1,
                                    write_timeout=timeout)
        self.timeout = timeout
        self.request_id = 0
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
        raise TimeoutError("gateway did not respond")

    def request(self, command: int, payload: bytes = b"", timeout: float | None = None) -> bytes:
        self.request_id = (self.request_id + 1) & 0xFFFFFFFF or 1
        frame = protocol.UsbFrame(command, 0, self.request_id, payload)
        self.serial.write(frame.encode())
        self.serial.flush()
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            response = self._read_frame(max(0.01, deadline - time.monotonic()))
            if response.type == protocol.USB_EVENT:
                status, event = protocol.decode_status_payload(response.payload)
                if status == 0 and event:
                    self.pending_events.append((event[0], event[1:]))
                continue
            if response.type != protocol.USB_RESPONSE or response.request_id != self.request_id:
                continue
            status, data = protocol.decode_status_payload(response.payload)
            if status != 0:
                raise RuntimeError(f"gateway command failed: {status:#x}")
            return data
        raise TimeoutError("gateway response timeout")

    def info(self) -> dict[str, object]:
        return json.loads(self.request(protocol.USB_GET_INFO).decode("utf-8"))

    def link(self) -> bool:
        data = self.request(protocol.USB_GET_LINK_STATUS)
        return bool(data and data[0])

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
                frame = self._read_frame(min(2.0, deadline - time.monotonic()))
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
        raise TimeoutError("program operation did not finish")

    def wait_event(self, expected_type: int, timeout: float = 10.0) -> bytes:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for index, (event_type, data) in enumerate(self.pending_events):
                if event_type == expected_type:
                    self.pending_events.pop(index)
                    return data
            frame = self._read_frame(min(2.0, deadline - time.monotonic()))
            if frame.type != protocol.USB_EVENT:
                continue
            status, event = protocol.decode_status_payload(frame.payload)
            if status == 0 and event:
                if event[0] == expected_type:
                    return event[1:]
                self.pending_events.append((event[0], event[1:]))
        raise TimeoutError(f"event {expected_type} did not arrive")
