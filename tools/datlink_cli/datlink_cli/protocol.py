from __future__ import annotations

import struct
from dataclasses import dataclass

VERSION = 2
USB_PAYLOAD_MAX = 4096
TARGET_MSPM0G3507 = 0x3507

USB_GET_INFO = 1
USB_GET_LINK_STATUS = 2
USB_IMAGE_BEGIN = 16
USB_IMAGE_DATA = 17
USB_IMAGE_END = 18
USB_PROGRAM_START = 19
USB_PROGRAM_ABORT = 20
USB_GET_PROGRESS = 21
USB_TARGET_RESET = 22
USB_TARGET_READ_INFO = 23
USB_LOADER_TEST = 24
USB_TARGET_BACKUP_START = 25
USB_TRANSPORT_RECOVER = 26
USB_RESPONSE = 0x80
USB_EVENT = 0x81

MSG_PROGRAM_PROGRESS = 21
MSG_PROGRAM_RESULT = 22
MSG_TARGET_INFO = 24
MSG_LOADER_TEST = 25
MSG_TARGET_BACKUP_DATA = 27
MSG_TARGET_BACKUP_RESULT = 28
MSG_COMMAND_ERROR = 29

BACKUP_MAIN_SIZE = 0x20000
BACKUP_DATA_MAX = 180
BACKUP_RESULT_LEN = 60
COMMAND_ERROR_LEN = 20


def crc32c(data: bytes, seed: int = 0) -> int:
    crc = (~seed) & 0xFFFFFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return (~crc) & 0xFFFFFFFF


def cobs_encode(data: bytes) -> bytes:
    output = bytearray(b"\x00")
    code_index = 0
    code = 1
    for value in data:
        if value == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(value)
            code += 1
            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1
    output[code_index] = code
    return bytes(output)


def cobs_decode(data: bytes) -> bytes:
    output = bytearray()
    offset = 0
    while offset < len(data):
        code = data[offset]
        if code == 0 or offset + code > len(data) + 1:
            raise ValueError("invalid COBS frame")
        offset += 1
        end = offset + code - 1
        if end > len(data):
            raise ValueError("truncated COBS frame")
        output.extend(data[offset:end])
        offset = end
        if code != 0xFF and offset < len(data):
            output.append(0)
    return bytes(output)


@dataclass(frozen=True)
class UsbFrame:
    type: int
    flags: int
    request_id: int
    payload: bytes

    def encode(self) -> bytes:
        if len(self.payload) > USB_PAYLOAD_MAX:
            raise ValueError("USB payload exceeds 4096 bytes")
        header = struct.pack("<BBHII", VERSION, self.type, self.flags,
                             self.request_id, len(self.payload))
        raw = header + self.payload
        return cobs_encode(raw + struct.pack("<I", crc32c(raw))) + b"\x00"

    @classmethod
    def decode(cls, encoded: bytes) -> "UsbFrame":
        raw = cobs_decode(encoded)
        if len(raw) < 16:
            raise ValueError("short USB frame")
        version, frame_type, flags, request_id, length = struct.unpack_from("<BBHII", raw)
        if version != VERSION:
            raise ValueError(
                f"protocol version mismatch: gateway={version}, CLI={VERSION}; "
                "flash matching Gateway and Probe v2 firmware")
        if length > USB_PAYLOAD_MAX or len(raw) != 16 + length:
            raise ValueError("invalid USB frame header")
        expected, = struct.unpack_from("<I", raw, 12 + length)
        if crc32c(raw[:12 + length]) != expected:
            raise ValueError("USB CRC32C mismatch")
        return cls(frame_type, flags, request_id, raw[12:12 + length])


def encode_manifest(operation_id: int, segments: list[tuple[int, bytes]]) -> tuple[bytes, bytes]:
    import hashlib

    if not 1 <= len(segments) <= 8:
        raise ValueError("manifest supports 1..8 segments")
    ordered = sorted(segments, key=lambda item: item[0])
    image = bytearray()
    records = bytearray()
    previous_end = 0
    for index, (address, data) in enumerate(ordered):
        if not data or address < 0 or address & 7 or address + len(data) > 0x20000:
            raise ValueError("segment is outside MSPM0G3507 MAIN flash")
        if index and address < previous_end:
            raise ValueError("overlapping segments")
        records += struct.pack("<IIII", address, len(data), len(image), crc32c(data))
        image += data
        previous_end = address + len(data)
    if not operation_id:
        raise ValueError("operation_id must be non-zero")
    header = struct.pack("<HHIII", 1, len(ordered), TARGET_MSPM0G3507,
                         operation_id, len(image))
    return header + hashlib.sha256(image).digest() + records, bytes(image)


def decode_status_payload(payload: bytes) -> tuple[int, bytes]:
    if len(payload) < 4:
        raise ValueError("response has no status")
    status, = struct.unpack_from("<i", payload)
    return status, payload[4:]


def decode_command_error(payload: bytes) -> tuple[int, int, int, int, int]:
    if len(payload) != COMMAND_ERROR_LEN:
        raise ValueError("invalid COMMAND_ERROR payload")
    session, sequence, message_type, status, detail = struct.unpack(
        "<IIB3xiI", payload)
    return session, sequence, message_type, status, detail


def decode_backup_data(payload: bytes) -> tuple[int, int, bytes]:
    if len(payload) < 10:
        raise ValueError("short backup data event")
    operation_id, offset, length = struct.unpack_from("<IIH", payload)
    if not operation_id or not 1 <= length <= BACKUP_DATA_MAX or len(payload) != 10 + length:
        raise ValueError("invalid backup data event")
    return operation_id, offset, payload[10:]


def decode_backup_result(
        payload: bytes) -> tuple[int, int, int, bytes, tuple[int, int, int, int]]:
    if len(payload) != BACKUP_RESULT_LEN:
        raise ValueError("invalid backup result event")
    operation_id, status, total = struct.unpack_from("<IiI", payload)
    if not operation_id:
        raise ValueError("backup result has zero operation ID")
    diagnostic = struct.unpack_from("<4I", payload, 44)
    return operation_id, status, total, payload[12:44], diagnostic
