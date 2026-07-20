"""Deterministic reference test for ACK-base/bitmap behavior under radio faults."""

import random
import unittest


class ReliableModel:
    def __init__(self, payload: bytes, seed: int):
        self.random = random.Random(seed)
        self.frames = [payload[offset:offset + 192] for offset in range(0, len(payload), 192)]
        self.pending = {sequence + 1: data for sequence, data in enumerate(self.frames)}
        self.received: dict[int, bytes] = {}
        self.base = 0

    def ack(self) -> tuple[int, int]:
        bitmap = 0
        for sequence in self.received:
            if self.base < sequence <= self.base + 32:
                bitmap |= 1 << (sequence - self.base - 1)
        return self.base, bitmap

    def deliver(self, sequence: int, data: bytes):
        self.received.setdefault(sequence, data)
        while self.base + 1 in self.received:
            self.base += 1

    def run(self) -> bytes:
        retries = {sequence: 0 for sequence in self.pending}
        while self.pending:
            window = sorted(self.pending)[:4]
            transmissions = []
            for sequence in window:
                retries[sequence] += 1
                if retries[sequence] > 8:
                    raise AssertionError("reference link exceeded retry budget")
                if self.random.random() >= 0.05:
                    transmissions.append((sequence, self.pending[sequence]))
                    if self.random.random() < 0.05:
                        transmissions.append((sequence, self.pending[sequence]))
            self.random.shuffle(transmissions)
            for sequence, data in transmissions:
                self.deliver(sequence, data)
            ack_base, bitmap = self.ack()
            for sequence in list(self.pending):
                if sequence <= ack_base or (
                    sequence <= ack_base + 32 and bitmap & (1 << (sequence - ack_base - 1))
                ):
                    del self.pending[sequence]
        return b"".join(self.received[index] for index in range(1, len(self.frames) + 1))


class ReliableTransferTests(unittest.TestCase):
    def test_128k_fifty_times_with_loss_duplicate_reorder(self):
        payload = bytes((index * 17 + 31) & 0xFF for index in range(128 * 1024))
        for seed in range(50):
            self.assertEqual(ReliableModel(payload, seed).run(), payload)

    def test_old_session_cannot_replace_live_session(self):
        peer_session = 0
        link_up = False

        def accept(session: int, kind: str) -> bool:
            nonlocal peer_session, link_up
            if peer_session == session:
                link_up = True
                return True
            if kind == "HELLO" and (peer_session == 0 or not link_up):
                peer_session = session
                link_up = True
                return True
            return False

        self.assertTrue(accept(0x1111, "HELLO"))
        self.assertFalse(accept(0x2222, "DATA"))
        self.assertFalse(accept(0x2222, "HELLO"))
        self.assertEqual(peer_session, 0x1111)
        link_up = False  # Three-second link timeout.
        self.assertTrue(accept(0x2222, "HELLO"))
        self.assertFalse(accept(0x1111, "DATA"))

    def test_retry_exhaustion_must_not_create_sequence_gap(self):
        received: dict[int, bytes] = {}
        base = 0
        retained = {1: b"first", 2: b"second"}

        # Simulate an outage longer than the eight fast retries. The sender
        # keeps sequence 1 for slower recovery instead of deleting it.
        retries = 0
        for _ in range(12):
            retries += 1
            self.assertIn(1, retained)
        self.assertGreater(retries, 8)
        for sequence in (2, 1, 2):
            received.setdefault(sequence, retained[sequence])
            while base + 1 in received:
                base += 1

        self.assertEqual(base, 2)
        self.assertEqual(b"".join(received[index] for index in range(1, base + 1)),
                         b"firstsecond")


if __name__ == "__main__":
    unittest.main()
