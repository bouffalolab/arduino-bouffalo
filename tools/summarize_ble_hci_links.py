#!/usr/bin/env python3
"""Validate numeric HCI link summaries and align them with discovery failures."""
import argparse
import collections
import csv
from decimal import Decimal
import json
from pathlib import Path

from summarize_ble_etw_metadata import summarize


COLUMNS = ["elapsed_ms", "bip_type", "event_code", "subevent", "status",
           "connection_handle", "detail", "interval", "latency", "supervision_timeout"]


def summarize_links(metadata, transport, links, capture, peer):
    context = summarize(metadata, peer, None, capture_summary=capture,
                        transport_text=transport)
    available = collections.Counter(
        (Decimal(row["elapsed_ms"]), int(row["bip_type"]), int(row["bip_data_length"]))
        for row in csv.DictReader(transport.splitlines()))
    reader = csv.DictReader(links.splitlines())
    if reader.fieldnames != COLUMNS:
        raise ValueError("unexpected link summary columns")
    rows = []
    for row in reader:
        if None in row or any(value is None for value in row.values()):
            raise ValueError("incomplete link summary row")
        elapsed = Decimal(row["elapsed_ms"])
        values = {key: int(row[key]) for key in COLUMNS[1:]}
        if (not elapsed.is_finite() or elapsed < 0 or values["bip_type"] != 2
                or not 0 <= values["status"] <= 255):
            raise ValueError("invalid link summary field")
        code, sub, status = values["event_code"], values["subevent"], values["status"]
        if code == 5 and sub == -1:
            length = 6
            if any(values[key] != -1 for key in ("interval", "latency", "supervision_timeout")):
                raise ValueError("disconnect has connection parameters")
            if status == 0 and not 0 <= values["detail"] <= 255:
                raise ValueError("invalid disconnect reason")
        elif code == 62 and sub in (1, 10):
            length = 21 if sub == 1 else 33
            if status == 0 and not (
                    values["detail"] in (0, 1) and 6 <= values["interval"] <= 3200
                    and 0 <= values["latency"] <= 499
                    and 10 <= values["supervision_timeout"] <= 3200):
                raise ValueError("invalid connection parameters")
        else:
            raise ValueError("unsupported link event")
        if status == 0:
            if not 0 <= values["connection_handle"] <= 4095:
                raise ValueError("invalid connection handle")
        elif any(values[key] != -1 for key in (
                "connection_handle", "detail", "interval", "latency", "supervision_timeout")):
            raise ValueError("undefined failed-event fields retained")
        key = elapsed, 2, length
        if available[key] == 0:
            raise ValueError("link summary disagrees with transport framing")
        available[key] -= 1
        rows.append(dict(elapsed_ms=float(elapsed), **values))
    failure = context["failure"]
    near = ({row["elapsed_ms"]: row["estimated_ms_after_request"]
             for row in failure["nearby_transport_counters"]} if failure else {})
    nearby = [dict(row, estimated_ms_after_request=near[row["elapsed_ms"]])
              for row in rows if row["elapsed_ms"] in near]
    return dict(
        link_rows_verified_against_transport=len(rows),
        event_counts=dict(collections.Counter(
            f'{r["event_code"]}:{r["subevent"]}:{r["status"]}:{r["detail"]}' for r in rows)),
        failure=({key: value for key, value in failure.items()
                  if key not in ("nearby_metadata", "nearby_transport_counters")} if failure else None),
        nearby_links=nearby,
        capture_integrity=context["capture_integrity"],
        scope="observed BIP2 HCI framing; whole-host link metadata, no device identity or root-cause proof",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("metadata", "transport", "links", "capture-summary", "peer", "output"):
        parser.add_argument("--" + flag, type=Path, required=True)
    args = parser.parse_args()
    result = summarize_links(args.metadata.read_text(encoding="utf-8-sig"),
                             args.transport.read_text(encoding="utf-8-sig"),
                             args.links.read_text(encoding="utf-8-sig"),
                             json.loads(args.capture_summary.read_text(encoding="utf-8-sig")),
                             args.peer.read_text())
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("BLE_HCI_LINK_SUMMARY_WRITTEN")


if __name__ == "__main__":
    main()
