#!/usr/bin/env python3
"""Protect the AP+STA fixture from timing out during host telemetry preflight."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "examples" / "ra4-wifis3-apsta-traffic" / "ra4-wifis3-apsta-traffic.ino"
WSL_RUNNER = ROOT / "tools" / "run_bl616cl_apsta_wlan_final_carrier.py"
WINDOWS_RUNNER = ROOT / "tools" / "run_bl616cl_apsta_windows_owned.ps1"
WINDOWS_CLIENT = ROOT / "tools" / "run_bl616cl_apsta_wlan_client_windows.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    fixture = FIXTURE.read_text(encoding="ascii")
    wsl_runner = WSL_RUNNER.read_text(encoding="ascii")
    windows_runner = WINDOWS_RUNNER.read_text(encoding="ascii")
    windows_client = WINDOWS_CLIENT.read_text(encoding="ascii")

    require("WaitTrafficStart," in fixture, "fixture has a traffic wait phase")
    require(
        re.search(
            r"phase = Phase::WaitTrafficStart;\s*Serial\.println\(\"RA4_APSTA:READY\"\);",
            fixture,
        )
        is not None,
        "fixture waits for host traffic start after READY",
    )
    require(
        "phase != Phase::WaitTrafficStart" in fixture,
        "fixture excludes preflight wait from traffic timeout",
    )
    require(
        re.search(
            r'phase == Phase::WaitTrafficStart\).*?strcmp\(command, "TRAFFIC"\)',
            fixture,
            re.DOTALL,
        )
        is not None,
        "fixture accepts an explicit TRAFFIC command",
    )
    require(
        "Serial.println(\"RA4_APSTA:TRAFFIC_START\")" in fixture,
        "fixture acknowledges traffic start",
    )

    require(
        "def trigger_traffic(link: serial.Serial)" in wsl_runner,
        "WSL runner has an explicit traffic trigger",
    )
    trigger_positions = [
        match.start()
        for match in re.finditer(r"trigger_traffic\(bridge\)", wsl_runner)
    ]
    require(
        len(trigger_positions) == 2,
        "WSL runner triggers traffic in both telemetry modes",
    )
    for position in trigger_positions:
        require(
            wsl_runner.find("run_windows_client(", position) >= 0,
            "each WSL traffic trigger is followed by the Windows client",
        )
    require(
        windows_runner.index('$serial.Write("TRAFFIC`n")') <
        windows_runner.index("& $trafficHelper"),
        "Windows-owned runner triggers traffic before the WLAN client",
    )
    require(
        "RA4_APSTA:AP_REASSOCIATION_TRAFFIC_PASS" in fixture,
        "RA4M1 fixture verifies a new AP client session",
    )
    for marker in (
        "WINDOWS_APSTA_CLIENT_DEPARTED",
        "WINDOWS_APSTA_CLIENT_REASSOCIATED",
        "WINDOWS_APSTA_CLIENT_REASSOCIATION_TRAFFIC_PASS",
    ):
        require(marker in windows_client, f"Windows client must emit {marker}")
    print("APSTA_FIXTURE_CONTROL_TEST_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
