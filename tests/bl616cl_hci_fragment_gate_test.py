from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from run_bl616cl_hci_fragment_gate import Events

BODY = bytes((2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 3, 2, 1, 6, 192))
ADVERTISEMENT = bytes((4, 0x3e, len(BODY))) + BODY
COMPLETE = bytes((4, 0x0e, 10, 1, 9, 0x10, 0)) + bytes(6)


class FragmentParserTest(unittest.TestCase):
    def test_every_split_and_bytewise_stream(self):
        stream = ADVERTISEMENT + COMPLETE
        for split in range(1, len(stream)):
            parser = Events()
            parser.feed(stream[:split]); parser.feed(stream[split:])
            self.assertEqual(parser.reports, 1)
            self.assertEqual(parser.completions, [(0x1009, 0)])
            self.assertFalse(parser.buffer)
        parser = Events()
        for byte in stream:
            parser.feed(bytes((byte,)))
        self.assertEqual(parser.reports, 1)
        self.assertFalse(parser.buffer)

    def test_interleaving_control_inside_report_rejected(self):
        for split in range(4, len(ADVERTISEMENT)):
            with self.subTest(split=split), self.assertRaises(RuntimeError):
                Events().feed(ADVERTISEMENT[:split] + COMPLETE + ADVERTISEMENT[split:])

    def test_partial_frame_and_batched_reports(self):
        parser = Events()
        parser.feed(ADVERTISEMENT[:4])
        self.assertTrue(parser.partial_advertisement())
        parser.feed(ADVERTISEMENT[4:])
        self.assertFalse(parser.partial_advertisement())
        body = bytes((2, 2)) + BODY[2:] + BODY[2:]
        parser.feed(bytes((4, 0x3e, len(body))) + body)
        self.assertEqual(parser.reports, 3)

    def test_malformed_lengths_and_unexpected_event_rejected(self):
        for packet in (b"\x04\x3e\x02\x02\x01", b"\x04\x0e\x01\x00",
                       b"\x04\x10\x01\x01", b"\x04\x3e\x03\x02\x00\x00"):
            with self.subTest(packet_length=len(packet)), self.assertRaises(RuntimeError):
                Events().feed(packet)


if __name__ == "__main__":
    unittest.main()
