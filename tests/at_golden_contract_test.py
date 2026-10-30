#!/usr/bin/env python3
"""Validate the checked-in, redacted ESP32-S3 Wi-Fi AT compatibility contract."""

from __future__ import annotations

import json
from pathlib import Path
import unittest


ROOT_DIR = Path(__file__).resolve().parents[1]
CONTRACT_PATH = ROOT_DIR / "tests" / "at_golden" / "contracts" / "esp32_s3_wifi_0.6.0.json"


def load_contracts() -> dict:
    data = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
    if data.get("schema_version") != 1:
        raise ValueError("unsupported contract schema")
    if data["baseline"]["firmware_version"] != "0.6.0":
        raise ValueError("unexpected baseline firmware")
    return {entry["id"]: entry for entry in data["contracts"]}


def response_matches(contract: dict, response: bytes) -> bool:
    specification = contract["response"]
    response_type = specification["type"]
    if response_type == "exact":
        return response == specification["ascii"].encode("ascii")

    if response_type == "prefixed-enum":
        prefix = specification["prefix_ascii"].encode("ascii")
        suffix = specification["suffix_ascii"].encode("ascii")
        if not response.startswith(prefix) or not response.endswith(suffix):
            return False
        value = response[len(prefix) : len(response) - len(suffix)].decode("ascii", "strict")
        return value in specification["allowed_values"]

    if response_type == "wifi-scan":
        if response == specification["empty_response_ascii"].encode("ascii"):
            return True

        prefix = specification["nonempty_prefix_ascii"].encode("ascii")
        payload_separator = specification["payload_length_separator_ascii"].encode("ascii")
        terminal = specification["terminal_ascii"].encode("ascii")
        row_suffix = specification["row_suffix_ascii"].encode("ascii")
        delimiter = specification["field_delimiter_ascii"].encode("ascii")
        if not response.startswith(prefix) or not response.endswith(terminal):
            return False

        separator_at = response.find(payload_separator, len(prefix))
        if separator_at < 0:
            return False
        length_text = response[len(prefix) : separator_at]
        if not length_text.isdigit():
            return False
        payload = response[separator_at + len(payload_separator) : -len(terminal)]
        if int(length_text) != len(payload):
            return False
        rows = payload.split(row_suffix)
        if not rows or rows[-1] != b"":
            return False
        for row in rows[:-1]:
            fields = row.split(delimiter)
            if len(fields) != 5:
                return False
            ssid, bssid, rssi, channel, encryption = fields
            if not ssid or not bssid:
                return False
            if not rssi.startswith((b"-", b"0", b"1", b"2", b"3", b"4", b"5", b"6", b"7", b"8", b"9")):
                return False
            if not channel.isdigit():
                return False
            if encryption.decode("ascii", "strict") not in specification["allowed_encryption_values"]:
                return False
        return True

    raise ValueError(f"unsupported response type: {response_type}")


class GoldenContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.contracts = load_contracts()

    def test_contract_ids_and_requests_are_unique(self) -> None:
        self.assertEqual(
            set(self.contracts),
            {
                "beginsta_empty_ssid",
                "wifi_mode_invalid",
                "wifi_mode_write_null",
                "wifi_mode_write_sta",
                "wifi_status_read",
                "wifi_mode_read",
                "reconnect_read",
                "reconnect_write_disable",
                "reconnect_write_enable",
                "autoconnect_read",
                "autoconnect_write_enable",
                "wifi_scan",
            },
        )
        requests = [contract["request_ascii"] for contract in self.contracts.values()]
        self.assertEqual(len(requests), len(set(requests)))
        self.assertTrue(all(request.endswith("\r\n") for request in requests))

    def test_exact_error_paths_match_captured_framing(self) -> None:
        self.assertTrue(response_matches(self.contracts["beginsta_empty_ssid"], b"ERROR\r\n"))
        self.assertTrue(response_matches(self.contracts["wifi_mode_invalid"], b"ERROR\r\n"))
        self.assertFalse(response_matches(self.contracts["wifi_mode_invalid"], b"ERROR\n"))

    def test_dynamic_enum_framing_matches_without_fixing_runtime_values(self) -> None:
        self.assertTrue(response_matches(self.contracts["wifi_status_read"], b"+GETSTATUS: 2\r\nOK\r\n"))
        self.assertTrue(response_matches(self.contracts["wifi_status_read"], b"+GETSTATUS: 255\r\nOK\r\n"))
        self.assertFalse(response_matches(self.contracts["wifi_status_read"], b"+GETSTATUS: 10\r\nOK\r\n"))
        self.assertTrue(response_matches(self.contracts["wifi_mode_read"], b"+WIFIMODE: 1\r\n\r\nOK\r\n"))
        self.assertFalse(response_matches(self.contracts["wifi_mode_read"], b"+WIFIMODE: 1\r\nOK\r\n"))
        self.assertTrue(response_matches(self.contracts["reconnect_read"], b"+RECONNECT: 0\r\nOK\r\n"))
        self.assertTrue(response_matches(self.contracts["reconnect_write_disable"], b"OK\r\n"))
        self.assertFalse(response_matches(self.contracts["reconnect_write_disable"], b"+RECONNECT: \r\nOK\r\n"))
        self.assertTrue(response_matches(self.contracts["reconnect_write_enable"], b"OK\r\n"))
        self.assertTrue(response_matches(self.contracts["autoconnect_read"], b"+AUTOCONNECT: 0\r\nOK\r\n"))
        self.assertFalse(response_matches(self.contracts["autoconnect_read"], b"+AUTOCONNECT: 1\r\nOK\r\n"))
        self.assertTrue(response_matches(self.contracts["autoconnect_write_enable"], b"OK\r\n"))

    def test_wifi_mode_write_framing_and_observed_round_trip(self) -> None:
        null_mode = self.contracts["wifi_mode_write_null"]
        sta_mode = self.contracts["wifi_mode_write_sta"]
        response = b"+WIFIMODE: \r\nOK\r\n"
        self.assertTrue(response_matches(null_mode, response))
        self.assertTrue(response_matches(sta_mode, response))
        self.assertFalse(response_matches(null_mode, b"+WIFIMODE: \r\n\r\nOK\r\n"))
        self.assertEqual(null_mode["observed_postconditions"]["readback_wifi_mode"], "0")
        self.assertEqual(null_mode["observed_postconditions"]["readback_wifi_status"], "255")
        self.assertEqual(sta_mode["observed_postconditions"]["readback_wifi_mode"], "1")
        self.assertEqual(sta_mode["observed_postconditions"]["readback_wifi_status"], "0")

    def test_autoconnect_is_a_documented_esp32_core_noop(self) -> None:
        read_semantics = self.contracts["autoconnect_read"]["semantics"]
        write_semantics = self.contracts["autoconnect_write_enable"]["semantics"]
        self.assertEqual(read_semantics["baseline_behavior"], "deprecated-no-op")
        self.assertEqual(read_semantics["return_value"], False)
        self.assertEqual(write_semantics["baseline_behavior"], "deprecated-no-op")
        self.assertEqual(write_semantics["setter_return_value"], False)
        self.assertEqual(write_semantics["setter_return_checked_by_handler"], False)
        self.assertTrue(write_semantics["framework_appends_ok_for_handler_status"])
        self.assertEqual(
            write_semantics["expected_readback_ascii"],
            "+AUTOCONNECT: 0\r\nOK\r\n",
        )

    def test_sanitized_scan_fixture_matches_captured_wire_shape(self) -> None:
        contract = self.contracts["wifi_scan"]
        response = contract["response"]["sanitized_example_response_ascii"].encode("ascii")
        postconditions = contract["response"]["observed_postconditions"]
        self.assertTrue(response_matches(contract, response))
        self.assertTrue(response_matches(contract, b"OK\r\n"))
        self.assertFalse(response_matches(contract, b"+WIFISCAN: |bad\r\nOK\r\n"))
        self.assertFalse(
            response_matches(
                contract,
                b"+WIFISCAN: 1|fixture | 02:00:00:00:00:01 | -42 | 1 | open\r\nOK\r\n",
            )
        )
        self.assertEqual(postconditions["immediate_status_request_ascii"], "AT+GETSTATUS?\r\n")
        self.assertEqual(
            postconditions["immediate_status_response_ascii"],
            "+GETSTATUS: 2\r\nOK\r\n",
        )
        self.assertEqual(
            postconditions["compatibility_note"],
            "Status 2 is an observed post-scan transition, not an environment-independent constant.",
        )
        self.assertFalse(
            response_matches(
                contract,
                b"+WIFISCAN: 50|fixture | 02:00:00:00:00:01 | -42 | 1 | WPA4\r\nOK\r\n",
            )
        )


if __name__ == "__main__":
    unittest.main()
