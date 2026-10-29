from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from run_ra4_ble_reconnect_peer_gate import Lines, validate


def evidence():
    return dict(writes=[1, 2, 3, 4], cycles=[1, 2, 3, 4], central_cycles=[1, 2, 3, 4],
                central_sessions=[1, 2], metadata_sessions=[1, 2], connected=2, disconnected=2, echoes=4,
                heaps=[(0, 33792, 49152)] + [(n, 32768, 47104) for n in range(1, 5)],
                skipped_scans=[])


class PeerGateTest(unittest.TestCase):
    def test_complete_two_sided_evidence(self):
        validate(evidence(), 2, 4)

    def test_partial_or_reordered_evidence_rejected(self):
        for key, value in (("disconnected", 1), ("echoes", 3),
                           ("writes", [1, 2, 4, 3]), ("central_cycles", [1, 2, 3]),
                           ("central_sessions", [1]), ("metadata_sessions", [1]), ("heaps", [(0, 1, 1)])):
            with self.subTest(key=key):
                data = evidence()
                data[key] = value
                with self.assertRaises(RuntimeError):
                    validate(data, 2, 4)

    def test_heap_loss_rejected(self):
        data = evidence()
        data["heaps"][-1] = (4, 32768, 46080)
        with self.assertRaisesRegex(RuntimeError, "Final heap"):
            validate(data, 2, 4)

    def test_scan_control_not_counted_as_scanning(self):
        data = evidence()
        with self.assertRaisesRegex(RuntimeError, "Scan control"):
            validate(data, 2, 4, no_wifi_scan=True)
        data["skipped_scans"] = [1, 2, 3, 4]
        validate(data, 2, 4, no_wifi_scan=True)
        with self.assertRaisesRegex(RuntimeError, "Scan control"):
            validate(data, 2, 4)

    def test_partial_serial_lines_preserved(self):
        class Serial:
            chunks = [b"ONE\r\nTW", b"O\r", b"\nTHREE\n"]

            @property
            def in_waiting(self):
                return len(self.chunks[0])

            def read(self, _count):
                return self.chunks.pop(0)

        lines = Lines(Serial())
        self.assertEqual(list(lines.read()), ["ONE"])
        self.assertEqual(list(lines.read()), [])
        self.assertEqual(list(lines.read()), ["TWO", "THREE"])


if __name__ == "__main__":
    unittest.main()
