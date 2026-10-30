#!/usr/bin/env python3
"""Compare host ATT fingerprints with one board snapshot, retaining uncertainty."""
import argparse
import collections
import csv
import decimal
import json
from pathlib import Path
import re

from summarize_ble_etw_metadata import summarize


COLUMNS = ["elapsed_ms", "bip_type", "connection_handle", "pb_flag",
           "att_opcode", "att_length", "fnv1a"]


def compare(metadata, transport, att_csv, capture, peer, board):
    context = summarize(metadata, peer, None, capture_summary=capture,
                        transport_text=transport)
    if context["failure"] is None:
        raise ValueError("comparison requires a failed discovery with a clock anchor")
    reader = csv.DictReader(att_csv.splitlines())
    if reader.fieldnames != COLUMNS:
        raise ValueError("unexpected ATT summary columns")
    transport_rows = collections.Counter(
        (decimal.Decimal(row["elapsed_ms"]), int(row["bip_type"]), int(row["bip_data_length"]))
        for row in csv.DictReader(transport.splitlines()))
    nearby = {decimal.Decimal(str(row["elapsed_ms"]))
              for row in context["failure"]["nearby_transport_counters"]}
    groups = collections.defaultdict(list)
    total = 0
    for row in reader:
        if None in row or any(value is None for value in row.values()):
            raise ValueError("incomplete ATT summary row")
        elapsed = decimal.Decimal(row["elapsed_ms"])
        values = {key: int(row[key]) for key in COLUMNS[1:]}
        if (not elapsed.is_finite() or elapsed < 0 or values["bip_type"] not in (3, 4)
                or values["pb_flag"] not in (0, 2)
                or not 0 <= values["connection_handle"] <= 4095
                or not 0 <= values["att_opcode"] <= 255
                or not 1 <= values["att_length"] <= 65519
                or not 0 <= values["fnv1a"] <= 0xffffffff):
            raise ValueError("invalid ATT summary numeric field")
        key = elapsed, values["bip_type"], values["att_length"] + 8
        if transport_rows[key] == 0:
            raise ValueError("ATT summary disagrees with transport framing")
        transport_rows[key] -= 1
        total += 1
        if elapsed in nearby:
            groups[values["connection_handle"], values["bip_type"]].append(
                (values["att_opcode"], values["att_length"], values["fnv1a"]))

    snapshots = []
    traces = []
    for line in board.splitlines():
        match = re.fullmatch(r"BLE_COEX_HCI:ATT_TRACE=([RT]),(\d+),(\d+),(\d+),(\d+),(\d+)", line)
        if match:
            traces.append((match[1], int(match[2]), int(match[3]), int(match[6])))
        if line.startswith("BLE_COEX_HCI:SNAPSHOT="):
            snapshots.append(traces)
            traces = []
    if not snapshots or not snapshots[-1]:
        raise ValueError("board has no final ATT trace snapshot")
    links = re.findall(r"BLE_COEX_HCI:LINK_TRACE=\d+,(\d+),(\d+),\d+,\d+", board)
    current_link = bool(links and links[-1] == ("1", "0"))
    candidates = []
    if current_link:
        for direction in ("R", "T"):
            expected = [row[1:] for row in snapshots[-1] if row[0] == direction]
            if not expected:
                continue
            for (handle, kind), observed in sorted(groups.items()):
                matched = 0
                for token in observed:
                    if matched < len(expected) and token == expected[matched]:
                        matched += 1
                candidates.append(dict(board_direction=direction, host_handle=handle,
                                       bip_type=kind, expected=len(expected),
                                       matched_prefix=matched, complete=matched == len(expected)))
    return {
        "failure": {key: value for key, value in context["failure"].items()
                    if key not in ("nearby_metadata", "nearby_transport_counters")},
        "att_rows_verified_against_transport": total,
        "last_board_link_is_connected": current_link,
        "candidates": candidates,
        "scope": ("ordered opcode/length/FNV consistency within the failure context; "
                  "no raw-byte proof, no inferred BIP direction, no per-device attribution; "
                  "a disconnected board snapshot is not assigned to the failed session"),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("metadata", "transport", "att", "capture-summary", "peer", "board", "output"):
        parser.add_argument("--" + flag, type=Path, required=True)
    args = parser.parse_args()
    result = compare(args.metadata.read_text(encoding="utf-8-sig"),
                     args.transport.read_text(encoding="utf-8-sig"),
                     args.att.read_text(encoding="utf-8-sig"),
                     json.loads(args.capture_summary.read_text(encoding="utf-8-sig")),
                     args.peer.read_text(), args.board.read_text())
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("BLE_ATT_SUMMARY_COMPARISON_WRITTEN")


if __name__ == "__main__":
    main()
