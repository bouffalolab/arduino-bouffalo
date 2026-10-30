import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from compare_ble_att_summary import compare, COLUMNS
from summarize_ble_etw_metadata import COLUMNS as META, TRANSPORT_COLUMNS


class ComparisonTest(unittest.TestCase):
    def setUp(self):
        self.metadata = ",".join(META) + "\n5000,8a1f9517-3a8c-4a9e-a018-4f17a200f277,402,0,0,4,4,19\n"
        self.transport = ",".join(TRANSPORT_COLUMNS) + "\n5000,1,4,3,11\n"
        self.att = ",".join(COLUMNS) + "\n5000,3,1,2,10,3,3853939470\n"
        self.capture = dict(schema=1, passed=True, origin_utc="1970-01-01T00:00:10Z",
                            origin_bracket_ms=0.1, events=1, events_lost=0, buffers_lost=0,
                            callback_errors=0, stop_result=0, process_result=0)
        self.peer = ("WINDOWS_BLE_WINRT:CLOCK_ORIGIN_UNIX_MS=10000\n"
                     "WINDOWS_BLE_WINRT:GET_SERVICES=5000\n"
                     "WINDOWS_BLE_WINRT:DISCOVERY_FAILURE=1000,1,1,242\nprivate name\n")
        self.board = ("BLE_COEX_HCI:LINK_TRACE=1,1,0,1,1\n"
                      "BLE_COEX_HCI:ATT_TRACE=R,10,3,3,1,3853939470\n"
                      "BLE_COEX_HCI:SNAPSHOT=1,0,10,0,0,0\n")

    def run_comparison(self, **changes):
        values = dict(metadata=self.metadata, transport=self.transport, att_csv=self.att,
                      capture=self.capture, peer=self.peer, board=self.board)
        return compare(**{**values, **changes})

    def test_full_numeric_match_and_mismatch_without_payload_or_direction_claim(self):
        result = self.run_comparison()
        self.assertTrue(result["candidates"][0]["complete"])
        self.assertNotIn("private", json.dumps(result))
        bad = self.run_comparison(att_csv=self.att.replace("3853939470", "1"))
        self.assertEqual(bad["candidates"][0]["matched_prefix"], 0)

    def test_disconnect_does_not_assign_old_trace_to_failed_session(self):
        result = self.run_comparison(board=self.board + "BLE_COEX_HCI:LINK_TRACE=2,2,0,1,19\n")
        self.assertFalse(result["last_board_link_is_connected"])
        self.assertEqual(result["candidates"], [])

    def test_reject_unknown_schema_corruption_or_missing_capture_integrity(self):
        for bad in [self.att.replace("fnv1a", "payload"), self.att.replace("5000,3", "5001,3"),
                    self.att.replace(",10,3,", ",10,4,"), self.att.replace(",1,2,", ",1,1,"),
                    self.att + "5000,3,1,2,10,3,3853939470\n"]:
            with self.assertRaises(ValueError):
                self.run_comparison(att_csv=bad)
        with self.assertRaises(ValueError):
            self.run_comparison(capture={**self.capture, "events_lost": 1})


if __name__ == "__main__":
    unittest.main()
