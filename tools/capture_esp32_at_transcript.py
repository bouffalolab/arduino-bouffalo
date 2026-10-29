#!/usr/bin/env python3
"""Capture byte-accurate ESP32-S3 AT responses through the RA4M1 proxy."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
import sys
import time
from typing import Any

try:
    import serial
except ImportError as exc:
    serial = None
    SERIAL_IMPORT_ERROR = exc
else:
    SERIAL_IMPORT_ERROR = None


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_CASE_FILE = ROOT_DIR / "tests" / "at_golden" / "cases" / "dynamic_wifi.json"
DEFAULT_STATEFUL_CASE_FILE = ROOT_DIR / "tests" / "at_golden" / "cases" / "stateful_wifi.json"
CAPTURE_SCHEMA_VERSION = 2
DEFAULT_BAUDRATE = 115200


class CaseError(ValueError):
    """A checked-in capture plan is malformed."""


@dataclass(frozen=True)
class Completion:
    terminals: tuple[bytes, ...]
    timeout_ms: int
    quiet_ms: int


@dataclass(frozen=True)
class Recovery:
    automatic_restore: bool
    operator_actions: tuple[str, ...]


@dataclass(frozen=True)
class CaptureStep:
    phase: str
    request: bytes
    raw_payload: bytes

    @property
    def wire_request(self) -> bytes:
        return self.request + self.raw_payload


@dataclass(frozen=True)
class CaptureCase:
    case_id: str
    classification: str
    summary: str
    preconditions: tuple[str, ...]
    state_change: str
    pre_state_requests: tuple[bytes, ...]
    pre_state_expected_responses: tuple[bytes, ...]
    request: bytes
    follow_ups: tuple[bytes, ...]
    steps: tuple[CaptureStep, ...]
    completion: Completion
    recovery: Recovery | None


def require_text(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise CaseError(f"{field} must be a non-empty string")
    return value


def ascii_bytes(value: Any, field: str) -> bytes:
    try:
        return require_text(value, field).encode("ascii")
    except UnicodeEncodeError as exc:
        raise CaseError(f"{field} must contain ASCII only") from exc


def parse_ascii_request_list(raw: Any, field: str) -> tuple[bytes, ...]:
    if not isinstance(raw, list) or not raw:
        raise CaseError(f"{field} must be a non-empty list")
    requests = tuple(ascii_bytes(value, field) for value in raw)
    if any(not request.endswith(b"\r\n") for request in requests):
        raise CaseError(f"{field} requests must end in CRLF")
    return requests


def validate_line_terminated_request(request: bytes, field: str) -> None:
    if not (request.endswith(b"\r\n") or request.endswith(b"\n\r")):
        raise CaseError(f"{field} requests must end in CRLF or LFCR")


def parse_raw_payload_hex(raw: Any, field: str) -> bytes:
    value = require_text(raw, field)
    if len(value) % 2 or any(character not in "0123456789abcdefABCDEF" for character in value):
        raise CaseError(f"{field} must contain an even number of hexadecimal characters")
    return bytes.fromhex(value)


def parse_steps(raw: Any, case_id: str) -> tuple[CaptureStep, ...]:
    if not isinstance(raw, list) or not raw:
        raise CaseError(f"{case_id}.steps must be a non-empty list")

    steps: list[CaptureStep] = []
    phases: set[str] = set()
    for index, raw_step in enumerate(raw):
        field = f"{case_id}.steps[{index}]"
        if not isinstance(raw_step, dict):
            raise CaseError(f"{field} must be an object")
        phase = require_text(raw_step.get("phase"), f"{field}.phase")
        if phase in phases:
            raise CaseError(f"{field}.phase must be unique")
        phases.add(phase)
        request = ascii_bytes(raw_step.get("request_ascii"), f"{field}.request_ascii")
        validate_line_terminated_request(request, f"{field}.request_ascii")
        raw_payload = b""
        if "raw_payload_hex" in raw_step:
            raw_payload = parse_raw_payload_hex(raw_step["raw_payload_hex"], f"{field}.raw_payload_hex")
        steps.append(CaptureStep(phase=phase, request=request, raw_payload=raw_payload))
    return tuple(steps)


def parse_recovery(raw: Any, case_id: str, state_change: str) -> Recovery | None:
    if state_change == "none":
        if raw is not None:
            raise CaseError(f"{case_id}.recovery is only valid for state-changing cases")
        return None
    if not isinstance(raw, dict):
        raise CaseError(f"{case_id}.recovery must be an object")
    if raw.get("automatic_restore") is not False:
        raise CaseError(f"{case_id}.recovery.automatic_restore must be false")
    actions = raw.get("operator_actions")
    if not isinstance(actions, list) or not actions or not all(
        isinstance(action, str) and action for action in actions
    ):
        raise CaseError(f"{case_id}.recovery.operator_actions must be a non-empty list")
    return Recovery(automatic_restore=False, operator_actions=tuple(actions))


def parse_completion(raw: Any, case_id: str) -> Completion:
    if not isinstance(raw, dict):
        raise CaseError(f"{case_id}.completion must be an object")
    if raw.get("type") != "terminal-line":
        raise CaseError(f"{case_id}.completion.type must be terminal-line")

    raw_terminals = raw.get("terminal_ascii")
    if not isinstance(raw_terminals, list) or not raw_terminals:
        raise CaseError(f"{case_id}.completion.terminal_ascii must be a non-empty list")
    terminals = tuple(
        ascii_bytes(value, f"{case_id}.completion.terminal_ascii") for value in raw_terminals
    )
    if any(not terminal.endswith(b"\r\n") for terminal in terminals):
        raise CaseError(f"{case_id}.completion terminal lines must end in CRLF")

    timeout_ms = raw.get("timeout_ms")
    quiet_ms = raw.get("quiet_ms")
    if not isinstance(timeout_ms, int) or timeout_ms <= 0:
        raise CaseError(f"{case_id}.completion.timeout_ms must be a positive integer")
    if not isinstance(quiet_ms, int) or quiet_ms < 0:
        raise CaseError(f"{case_id}.completion.quiet_ms must be a non-negative integer")
    return Completion(terminals=terminals, timeout_ms=timeout_ms, quiet_ms=quiet_ms)


def parse_case(raw: Any) -> CaptureCase:
    if not isinstance(raw, dict):
        raise CaseError("each case must be an object")
    case_id = require_text(raw.get("id"), "case.id")
    preconditions = raw.get("preconditions", [])
    if not isinstance(preconditions, list) or not all(
        isinstance(item, str) and item for item in preconditions
    ):
        raise CaseError(f"{case_id}.preconditions must be a list of non-empty strings")
    state_change = raw.get("state_change")
    if state_change not in {"none", "temporary", "persistent"}:
        raise CaseError(f"{case_id}.state_change must be none, temporary, or persistent")
    raw_pre_state_requests = raw.get("pre_state_requests_ascii")
    if state_change == "none":
        if raw_pre_state_requests is not None:
            raise CaseError(f"{case_id}.pre_state_requests_ascii is only valid for state-changing cases")
        pre_state_requests = ()
    else:
        pre_state_requests = parse_ascii_request_list(
            raw_pre_state_requests, f"{case_id}.pre_state_requests_ascii"
        )
    raw_pre_state_expected_responses = raw.get("pre_state_expected_responses_ascii")
    if raw_pre_state_expected_responses is None:
        pre_state_expected_responses = ()
    else:
        if state_change == "none":
            raise CaseError(
                f"{case_id}.pre_state_expected_responses_ascii is only valid for state-changing cases"
            )
        pre_state_expected_responses = parse_ascii_request_list(
            raw_pre_state_expected_responses, f"{case_id}.pre_state_expected_responses_ascii"
        )
        if len(pre_state_expected_responses) != len(pre_state_requests):
            raise CaseError(
                f"{case_id}.pre_state_expected_responses_ascii must match pre_state_requests_ascii"
            )
    has_steps = "steps" in raw
    has_legacy_requests = "request_ascii" in raw or "follow_up_requests_ascii" in raw
    if has_steps and has_legacy_requests:
        raise CaseError(
            f"{case_id} must use either steps or request_ascii/follow_up_requests_ascii, not both"
        )
    if has_steps:
        steps = parse_steps(raw["steps"], case_id)
        request = steps[0].request
        follow_ups = tuple(step.request for step in steps[1:])
    else:
        follow_up_values = raw.get("follow_up_requests_ascii", [])
        if not isinstance(follow_up_values, list):
            raise CaseError(f"{case_id}.follow_up_requests_ascii must be a list")
        request = ascii_bytes(raw.get("request_ascii"), f"{case_id}.request_ascii")
        validate_line_terminated_request(request, f"{case_id}.request_ascii")
        follow_ups = tuple(
            ascii_bytes(value, f"{case_id}.follow_up_requests_ascii") for value in follow_up_values
        )
        for follow_up in follow_ups:
            validate_line_terminated_request(follow_up, f"{case_id}.follow_up_requests_ascii")
        steps = (CaptureStep(phase="request", request=request, raw_payload=b""),) + tuple(
            CaptureStep(phase="follow_up", request=follow_up, raw_payload=b"")
            for follow_up in follow_ups
        )
    return CaptureCase(
        case_id=case_id,
        classification=require_text(raw.get("classification"), f"{case_id}.classification"),
        summary=require_text(raw.get("summary"), f"{case_id}.summary"),
        preconditions=tuple(preconditions),
        state_change=state_change,
        pre_state_requests=pre_state_requests,
        pre_state_expected_responses=pre_state_expected_responses,
        request=request,
        follow_ups=follow_ups,
        steps=steps,
        completion=parse_completion(raw.get("completion"), case_id),
        recovery=parse_recovery(raw.get("recovery"), case_id, state_change),
    )


def load_cases(case_file: Path) -> dict[str, CaptureCase]:
    try:
        raw = json.loads(case_file.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise CaseError(f"case file not found: {case_file}") from exc
    except json.JSONDecodeError as exc:
        raise CaseError(f"invalid JSON in {case_file}: {exc}") from exc

    if not isinstance(raw, dict) or raw.get("schema_version") != 1:
        raise CaseError(f"{case_file} must use schema_version 1")
    raw_cases = raw.get("cases")
    if not isinstance(raw_cases, list) or not raw_cases:
        raise CaseError(f"{case_file} must contain a non-empty cases list")

    parsed: dict[str, CaptureCase] = {}
    for raw_case in raw_cases:
        case = parse_case(raw_case)
        if case.case_id in parsed:
            raise CaseError(f"duplicate case id: {case.case_id}")
        parsed[case.case_id] = case
    return parsed


def render_printable(data: bytes) -> str:
    return "".join(
        chr(value) if 0x20 <= value <= 0x7E else f"\\x{value:02x}" for value in data
    )


def encode_bytes(data: bytes) -> dict[str, str]:
    return {
        "base64": base64.b64encode(data).decode("ascii"),
        "hex": data.hex(),
        "printable_ascii": render_printable(data),
    }


def terminal_name(response: bytes, completion: Completion) -> str | None:
    for terminal in completion.terminals:
        if response.endswith(terminal):
            return terminal.decode("ascii").replace("\r", "\\r").replace("\n", "\\n")
    return None


def drain_input(port: Any, quiet_ms: int) -> bytes:
    drained = bytearray()
    deadline = time.monotonic() + quiet_ms / 1000.0
    while time.monotonic() < deadline:
        waiting = port.in_waiting
        if waiting:
            drained.extend(port.read(waiting))
            deadline = time.monotonic() + quiet_ms / 1000.0
            continue
        time.sleep(0.005)
    return bytes(drained)


def capture_request(port: Any, request: bytes, completion: Completion) -> dict[str, Any]:
    stale = drain_input(port, completion.quiet_ms)
    started = time.monotonic()
    port.write(request)
    port.flush()

    response = bytearray()
    terminal = None
    last_data_at = started
    deadline = started + completion.timeout_ms / 1000.0
    while time.monotonic() < deadline:
        waiting = port.in_waiting
        if waiting:
            response.extend(port.read(waiting))
            last_data_at = time.monotonic()
            if terminal is None:
                terminal = terminal_name(bytes(response), completion)
            continue
        if terminal is not None and time.monotonic() - last_data_at >= completion.quiet_ms / 1000.0:
            break
        time.sleep(0.005)

    ended = time.monotonic()
    return {
        "request": encode_bytes(request),
        "discarded_before_request": encode_bytes(stale),
        "response": encode_bytes(bytes(response)),
        "terminal": terminal,
        "timed_out": terminal is None,
        "elapsed_ms": round((ended - started) * 1000, 3),
    }


def capture_exchange(
    port: Any, request: bytes, completion: Completion, phase: str
) -> dict[str, Any]:
    exchange = capture_request(port, request, completion)
    exchange["phase"] = phase
    return exchange


def read_preflight_record(case: CaptureCase, record_path: Path) -> str:
    try:
        raw = record_path.read_bytes()
        record = json.loads(raw)
    except FileNotFoundError as exc:
        raise CaseError(f"preflight record not found: {record_path}") from exc
    except json.JSONDecodeError as exc:
        raise CaseError(f"invalid JSON in preflight record {record_path}: {exc}") from exc

    if record.get("schema_version") != CAPTURE_SCHEMA_VERSION:
        raise CaseError("preflight record schema version does not match this capture tool")
    if record.get("case", {}).get("id") != case.case_id:
        raise CaseError("preflight record belongs to a different case")
    if record.get("execution", {}).get("mode") != "preflight":
        raise CaseError("preflight record was not produced with --preflight")

    exchanges = record.get("exchanges")
    if not isinstance(exchanges, list) or len(exchanges) != len(case.pre_state_requests):
        raise CaseError("preflight record does not contain every required pre-state response")
    for expected_request, exchange in zip(case.pre_state_requests, exchanges):
        if (
            exchange.get("phase") != "pre_state"
            or exchange.get("timed_out") is not False
            or exchange.get("request", {}).get("hex") != expected_request.hex()
        ):
            raise CaseError("preflight record is incomplete, timed out, or does not match the case")
    if case.pre_state_expected_responses:
        for expected_response, exchange in zip(case.pre_state_expected_responses, exchanges):
            if exchange.get("response", {}).get("hex") != expected_response.hex():
                raise CaseError("preflight record does not match the required baseline response")
    return hashlib.sha256(raw).hexdigest()


def build_output(
    case: CaptureCase,
    port_name: str,
    baudrate: int,
    firmware_version: str | None,
    captures: list[dict[str, Any]],
    execution_mode: str,
    preflight_sha256: str | None,
) -> dict[str, Any]:
    recovery = None
    if case.recovery is not None:
        recovery = {
            "automatic_restore": case.recovery.automatic_restore,
            "operator_actions": list(case.recovery.operator_actions),
        }
    return {
        "schema_version": CAPTURE_SCHEMA_VERSION,
        "captured_at_utc": datetime.now(timezone.utc).isoformat(),
        "transport": {
            "port": port_name,
            "baudrate": baudrate,
            "path": "host USB CDC -> RA4M1 Serial -> Serial2 -> ESP32-S3 AT UART",
        },
        "esp32_s3_firmware_version": firmware_version,
        "case": {
            "id": case.case_id,
            "classification": case.classification,
            "summary": case.summary,
            "preconditions": list(case.preconditions),
            "state_change": case.state_change,
            "pre_state_requests_ascii": [
                request.decode("ascii") for request in case.pre_state_requests
            ],
            "pre_state_expected_responses_ascii": [
                response.decode("ascii") for response in case.pre_state_expected_responses
            ],
            "recovery": recovery,
        },
        "execution": {
            "mode": execution_mode,
            "preflight_sha256": preflight_sha256,
        },
        "exchanges": captures,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture ESP32-S3 AT transcripts through the temporary RA4M1 proxy."
    )
    parser.add_argument("--case", help="case id from the checked-in capture plan")
    parser.add_argument("--case-file", type=Path, default=DEFAULT_CASE_FILE)
    parser.add_argument("--port", help="explicit serial device path; never auto-detected")
    parser.add_argument("--baudrate", type=int, default=DEFAULT_BAUDRATE)
    parser.add_argument("--output", type=Path, help="JSON capture output path")
    parser.add_argument(
        "--firmware-version",
        help="ESP32-S3 firmware version, obtained before capture (for example 0.6.0)",
    )
    parser.add_argument(
        "--allow-state-change",
        action="store_true",
        help="allow a state-changing capture after a valid --preflight-record is supplied",
    )
    parser.add_argument(
        "--preflight",
        action="store_true",
        help="capture only the required read-only pre-state for a state-changing case",
    )
    parser.add_argument(
        "--preflight-record",
        type=Path,
        help="JSON record created by --preflight for this state-changing case",
    )
    parser.add_argument(
        "--list-cases",
        action="store_true",
        help="print checked-in case ids and exit",
    )
    parser.add_argument(
        "--validate-cases",
        action="store_true",
        help="validate the case file and exit without opening a serial port",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the selected capture plan without opening a serial port",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        cases = load_cases(args.case_file)
    except CaseError as exc:
        print(f"case validation failed: {exc}", file=sys.stderr)
        return 2

    if args.list_cases:
        for case in cases.values():
            print(f"{case.case_id}\tstate_change={case.state_change}\t{case.summary}")
        return 0
    if args.validate_cases:
        print(f"validated {len(cases)} cases from {args.case_file}")
        return 0
    if not args.case:
        print("--case is required unless --list-cases or --validate-cases is used", file=sys.stderr)
        return 2
    if args.case not in cases:
        print(f"unknown case: {args.case}", file=sys.stderr)
        return 2
    case = cases[args.case]

    if args.dry_run:
        print(
            json.dumps(
                {
                    "case": case.case_id,
                    "state_change": case.state_change,
                    "pre_state_requests_ascii": [
                        request.decode("ascii") for request in case.pre_state_requests
                    ],
                    "pre_state_expected_responses_ascii": [
                        response.decode("ascii") for response in case.pre_state_expected_responses
                    ],
                    "request_ascii": case.request.decode("ascii"),
                    "follow_up_requests_ascii": [
                        request.decode("ascii") for request in case.follow_ups
                    ],
                    "steps": [
                        {
                            "phase": step.phase,
                            "request_ascii": step.request.decode("ascii"),
                            "raw_payload_hex": step.raw_payload.hex(),
                        }
                        for step in case.steps
                    ],
                    "recovery": (
                        None
                        if case.recovery is None
                        else {
                            "automatic_restore": case.recovery.automatic_restore,
                            "operator_actions": list(case.recovery.operator_actions),
                        }
                    ),
                    "timeout_ms": case.completion.timeout_ms,
                },
                ensure_ascii=True,
                indent=2,
            )
        )
        return 0

    preflight_sha256 = None
    if case.state_change == "none":
        if args.preflight or args.preflight_record:
            print("--preflight and --preflight-record are only valid for state-changing cases", file=sys.stderr)
            return 2
        if args.allow_state_change:
            print("--allow-state-change is unnecessary for a state_change=none case", file=sys.stderr)
            return 2
        execution_mode = "capture"
    elif args.preflight:
        if args.allow_state_change or args.preflight_record:
            print("--preflight cannot be combined with --allow-state-change or --preflight-record", file=sys.stderr)
            return 2
        execution_mode = "preflight"
    else:
        if not args.allow_state_change:
            print(
                f"{case.case_id} is marked state_change={case.state_change}; "
                "first run --preflight, review recovery, then rerun with "
                "--allow-state-change and --preflight-record",
                file=sys.stderr,
            )
            return 2
        if args.preflight_record is None:
            print("--preflight-record is required before a state-changing capture", file=sys.stderr)
            return 2
        try:
            preflight_sha256 = read_preflight_record(case, args.preflight_record)
        except CaseError as exc:
            print(f"preflight validation failed: {exc}", file=sys.stderr)
            return 2
        execution_mode = "mutation"
    if not args.port or not args.output:
        print("--port and --output are required for capture", file=sys.stderr)
        return 2
    if args.baudrate <= 0:
        print("--baudrate must be positive", file=sys.stderr)
        return 2
    if serial is None:
        print(f"pyserial is required: {SERIAL_IMPORT_ERROR}", file=sys.stderr)
        return 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with serial.Serial(args.port, args.baudrate, timeout=0, write_timeout=2) as port:
            if execution_mode == "preflight":
                exchanges = [
                    capture_exchange(port, request, case.completion, "pre_state")
                    for request in case.pre_state_requests
                ]
            else:
                exchanges = [
                    capture_exchange(port, step.wire_request, case.completion, step.phase)
                    for step in case.steps
                ]
    except serial.SerialException as exc:
        print(f"serial capture failed: {exc}", file=sys.stderr)
        return 1

    output = build_output(
        case=case,
        port_name=args.port,
        baudrate=args.baudrate,
        firmware_version=args.firmware_version,
        captures=exchanges,
        execution_mode=execution_mode,
        preflight_sha256=preflight_sha256,
    )
    args.output.write_text(json.dumps(output, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    timed_out = any(exchange["timed_out"] for exchange in exchanges)
    preflight_mismatch = (
        execution_mode == "preflight"
        and bool(case.pre_state_expected_responses)
        and any(
            exchange["response"]["hex"] != expected_response.hex()
            for expected_response, exchange in zip(case.pre_state_expected_responses, exchanges)
        )
    )
    print(f"wrote {args.output}")
    if preflight_mismatch:
        print("preflight response does not match the required baseline", file=sys.stderr)
    return 1 if timed_out or preflight_mismatch else 0


if __name__ == "__main__":
    raise SystemExit(main())
