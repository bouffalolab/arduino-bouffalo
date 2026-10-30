#!/usr/bin/env python3
"""Correlate redacted WinRT timing with header-only Bluetooth ETW metadata."""
import argparse
import collections
import csv
import datetime
import decimal
import json
from pathlib import Path
import re


COLUMNS = ["elapsed_ms", "provider", "event_id", "opcode", "task", "level",
           "process_id", "user_data_length"]
TRANSPORT_COLUMNS = ["elapsed_ms", "command_packets_allowed", "acl_write_credits",
                     "bip_type", "bip_data_length"]
PROVIDERS = {
    "0602ecef-6381-4bc0-aeda-eb9bb919b276": "policy",
    "db25b328-a6f6-444f-9d97-a50e20217d16": "bthmini",
    "33693e1d-246a-471b-83be-3e75f47a832d": "bthusb",
    "8a1f9517-3a8c-4a9e-a018-4f17a200f277": "bthport",
}
EVENTS = {
    ("policy", 3): "advertising_policy",
    ("policy", 9): "connection_success",
    ("policy", 10): "connection_failure",
    ("bthmini", 201): "invalid_hci_event_length",
    ("bthmini", 202): "invalid_data_packet_length",
    ("bthusb", 1): "controller_information",
    ("bthport", 263): "hci_packet_write",
    ("bthport", 264): "hci_packet_read",
    ("bthport", 267): "fixed_channel_data_send",
    ("bthport", 302): "fixed_channel_connect",
    # Local manifest identifies channel 16 as HCI but leaves event 402's
    # payload unspecified. Its header does not establish packet direction.
    ("bthport", 402): "hci_channel_metadata",
    ("bthport", 403): "bthport_event_403",
}


def summarize(csv_text, peer_text, ready_utc, context_ms=2000, capture_summary=None,
              transport_text=None):
    if capture_summary is not None:
        required = {"schema", "passed", "origin_utc", "origin_bracket_ms", "events",
                    "events_lost", "buffers_lost", "callback_errors", "stop_result", "process_result"}
        if set(capture_summary) != required or capture_summary["schema"] != 1:
            raise ValueError("unexpected collector summary schema")
        counters = ["events", "events_lost", "buffers_lost", "callback_errors", "stop_result", "process_result"]
        if any(type(capture_summary[key]) is not int or capture_summary[key] < 0 for key in counters):
            raise ValueError("invalid collector counters")
        if (capture_summary["passed"] is not True or capture_summary["events"] == 0
                or any(capture_summary[key] != 0 for key in counters[1:-1])
                or capture_summary["process_result"] not in (0, 1223)):
            raise ValueError("collector integrity check failed")
        bracket = decimal.Decimal(str(capture_summary["origin_bracket_ms"]))
        if not bracket.is_finite() or bracket < 0:
            raise ValueError("invalid collector clock bracket")
        ready_utc = capture_summary["origin_utc"]
    if ready_utc is None:
        raise ValueError("provide capture READY time or collector summary")
    # .NET's round-trip format has seven fractional digits; Python 3.10's
    # fromisoformat accepts at most six. Drop only sub-microsecond precision.
    ready_utc = re.sub(r"(\.\d{6})\d+(?=Z$|\+00:00$)", r"\1", ready_utc)
    stamp = datetime.datetime.fromisoformat(ready_utc.replace("Z", "+00:00"))
    if stamp.utcoffset() != datetime.timedelta(0):
        raise ValueError("capture READY time must include UTC timezone")
    ready_ms = stamp.timestamp() * 1000
    reader = csv.DictReader(csv_text.splitlines())
    if reader.fieldnames != COLUMNS:
        raise ValueError("unexpected ETW columns; only metadata schema is accepted")
    events = []
    transport_headers = []
    for row in reader:
        if None in row or any(value is None for value in row.values()):
            raise ValueError("incomplete ETW row")
        provider = PROVIDERS.get(row["provider"].lower())
        if provider is None:
            raise ValueError("unknown ETW provider")
        elapsed = decimal.Decimal(row["elapsed_ms"])
        if not elapsed.is_finite() or elapsed < 0:
            raise ValueError("invalid ETW timestamp")
        values = {column: int(row[column]) for column in COLUMNS[2:]}
        if any(value < 0 for value in values.values()):
            raise ValueError("negative ETW header value")
        event_id = values["event_id"]
        if provider == "bthport" and event_id == 402:
            transport_headers.append((elapsed, values["user_data_length"]))
        events.append({"elapsed_ms": float(elapsed), "provider": provider,
                       "event_id": event_id,
                       "kind": EVENTS.get((provider, event_id), "other_metadata")})
    if capture_summary is not None and len(events) != capture_summary["events"]:
        raise ValueError("CSV row count disagrees with collector summary")
    transport = []
    if transport_text is not None:
        transport_reader = csv.DictReader(transport_text.splitlines())
        if transport_reader.fieldnames != TRANSPORT_COLUMNS:
            raise ValueError("unexpected transport counter columns")
        for row in transport_reader:
            if None in row or any(value is None for value in row.values()):
                raise ValueError("incomplete transport counter row")
            elapsed = decimal.Decimal(row["elapsed_ms"])
            values = {key: int(row[key]) for key in TRANSPORT_COLUMNS[1:]}
            if (not elapsed.is_finite() or elapsed < 0 or
                    any(not 0 <= values[key] <= limit for key, limit in
                        zip(TRANSPORT_COLUMNS[1:], (255, 65535, 255, 65527)))):
                raise ValueError("invalid transport counter value")
            index = len(transport)
            if (index >= len(transport_headers) or elapsed != transport_headers[index][0]
                    or values["bip_data_length"] + 8 != transport_headers[index][1]):
                raise ValueError("transport counters disagree with event headers")
            transport.append(dict(elapsed_ms=float(elapsed), **values))
        if len(transport) != len(transport_headers):
            raise ValueError("transport counter rows missing")
    origin = re.search(r"(?m)^WINDOWS_BLE_WINRT:CLOCK_ORIGIN_UNIX_MS=([\d.]+)\s*$", peer_text)
    if origin is None:
        raise ValueError("peer has no logging UTC anchor; do not infer exact alignment")
    peer_origin = float(origin[1])
    sessions = [int(value) for value in re.findall(
        r"(?m)^WINDOWS_BLE_(?:COEX|DISCOVERY):SESSION_PASS=(\d+)\s*$", peer_text)]
    failure = None
    # Associate only a failure snapshot with its preceding service request.
    request = None
    stage_snapshot = None
    for line in peer_text.splitlines():
        match = re.fullmatch(r"WINDOWS_BLE_WINRT:GET_SERVICES=(\d+)", line)
        if match:
            request = int(match[1])
            stage_snapshot = None
        stage = re.fullmatch(r"WINDOWS_BLE_WINRT:DISCOVERY_STAGE=(UNKNOWN|SERVICES|CHARACTERISTICS|DESCRIPTORS),(\d+),(-?\d+),(\d+)", line)
        if stage:
            stage_snapshot = {"operation": stage[1], "calls": int(stage[2]),
                              "result_status": int(stage[3]), "elapsed_ms": int(stage[4])}
        match = re.fullmatch(r"WINDOWS_BLE_WINRT:DISCOVERY_(FAILURE|CANCELLED)=(\d+),(-?\d+),(-?\d+),(-?\d+)", line)
        if match:
            if request is None:
                raise ValueError("discovery failure has no preceding request")
            kind = match[1]
            duration, connected, active, mtu = map(int, match.groups()[1:])
            if connected not in (-1, 0, 1) or active not in (-1, 0, 1) or mtu < -1:
                raise ValueError("invalid discovery state snapshot")
            start = peer_origin + request
            end = start + duration
            nearby = []
            for event in events:
                # Legacy READY has an unknown startup offset. A new collector
                # summary supplies a UTC read bracketed at the QPC origin.
                estimate = ready_ms + event["elapsed_ms"]
                if start - context_ms <= estimate <= end + context_ms:
                    nearby.append(dict(event, estimated_ms_after_request=round(estimate-start, 3)))
            failure = {"event": kind, "duration_ms": duration, "connected": connected,
                       "session_active": active, "mtu": mtu,
                       "context_ms": context_ms, "nearby_metadata": nearby}
            if stage_snapshot is not None:
                failure["discovery_stage"] = stage_snapshot
            if transport_text is not None:
                failure["nearby_transport_counters"] = [dict(row, estimated_ms_after_request=round(
                    ready_ms + row["elapsed_ms"] - start, 3)) for row in transport
                    if start - context_ms <= ready_ms + row["elapsed_ms"] <= end + context_ms]
    result = {
        "metadata_rows": len(events),
        "event_counts": dict(collections.Counter(event["kind"] for event in events)),
        "event_id_counts": dict(collections.Counter(
            f"{event['provider']}:{event['event_id']}" for event in events)),
        "completed_sessions": sessions,
        "failure": failure,
        "capture_integrity": ("collector reported zero loss/errors; CSV row count agrees"
                              if capture_summary is not None else
                              "loss and callback counters cannot be verified from CSV alone"),
        "clock_limit": ({"origin_bracket_ms": float(bracket),
                         "note": "QPC origin preceded UTC read within this bracket; wall clock accuracy is external"}
                        if capture_summary is not None else
                        "ETW UTC uses READY as an upper estimate; QPC-origin-to-READY offset was not recorded"),
        "scope": "host metadata across devices; no ATT delivery or per-device attribution proof",
    }
    if transport_text is not None:
        result["transport_counters"] = {
            "rows": len(transport),
            "bip_type_counts": dict(collections.Counter(row["bip_type"] for row in transport)),
            "acl_write_credits_min": min((row["acl_write_credits"] for row in transport), default=None),
            "acl_write_credits_zero_rows": sum(row["acl_write_credits"] == 0 for row in transport),
            "scope": "host transport counters; BIP type values are not assigned packet direction",
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--etw-csv", required=True, type=Path)
    parser.add_argument("--peer-log", required=True, type=Path)
    parser.add_argument("--capture-ready-utc")
    parser.add_argument("--capture-summary", type=Path)
    parser.add_argument("--transport-csv", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = summarize(args.etw_csv.read_text(encoding="utf-8-sig"),
                       args.peer_log.read_text(encoding="utf-8"), args.capture_ready_utc,
                       capture_summary=(json.loads(args.capture_summary.read_text(encoding="utf-8-sig"))
                                        if args.capture_summary else None),
                       transport_text=(args.transport_csv.read_text(encoding="utf-8-sig")
                                       if args.transport_csv else None))
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("BLE_ETW_METADATA_SUMMARY_WRITTEN")


if __name__ == "__main__":
    main()
