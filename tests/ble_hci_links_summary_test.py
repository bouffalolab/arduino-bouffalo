import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from summarize_ble_hci_links import summarize_links, COLUMNS
from summarize_ble_etw_metadata import COLUMNS as META, TRANSPORT_COLUMNS


class LinkSummaryTest(unittest.TestCase):
    def setUp(self):
        self.metadata = ",".join(META) + "\n5000,8a1f9517-3a8c-4a9e-a018-4f17a200f277,402,0,0,4,4,41\n"
        self.transport = ",".join(TRANSPORT_COLUMNS) + "\n5000,1,4,2,33\n"
        self.links = ",".join(COLUMNS) + "\n5000,2,62,10,0,1,0,48,0,960\n"
        self.capture = dict(schema=1, passed=True, origin_utc="1970-01-01T00:00:10Z",
                            origin_bracket_ms=.1, events=1, events_lost=0, buffers_lost=0,
                            callback_errors=0, stop_result=0, process_result=0)
        self.peer = ("WINDOWS_BLE_WINRT:CLOCK_ORIGIN_UNIX_MS=10000\n"
                     "WINDOWS_BLE_WINRT:GET_SERVICES=5000\n"
                     "WINDOWS_BLE_WINRT:DISCOVERY_FAILURE=1000,1,1,242\nprivate values\n")

    def run_summary(self, **changes):
        values = dict(metadata=self.metadata, transport=self.transport, links=self.links,
                      capture=self.capture, peer=self.peer)
        return summarize_links(**{**values, **changes})

    def test_numeric_projection_and_clock_without_private_fields(self):
        result = self.run_summary()
        self.assertEqual(result["link_rows_verified_against_transport"], 1)
        self.assertEqual(result["nearby_links"][0]["estimated_ms_after_request"], 0)
        self.assertEqual(result["nearby_links"][0]["interval"], 48)
        self.assertNotIn("private", json.dumps(result))

    def test_event_shape_and_one_to_one_transport_validation(self):
        for bad in [self.links.replace("5000,2", "5001,2"),
                    self.links.replace(",62,10,", ",62,1,"),
                    self.links.replace(",0,48,0,960", ",0,0,0,960"),
                    self.links.replace(",0,1,0,48", ",62,1,0,48"),
                    self.links + "5000,2,62,10,0,1,0,48,0,960\n",
                    self.links.replace("supervision_timeout", "private_data")]:
            with self.assertRaises(ValueError):
                self.run_summary(links=bad)

    def test_disconnect_and_failed_connection_fields(self):
        metadata = self.metadata.replace(",41\n", ",14\n")
        transport = self.transport.replace(",33\n", ",6\n")
        header = ",".join(COLUMNS) + "\n"
        result = self.run_summary(metadata=metadata, transport=transport,
                                  links=header + "5000,2,5,-1,0,1,62,-1,-1,-1\n")
        self.assertEqual(result["nearby_links"][0]["detail"], 62)
        result = self.run_summary(links=header + "5000,2,62,10,62,-1,-1,-1,-1,-1\n")
        self.assertEqual(result["nearby_links"][0]["connection_handle"], -1)

    def test_failed_or_missing_integrity_is_rejected(self):
        with self.assertRaises(ValueError):
            self.run_summary(capture={**self.capture, "events_lost": 1})
        with self.assertRaises(ValueError):
            self.run_summary(capture=None)


if __name__ == "__main__":
    unittest.main()
