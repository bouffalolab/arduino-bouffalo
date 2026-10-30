import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "ble_etw_summary", Path(__file__).resolve().parents[1] / "tools/summarize_ble_etw_metadata.py")
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)

HEADER = ",".join(summary.COLUMNS) + "\n"
PEER = """WINDOWS_BLE_WINRT:CLOCK_ORIGIN_UNIX_MS=10000.000
WINDOWS_BLE_COEX:SESSION_PASS=1
WINDOWS_BLE_WINRT:GET_SERVICES=5000
WINDOWS_BLE_WINRT:DISCOVERY_FAILURE=1000,0,0,23
private-device-id and private-payload must not reach the summary
"""


class EtwMetadataTest(unittest.TestCase):
    def test_separate_clock_origins_and_context(self):
        csv = HEADER + "5000,0602ecef-6381-4bc0-aeda-eb9bb919b276,9,0,0,4,4,8\n"
        csv += "10000,0602ecef-6381-4bc0-aeda-eb9bb919b276,10,0,0,4,4,8\n"
        result = summary.summarize(csv, PEER, "1970-01-01T00:00:10Z", context_ms=100)
        self.assertEqual(result["completed_sessions"], [1])
        self.assertEqual(result["event_counts"]["connection_failure"], 1)
        self.assertEqual(len(result["failure"]["nearby_metadata"]), 1)
        self.assertEqual(result["failure"]["nearby_metadata"][0]["estimated_ms_after_request"], 0)
        self.assertNotIn("private", str(result))

    def test_reject_unanchored_log_and_payload_columns(self):
        with self.assertRaises(ValueError):
            summary.summarize(HEADER, PEER.replace("CLOCK_ORIGIN_UNIX_MS", "UNKNOWN"), "1970-01-01T00:00:10Z")
        with self.assertRaises(ValueError):
            summary.summarize(HEADER.strip() + ",payload\n", PEER, "1970-01-01T00:00:10Z")

    def test_successful_sample_is_not_a_failure_and_invalid_rows_are_rejected(self):
        result = summary.summarize(HEADER, PEER.split("WINDOWS_BLE_WINRT:DISCOVERY_FAILURE")[0], "1970-01-01T00:00:10Z")
        self.assertIsNone(result["failure"])
        for row in ["nan,0602ecef-6381-4bc0-aeda-eb9bb919b276,9,0,0,4,4,8",
                    "1,unknown,9,0,0,4,4,8", "1,0602ecef-6381-4bc0-aeda-eb9bb919b276,9"]:
            with self.assertRaises(ValueError):
                summary.summarize(HEADER + row, PEER, "1970-01-01T00:00:10Z")

    def test_dotnet_round_trip_timestamp_on_python_310(self):
        result = summary.summarize(HEADER, PEER, "1970-01-01T00:00:10.0000000Z")
        self.assertEqual(result["failure"]["duration_ms"], 1000)

    def test_bthport_headers_and_collector_integrity(self):
        csv = HEADER + "5000,8a1f9517-3a8c-4a9e-a018-4f17a200f277,263,0,0,4,4,123\n"
        capture = dict(schema=1, passed=True, origin_utc="1970-01-01T00:00:10.0000000Z",
                       origin_bracket_ms=0.25, events=1, events_lost=0, buffers_lost=0,
                       callback_errors=0, stop_result=0, process_result=0)
        result = summary.summarize(csv, PEER, None, capture_summary=capture)
        self.assertEqual(result["event_counts"], {"hci_packet_write": 1})
        self.assertEqual(result["clock_limit"]["origin_bracket_ms"], 0.25)
        self.assertNotIn("123", str(result))  # header data length is not payload evidence
        for bad in [dict(passed=False), dict(events=2), dict(events_lost=1),
                    dict(callback_errors=1), dict(stop_result=5), dict(process_result=5),
                    dict(origin_bracket_ms=float("nan")), dict(events=True)]:
            with self.assertRaises(ValueError):
                summary.summarize(csv, PEER, None, capture_summary={**capture, **bad})

    def test_hci_channel_metadata_does_not_invent_direction(self):
        csv = HEADER + "5000,8a1f9517-3a8c-4a9e-a018-4f17a200f277,402,0,0,4,4,123\n"
        csv += "5001,8a1f9517-3a8c-4a9e-a018-4f17a200f277,403,10,1,4,4,8\n"
        result = summary.summarize(csv, PEER.replace("BLE_COEX", "BLE_DISCOVERY"),
                                   "1970-01-01T00:00:10Z")
        self.assertEqual(result["completed_sessions"], [1])
        self.assertEqual(result["event_id_counts"], {"bthport:402": 1, "bthport:403": 1})
        self.assertNotIn("hci_packet_write", result["event_counts"])
        self.assertNotIn("hci_packet_read", result["event_counts"])

    def test_cancelled_discovery_is_preserved_without_claiming_cause(self):
        peer = PEER.replace("DISCOVERY_FAILURE=1000,0,0,23", "DISCOVERY_CANCELLED=20016,1,1,242")
        result = summary.summarize(HEADER, peer, "1970-01-01T00:00:10Z")
        self.assertEqual(result["failure"]["event"], "CANCELLED")
        self.assertEqual(result["failure"]["duration_ms"], 20016)
        self.assertEqual(result["failure"]["mtu"], 242)
        self.assertNotIn("timeout", str(result["failure"]))
        unknown = peer.replace("20016,1,1,242", "20016,-1,-1,-1")
        self.assertEqual(summary.summarize(HEADER, unknown, "1970-01-01T00:00:10Z")["failure"]["mtu"], -1)

    def test_transport_counters_must_match_each_hci_header(self):
        csv = HEADER + "5000,8a1f9517-3a8c-4a9e-a018-4f17a200f277,402,0,0,4,4,11\n"
        header = ",".join(summary.TRANSPORT_COLUMNS) + "\n"
        data = header + "5000,1,0,2,3\n"
        result = summary.summarize(csv, PEER, "1970-01-01T00:00:10Z", transport_text=data)
        self.assertEqual(result["transport_counters"]["acl_write_credits_zero_rows"], 1)
        self.assertEqual(len(result["failure"]["nearby_transport_counters"]), 1)
        for bad in [header, data.replace("5000,", "5001,"), data.replace(",2,3", ",2,4"),
                    data.replace(",0,2,3", ",-1,2,3"), data.replace(",0,2,3", ",65536,2,3"),
                    data + "5000,1,0,2,3\n", data.replace("bip_data_length", "raw_packet")]:
            with self.assertRaises(ValueError):
                summary.summarize(csv, PEER, "1970-01-01T00:00:10Z", transport_text=bad)

    def test_discovery_stage_is_numeric_whitelist_and_resets_at_next_request(self):
        peer = PEER.replace("WINDOWS_BLE_WINRT:DISCOVERY_FAILURE", "WINDOWS_BLE_WINRT:DISCOVERY_STAGE=CHARACTERISTICS,3,-1,2625\nWINDOWS_BLE_WINRT:DISCOVERY_FAILURE")
        result = summary.summarize(HEADER, peer, "1970-01-01T00:00:10Z")
        self.assertEqual(result["failure"]["discovery_stage"], dict(
            operation="CHARACTERISTICS", calls=3, result_status=-1, elapsed_ms=2625))
        peer = peer.replace("WINDOWS_BLE_WINRT:DISCOVERY_FAILURE", "WINDOWS_BLE_WINRT:GET_SERVICES=6000\nWINDOWS_BLE_WINRT:DISCOVERY_FAILURE")
        self.assertNotIn("discovery_stage", summary.summarize(
            HEADER, peer, "1970-01-01T00:00:10Z")["failure"])
