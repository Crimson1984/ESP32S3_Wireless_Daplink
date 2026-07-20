import hashlib
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "datlink_cli"))

from datlink_cli.image import load_image, parse_ihex
from datlink_cli.cli import decode_target_result
from datlink_cli.client import DatlinkClient, Progress
from datlink_cli.protocol import (UsbFrame, cobs_decode, cobs_encode, crc32c,
                                  decode_backup_data, decode_backup_result,
                                  encode_manifest)


class FakeSerial:
    def __init__(self):
        self.writes: list[bytes] = []
        self.flushes = 0

    def write(self, data: bytes) -> None:
        self.writes.append(data)

    def flush(self) -> None:
        self.flushes += 1


class ProtocolTests(unittest.TestCase):
    def test_crc32c_standard_vector(self):
        self.assertEqual(crc32c(b"123456789"), 0xE3069283)

    def test_cobs_round_trip(self):
        vectors = [b"", b"\x00", b"abc", b"a\x00b", bytes(range(256)), os.urandom(4096)]
        for vector in vectors:
            self.assertEqual(cobs_decode(cobs_encode(vector)), vector)

    def test_usb_round_trip_and_crc_rejection(self):
        encoded = UsbFrame(16, 0x1234, 99, b"abc\x00def").encode()
        decoded = UsbFrame.decode(encoded[:-1])
        self.assertEqual(decoded.payload, b"abc\x00def")
        corrupt = bytearray(encoded[:-1])
        corrupt[-1] ^= 1
        with self.assertRaises(ValueError):
            UsbFrame.decode(bytes(corrupt))

    def test_manifest_layout(self):
        operation = 0x12345678
        manifest, image = encode_manifest(operation, [(0x100, b"B" * 8), (0, b"A" * 16)])
        version, count, target, decoded_operation, total = struct.unpack_from("<HHIII", manifest)
        self.assertEqual((version, count, target, decoded_operation, total),
                         (1, 2, 0x3507, operation, 24))
        self.assertEqual(manifest[16:48], hashlib.sha256(image).digest())
        self.assertEqual(image, b"A" * 16 + b"B" * 8)

    def test_manifest_rejects_overlap_and_nonmain(self):
        with self.assertRaises(ValueError):
            encode_manifest(1, [(0, b"A" * 8), (4, b"B" * 8)])
        with self.assertRaises(ValueError):
            encode_manifest(1, [(0x1FFFF, b"AB")])

    def test_ihex_and_bin(self):
        # Extended linear address 0, four data bytes, EOF.
        lines = [":020000040000FA", ":0400100001020304E2", ":00000001FF"]
        with tempfile.TemporaryDirectory() as directory:
            hex_path = Path(directory) / "test.hex"
            hex_path.write_text("\n".join(lines), encoding="ascii")
            self.assertEqual(parse_ihex(hex_path), [(0x10, b"\x01\x02\x03\x04")])
            bin_path = Path(directory) / "test.bin"
            bin_path.write_bytes(b"\x11\x22")
            self.assertEqual(load_image(str(bin_path), 0x80), [(0x80, b"\x11\x22")])

    def test_target_operation_result(self):
        values = (0x6BA02477, 0x84770001, 0x410CC601, 0x2BB8802F,
                  0x80C7AE2D, 0x00200080, 1000)
        result = decode_target_result(struct.pack("<i7I", 0, *values), "loader")
        self.assertEqual(result["dpidr"], "0x6ba02477")
        self.assertEqual(result["swd_clock_khz"], 1000)

        failure = struct.pack("<i4I", -14, 14, values[0], values[1], 1000)
        with self.assertRaisesRegex(RuntimeError, "loader_execute"):
            decode_target_result(failure, "loader")

    def test_backup_event_layouts(self):
        operation = 0x12345678
        chunk = bytes(range(180))
        decoded = decode_backup_data(struct.pack("<IIH", operation, 0x240, len(chunk)) + chunk)
        self.assertEqual(decoded, (operation, 0x240, chunk))
        with self.assertRaises(ValueError):
            decode_backup_data(struct.pack("<IIH", operation, 0, 181) + bytes(181))

        digest = hashlib.sha256(b"backup").digest()
        payload = (struct.pack("<IiI", operation, 0, 0x20000) + digest +
                   struct.pack("<4I", 0, 0x6BA02477, 0x84770001, 1000))
        result = decode_backup_result(payload)
        self.assertEqual(result[:4], (operation, 0, 0x20000, digest))
        self.assertEqual(result[4], (0, 0x6BA02477, 0x84770001, 1000))

    def test_request_ignores_one_serial_poll_timeout(self):
        client = DatlinkClient.__new__(DatlinkClient)
        client.serial = FakeSerial()
        client.timeout = 0.2
        client.request_id = 41
        client.pending_events = []
        calls = 0

        def read_frame(_timeout: float) -> UsbFrame:
            nonlocal calls
            calls += 1
            if calls == 1:
                raise TimeoutError("serial receive timeout")
            return UsbFrame(0x80, 0, client.request_id, struct.pack("<i", 0) + b"ok")

        client._read_frame = read_frame
        self.assertEqual(client.request(1), b"ok")
        self.assertEqual(calls, 2)
        self.assertEqual(client.serial.flushes, 1)

    def test_wait_event_uses_operation_deadline_not_poll_timeout(self):
        client = DatlinkClient.__new__(DatlinkClient)
        client.pending_events = []
        event = UsbFrame(
            0x81, 0, 0,
            struct.pack("<iB", 0, 24) + b"target-result")
        reads = iter((TimeoutError("serial receive timeout"), event))

        def read_frame(_timeout: float) -> UsbFrame:
            value = next(reads)
            if isinstance(value, BaseException):
                raise value
            return value

        client._read_frame = read_frame
        self.assertEqual(client.wait_event(24, timeout=0.2), b"target-result")

    def test_wait_result_uses_operation_deadline_not_poll_timeout(self):
        client = DatlinkClient.__new__(DatlinkClient)
        client.pending_events = []
        progress_payload = struct.pack("<iB3xIII", 0, 7, 128, 128, 0)
        event = UsbFrame(
            0x81, 0, 0,
            struct.pack("<iB", 0, 22) + progress_payload)
        reads = iter((TimeoutError("serial receive timeout"), event))

        def read_frame(_timeout: float) -> UsbFrame:
            value = next(reads)
            if isinstance(value, BaseException):
                raise value
            return value

        client._read_frame = read_frame
        self.assertEqual(client.wait_result(timeout=0.2),
                         Progress(0, 7, 128, 128, 0))


if __name__ == "__main__":
    unittest.main()
