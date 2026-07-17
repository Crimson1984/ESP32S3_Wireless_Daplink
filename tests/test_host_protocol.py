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
from datlink_cli.protocol import (UsbFrame, cobs_decode, cobs_encode, crc32c,
                                  encode_manifest)


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


if __name__ == "__main__":
    unittest.main()
