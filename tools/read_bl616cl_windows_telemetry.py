#!/usr/bin/env python3
"""Read paired BL616CL HID telemetry through Windows, then reattach WSL USB.

The installed usbipd-win 3.2 client can reject the current WSL kernel even
though the local usbip client works. After the Windows-only HID read, use the
WSL client directly so the caller can confirm the actual CDC identity before
continuing.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_ra4_at_uart_gate as paired


ROOT_DIR = Path(__file__).resolve().parents[1]
WINDOWS_WSL_ROOT = r"\\wsl.localhost\Ubuntu-22.04"
TELEMETRY_SCRIPTS = {
    "ap-start": ROOT_DIR / "tools" / "inspect_bl616cl_ap_start_windows.ps1",
    "ap-start-heap": ROOT_DIR / "tools" / "inspect_bl616cl_ap_start_heap_windows.ps1",
    "ap-start-command": (
        ROOT_DIR / "tools" / "inspect_bl616cl_ap_start_command_windows.ps1"
    ),
    "ap-start-trace": ROOT_DIR / "tools" / "inspect_bl616cl_ap_start_trace_windows.ps1",
    "ap-start-notification": (
        ROOT_DIR / "tools" / "inspect_bl616cl_ap_start_notification_windows.ps1"
    ),
    "ap-start-wpa-event-path": (
        ROOT_DIR / "tools" / "inspect_bl616cl_ap_start_wpa_event_path_windows.ps1"
    ),
    "ap-start-create-ap": (
        ROOT_DIR / "tools" / "inspect_bl616cl_ap_start_create_ap_windows.ps1"
    ),
    "ap-start-hostapd-lifecycle": (
        ROOT_DIR / "tools" / "inspect_bl616cl_ap_start_hostapd_lifecycle_windows.ps1"
    ),
    "ap-start-hostapd-state-update": (
        ROOT_DIR / "tools" /
        "inspect_bl616cl_ap_start_hostapd_state_update_windows.ps1"
    ),
    "ap-start-status-code": (
        ROOT_DIR / "tools" / "inspect_bl616cl_ap_start_status_code_windows.ps1"
    ),
    "ap-start-allocator": (
        ROOT_DIR / "tools" / "inspect_bl616cl_ap_start_allocator_windows.ps1"
    ),
    "cdc-in": ROOT_DIR / "tools" / "inspect_bl616cl_cdc_in_windows.ps1",
    "user-uart": ROOT_DIR / "tools" / "inspect_bl616cl_user_uart_windows.ps1",
    "swd-readonly": ROOT_DIR / "tools" / "verify_bl616cl_swd_readonly_windows.ps1",
    "swd-frequency-sweep": (
        ROOT_DIR / "tools" / "verify_bl616cl_swd_frequency_sweep_windows.ps1"
    ),
    "swd-halt-reset": (
        ROOT_DIR / "tools" / "verify_bl616cl_swd_halt_reset_windows.ps1"
    ),
    "swd-d4-attach-readonly": (
        ROOT_DIR / "tools" / "verify_bl616cl_swd_d4_attach_readonly_windows.ps1"
    ),
    "swd-d4-attach-readonly-reset": (
        ROOT_DIR /
        "tools" / "verify_bl616cl_swd_d4_attach_readonly_reset_windows.ps1"
    ),
    "swd-d4-release-power": (
        ROOT_DIR / "tools" / "verify_bl616cl_swd_d4_release_power_windows.ps1"
    ),
    "swd-d4-minimal-idcode-reset": (
        ROOT_DIR /
        "tools" / "verify_bl616cl_swd_d4_minimal_idcode_reset_windows.ps1"
    ),
    "swd-d4-disconnect-delay": (
        ROOT_DIR /
        "tools" / "verify_bl616cl_swd_d4_disconnect_delay_windows.ps1"
    ),
    "swd-d4-halt-run-disconnect-delay": (
        ROOT_DIR /
        "tools" / "verify_bl616cl_swd_d4_halt_run_disconnect_delay_windows.ps1"
    ),
    "swd-d4-no-reattach": (
        ROOT_DIR / "tools" / "verify_bl616cl_swd_d4_no_reattach_windows.ps1"
    ),
    "swd-d4-disconnect-reconnect": (
        ROOT_DIR / "tools" / "verify_bl616cl_swd_d4_disconnect_reconnect_windows.ps1"
    ),
    "ra4-application-reset": ROOT_DIR / "tools" / "reset_ra4_application_windows.ps1",
    "tcp-runtime": ROOT_DIR / "tools" / "inspect_bl616cl_tcp_runtime_windows.ps1",
    "tcp-data-plane": (
        ROOT_DIR / "tools" / "inspect_bl616cl_tcp_data_plane_windows.ps1"
    ),
    "ap-packet-input": (
        ROOT_DIR / "tools" / "inspect_bl616cl_ap_packet_input_windows.ps1"
    ),
    "ap-udp-packet-input": (
        ROOT_DIR / "tools" / "inspect_bl616cl_ap_udp_packet_input_windows.ps1"
    ),
    "udp-socket": (
        ROOT_DIR / "tools" / "inspect_bl616cl_udp_socket_windows.ps1"
    ),
    "tcp-listener-lwip": (
        ROOT_DIR / "tools" / "inspect_bl616cl_tcp_listener_lwip_windows.ps1"
    ),
    "tcp-listener-close": (
        ROOT_DIR / "tools" / "inspect_bl616cl_tcp_listener_close_windows.ps1"
    ),
}


class TelemetryCaptureError(RuntimeError):
    def __init__(self, message: str, bus_id: str) -> None:
        super().__init__(message)
        self.bus_id = bus_id


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "page",
        choices=sorted(TELEMETRY_SCRIPTS),
        help="diagnostic feature page to read",
    )
    parser.add_argument("--bridge-port", default="/dev/ttyACM0")
    parser.add_argument("--wsl-distribution", default="Ubuntu-22.04")
    parser.add_argument("--reattach-timeout-ms", type=int, default=10000)
    parser.add_argument(
        "--wsl-usbip-host",
        default="auto",
        help="Windows USB/IP host address, or auto to use the WSL default gateway",
    )
    parser.add_argument(
        "--windows-hid-ready-timeout-ms",
        type=int,
        default=10000,
        help="maximum Windows-side wait for the BL616CL CMSIS-DAP HID interface",
    )
    parser.add_argument("--telemetry-attempts", type=int, default=3)
    parser.add_argument("--retry-delay-ms", type=int, default=750)
    return parser.parse_args()


def capture_windows_telemetry(
    telemetry_script: Path,
    windows_hid_ready_timeout_seconds: float,
    bus_id: str,
) -> str:
    windows_script = WINDOWS_WSL_ROOT + str(telemetry_script)
    windows_source = WINDOWS_WSL_ROOT + str(
        ROOT_DIR / "tools" / "verify_bl616cl_cmsis_dap_windows.cs"
    )
    script = (
        "$ErrorActionPreference = 'Stop'; "
        "$stage = Join-Path $env:TEMP ('uno-r4-bl616cl-telemetry-' + "
        "[Guid]::NewGuid().ToString('N')); "
        "New-Item -ItemType Directory -Path $stage | Out-Null; "
        "$stagedScript = Join-Path $stage (Split-Path -Leaf "
        f"'{windows_script}'); "
        "$stagedSource = Join-Path $stage 'verify_bl616cl_cmsis_dap_windows.cs'; "
        "try { "
        f"Copy-Item -LiteralPath '{windows_script}' -Destination $stagedScript -Force; "
        f"Copy-Item -LiteralPath '{windows_source}' -Destination $stagedSource -Force; "
        f"$busid = '{bus_id}'; "
        "usbipd wsl detach --busid $busid; "
        "try { "
        "$deadline = (Get-Date).AddSeconds("
        f"{windows_hid_ready_timeout_seconds:.3f}"
        "); "
        "$hidDevices = @(); "
        "do { "
        "$hidDevices = @(Get-PnpDevice -PresentOnly -Class HIDClass "
        "-ErrorAction SilentlyContinue | Where-Object { "
        "$_.Status -eq 'OK' -and "
        "$_.InstanceId -match '^HID\\\\VID_2341&PID_1002&MI_02\\\\' "
        "}); "
        # The paired BL616CL and an ESP32-S3 baseline board can both expose
        # 2341:1002 CMSIS-DAP HID. The C# probe below resolves the BL616CL
        # through its USB parent serial, so this is only a readiness check.
        "if ($hidDevices.Count -ge 1) { break }; "
        "Start-Sleep -Milliseconds 250 "
        "} while ((Get-Date) -lt $deadline); "
        "if ($hidDevices.Count -lt 1) { "
        "throw \"paired BL616CL CMSIS-DAP HID was not healthy after detach; "
        "found $($hidDevices.Count) matching interface(s)\" "
        "}; "
        "& $stagedScript; "
        "if ($LASTEXITCODE -ne 0) { "
        "throw \"Windows BL616CL telemetry helper failed with exit code $LASTEXITCODE\" "
        "} "
        "} finally { "
        "Write-Output \"BL616CL_WINDOWS_TELEMETRY_BUSID=$busid\" "
        "} "
        "} finally { "
        "Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue "
        "}"
    )
    command = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-Command",
        script,
    ]
    print("COMMAND=" + " ".join(command[:5]) + " <redacted PowerShell body>")
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        # Account for the caller-configured PnP wait plus feature-report
        # compilation/opening time. Killing PowerShell mid-detach can leave
        # the composite device in a descriptor-error state on Windows.
        timeout=windows_hid_ready_timeout_seconds + 20.0,
    )
    output = result.stdout + result.stderr
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    bus_id = next(
        (
            line.partition("=")[2].strip()
            for line in output.splitlines()
            if line.startswith("BL616CL_WINDOWS_TELEMETRY_BUSID=")
        ),
        "",
    )
    if result.returncode != 0:
        raise TelemetryCaptureError(
            f"Windows BL616CL telemetry helper failed with exit code {result.returncode}",
            bus_id,
        )
    if not bus_id:
        raise RuntimeError("Windows BL616CL telemetry did not report a USB/IP bus ID")
    return bus_id


def discover_windows_bus_id() -> str:
    script = (
        "$ErrorActionPreference = 'Stop'; "
        "$matches = @((usbipd state | ConvertFrom-Json).Devices | "
        "Where-Object { $_.BusId -ne $null -and "
        "$_.InstanceId.EndsWith('\\BL616CL-PROVISIONAL', "
        "[System.StringComparison]::OrdinalIgnoreCase) } | "
        "ForEach-Object { $_.BusId }); "
        "if ($matches.Count -ne 1) { "
        "throw \"expected exactly one paired BL616CL USB device, found $($matches.Count)\" "
        "}; "
        "Write-Output $matches[0]"
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", script],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        # Query the structured device state before detach so the stable
        # BL616CL-PROVISIONAL serial selects the intended board even when an
        # ESP32-S3 UNO R4 baseline board is attached too.
        timeout=30.0,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(
            "cannot identify paired BL616CL USB/IP bus ID" +
            (f": {detail}" if detail else "")
        )
    bus_id = result.stdout.strip()
    if not bus_id:
        raise RuntimeError("Windows did not report a paired BL616CL USB/IP bus ID")
    return bus_id


def resolve_wsl_usbip_host(requested: str) -> str:
    if requested != "auto":
        return requested

    result = subprocess.run(
        ["ip", "route", "show", "default"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError("cannot resolve the WSL USB/IP host route")
    fields = result.stdout.split()
    if len(fields) < 3 or fields[0] != "default" or fields[1] != "via":
        raise RuntimeError("WSL default route does not provide a USB/IP host")
    return fields[2]


def resolve_wsl_usbip_client() -> str:
    """Locate an installed usbip userspace client for this WSL kernel.

    WSL kernel updates can outpace the packaged linux-tools version. The
    compatible client still works when it is installed under a different
    linux-tools directory, but `/usr/bin/usbip` may be absent or point to a
    wrapper with no matching kernel directory.
    """
    candidates = ["/usr/bin/usbip"]
    kernel_client = f"/usr/lib/linux-tools-{os.uname().release}/usbip"
    if kernel_client not in candidates:
        candidates.append(kernel_client)
    candidates.extend(sorted(glob.glob("/usr/lib/linux-tools-*/usbip")))

    seen: set[str] = set()
    for candidate in candidates:
        if candidate in seen or not os.path.isfile(candidate):
            continue
        seen.add(candidate)
        result = subprocess.run(
            [candidate, "version"],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            return candidate
    raise RuntimeError(
        "cannot find a working WSL usbip client; install linux-tools or "
        "provide a compatible /usr/bin/usbip wrapper"
    )


def discover_wsl_usbip_bus_id(host: str) -> str:
    """Find the final carrier through the USB/IP service without PowerShell.

    This is the recovery path for hosts where WSL interop cannot start a
    Windows process.  USB/IP's device list includes the Windows PnP instance
    ID, whose serial suffix remains stable across the BL616CL USB reset.
    """
    usbip_client = resolve_wsl_usbip_client()
    result = subprocess.run(
        [usbip_client, "list", f"--remote={host}"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(
            "cannot list Windows USB/IP exports" +
            (f": {detail}" if detail else "")
        )

    current_bus_id = ""
    matches: list[str] = []
    for line in result.stdout.splitlines():
        match = re.match(r"^\s*(\d+-\d+):", line)
        if match is not None:
            current_bus_id = match.group(1)
            continue
        if (
            current_bus_id
            and r"\BL616CL-PROVISIONAL" in line
            and current_bus_id not in matches
        ):
            matches.append(current_bus_id)

    if len(matches) != 1:
        found = ", ".join(matches) if matches else "none"
        raise RuntimeError(
            "expected exactly one exported final-carrier BL616CL USB device, "
            f"found: {found}"
        )
    return matches[0]


def reattach_bridge_via_windows_usbip(distribution: str, bus_id: str) -> None:
    """Use usbipd-win to attach only this telemetry transaction's device."""
    command = [
        "powershell.exe",
        "-NoProfile",
        "-Command",
        (
            "usbipd wsl attach "
            f"--busid {bus_id} --distribution {distribution}"
        ),
    ]
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(
            "Windows USB/IP reattach failed" +
            (f": {detail}" if detail else "")
        )


def reattach_bridge_via_wsl_usbip(host: str, bus_id: str) -> None:
    """Fallback for hosts where usbipd-win cannot attach the current WSL."""
    usbip_client = resolve_wsl_usbip_client()
    command = [
        "sudo",
        "-n",
        usbip_client,
        "attach",
        f"--remote={host}",
        f"--busid={bus_id}",
    ]
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        # usbipd-win can attach the device before it reports that its WSL
        # client check failed. In that case a direct attach correctly reports
        # the already-exported device as busy; the caller still verifies the
        # actual CDC identity before continuing.
        if "Device busy (exported)" in detail:
            return
        raise RuntimeError(
            "WSL USB/IP reattach failed; configure a narrowly scoped NOPASSWD "
            f"rule for `sudo -n {usbip_client} attach`. " +
            (f"detail: {detail}" if detail else "")
        )


def wait_for_bridge(port: str, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            paired.require_port_identity(port, "2341", "1002", "00")
            return
        except RuntimeError:
            time.sleep(0.25)
    raise RuntimeError("paired BL616CL did not reattach to WSL")


def reattach_bridge(
    distribution: str,
    host: str,
    bus_id: str,
    port: str,
    timeout_seconds: float,
) -> None:
    """Prefer usbipd-win, then prove the CDC node or fall back to usbip."""
    try:
        reattach_bridge_via_windows_usbip(distribution, bus_id)
        wait_for_bridge(port, timeout_seconds)
        return
    except RuntimeError:
        pass

    reattach_bridge_via_wsl_usbip(host, bus_id)
    wait_for_bridge(port, timeout_seconds)


def main() -> int:
    args = parse_arguments()
    if (
        args.reattach_timeout_ms <= 0
        or args.windows_hid_ready_timeout_ms <= 0
        or args.telemetry_attempts <= 0
        or args.retry_delay_ms < 0
    ):
        raise RuntimeError("invalid telemetry retry or reattach timeout")
    paired.require_port_identity(args.bridge_port, "2341", "1002", "00")
    telemetry_script = TELEMETRY_SCRIPTS[args.page]
    if not telemetry_script.is_file():
        raise RuntimeError(f"telemetry script is missing: {telemetry_script}")

    print(f"Bridge: {args.bridge_port} (2341:1002 paired BL616CL)")
    print(f"Telemetry page: {args.page}")
    wsl_usbip_host = resolve_wsl_usbip_host(args.wsl_usbip_host)
    last_error: RuntimeError | None = None

    for attempt in range(1, args.telemetry_attempts + 1):
        bus_id = discover_windows_bus_id()
        try:
            bus_id = capture_windows_telemetry(
                telemetry_script,
                args.windows_hid_ready_timeout_ms / 1000.0,
                bus_id,
            )
            reattach_bridge(
                args.wsl_distribution,
                wsl_usbip_host,
                bus_id,
                args.bridge_port,
                args.reattach_timeout_ms / 1000.0,
            )
            print("BL616CL_WINDOWS_TELEMETRY_PASS")
            return 0
        except TelemetryCaptureError as error:
            last_error = error
            bus_id = error.bus_id
        except subprocess.TimeoutExpired:
            last_error = TelemetryCaptureError(
                "Windows BL616CL telemetry helper timed out",
                bus_id,
            )
        except RuntimeError as error:
            last_error = error

        if bus_id:
            try:
                reattach_bridge(
                    args.wsl_distribution,
                    wsl_usbip_host,
                    bus_id,
                    args.bridge_port,
                    args.reattach_timeout_ms / 1000.0,
                )
            except RuntimeError as reattach_error:
                last_error = reattach_error

        if attempt < args.telemetry_attempts:
            print(
                "BL616CL_WINDOWS_TELEMETRY_RETRY "
                f"page={args.page} attempt={attempt}/{args.telemetry_attempts}"
            )
            time.sleep(args.retry_delay_ms / 1000.0)

    if last_error is None:
        raise RuntimeError("Windows BL616CL telemetry failed without an error")
    raise last_error


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
