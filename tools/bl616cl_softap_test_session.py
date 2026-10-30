"""Hold a temporary Windows WLAN association while a separate gate runs."""

import base64
from datetime import datetime, timezone
import ipaddress
import subprocess
import time
from pathlib import Path
from types import SimpleNamespace

import serial

import run_bl616cl_softap_wlan_udp_final_carrier as echo
from run_ra4_wifis3_tls_gate import wsl_unc_path


class SoftApTestSession:
    def __init__(self, work_dir: Path):
        self.hold = work_dir / "wlan.hold"
        self.log = work_dir / "wlan.log"
        self.process = None
        self.output = None

    def start(self, args, ssid: str, password: str) -> None:
        self.ssid = ssid
        bootstrap = SimpleNamespace(
            sketch=echo.DEFAULT_SKETCH, image=echo.DEFAULT_IMAGE,
            build_dir=echo.DEFAULT_BUILD_DIR, fqbn=echo.DEFAULT_FQBN,
            arduino_cli=args.arduino_cli, bridge_port=args.bridge_port,
            runtime_settle_ms=args.runtime_settle_ms,
        )
        image = echo.compile_echo(bootstrap)
        echo.upload_echo(bootstrap, image)
        with serial.Serial(args.bridge_port, 115200, timeout=0.1) as bridge:
            bridge.dtr = False
            bridge.rts = False
            bridge.reset_input_buffer()
            bridge.write(f"START {ssid} {password}\n".encode("ascii"))
            bridge.flush()
            ready = echo.capture_until(bridge, (echo.READY_MARKER,), 30.0)
            address = echo.extract_address(ready)
            self.ap_address = ipaddress.IPv4Address(address)
            echo.print_ra4_markers(ready)

        # A new random SSID may be absent from Windows discovery cache.
        # Use the existing bounded directed-scan preparation before connecting.
        self.prepare_association(args, ssid, password, address)
        self.connect_prepared()

    def associate(self, args, ssid: str, password: str, address: str) -> None:
        """Own a Windows profile for an AP already started by another fixture."""
        self._launch_association(args, ssid, password, address)
        self._wait_association()

    def prepare_association(self, args, ssid: str, password: str, address: str) -> None:
        """Install the host profile before first AP creation, without connecting."""
        self.connect_when = self.hold.with_name("wlan.connect")
        self.connect_when.unlink(missing_ok=True)
        self._launch_association(args, ssid, password, address, self.connect_when)
        self._wait_marker("WINDOWS_SOFTAP_UDP_CLIENT_PROFILE_READY")
        print("TLS_RESOURCE_SOFTAP_PROFILE_READY", flush=True)

    def connect_prepared(self) -> None:
        if self.process is None or self.process.poll() is not None:
            raise RuntimeError("prepared WLAN profile owner is not running")
        self.connect_when.touch()
        self._wait_association()

    def _launch_association(self, args, ssid: str, password: str, address: str,
                            connect_when: Path | None = None) -> None:
        print("TLS_RESOURCE_SOFTAP_HOST_NOTICE=Windows will join the test AP without Internet; leave Wi-Fi unchanged. The runner reconnects and restores the original network automatically.", flush=True)
        self.ssid = ssid
        self.ap_address = ipaddress.IPv4Address(address)
        self.hold.touch()
        self.output = self.log.open("w", encoding="utf-8")
        encoded_ssid = base64.b64encode(ssid.encode("ascii")).decode("ascii")
        encoded_password = base64.b64encode(password.encode("ascii")).decode("ascii")
        # Generated credentials are passed only to this process, never logged.
        command = (
            "$ErrorActionPreference='Stop'; "
            "$ssid=[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String("
            f"'{encoded_ssid}')); "
            "$password=[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String("
            f"'{encoded_password}')); "
            f"& '{wsl_unc_path(echo.WINDOWS_HELPER)}' -Interface WLAN "
            f"-Ssid $ssid -Password $password -Address {address} -Port 4244 "
            f"-HoldUntilReleasedPath '{wsl_unc_path(self.hold)}'"
        )
        if connect_when is not None:
            command += f" -ConnectWhenPath '{wsl_unc_path(connect_when)}'"
        print(f"TLS_RESOURCE_SOFTAP_TIMELINE={datetime.now(timezone.utc).isoformat()},ASSOCIATION_HELPER_LAUNCH", flush=True)
        self.process = subprocess.Popen(
            ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
             "-Command", command], stdout=self.output, stderr=subprocess.STDOUT,
        )

    def _wait_marker(self, marker: str) -> None:
        deadline = time.monotonic() + 70.0
        while time.monotonic() < deadline:
            text = self.log.read_text(encoding="utf-8", errors="replace")
            if marker in text:
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.25)
        raise RuntimeError("temporary SoftAP association did not become ready")

    def _wait_association(self) -> None:
        self._wait_marker("WINDOWS_SOFTAP_UDP_CLIENT_ASSOCIATED")
        print(f"TLS_RESOURCE_SOFTAP_TIMELINE={datetime.now(timezone.utc).isoformat()},ASSOCIATION_HELPER_READY", flush=True)
        print("TLS_RESOURCE_SOFTAP_WLAN_ASSOCIATED")

    def request_preparation_rejoin(self) -> None:
        """Reconnect our owned profile after Sketch reset, before pressure only."""
        if self.process is None or self.process.poll() is not None:
            raise RuntimeError("temporary WLAN profile owner is not running")
        # Call netsh directly; starting PowerShell here adds a second WSL
        # interop process before the actual WLAN operation.
        print(f"TLS_RESOURCE_SOFTAP_TIMELINE={datetime.now(timezone.utc).isoformat()},PREPARATION_REJOIN_REQUEST", flush=True)
        result = subprocess.run(
            ["netsh.exe", "wlan", "connect", f"name={self.ssid}",
             f"ssid={self.ssid}", "interface=WLAN"],
            capture_output=True, timeout=10.0,
        )
        if result.returncode != 0:
            raise RuntimeError("temporary SoftAP rejoin request failed")
        print("TLS_RESOURCE_SOFTAP_PREPARATION_REJOIN_REQUESTED")

    def current_address(self) -> str:
        encoded = base64.b64encode(self.ssid.encode("ascii")).decode("ascii")
        network = ipaddress.IPv4Network(f"{self.ap_address}/24", strict=False)
        prefix = str(network.network_address).rsplit(".", 1)[0] + "."
        command = (
            "$ErrorActionPreference='Stop'; "
            "$ssid=[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String("
            f"'{encoded}')); "
            "$profile=Get-NetConnectionProfile -InterfaceAlias WLAN -ErrorAction SilentlyContinue; "
            "if($profile.Name -eq $ssid) { Get-NetIPAddress -InterfaceAlias WLAN "
            f"-AddressFamily IPv4 | Where-Object {{ $_.IPAddress -like '{prefix}*' "
            "-and $_.PrefixOrigin -ne 'WellKnown' } | ForEach-Object { $_.IPAddress } }"
        )
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline:
            try:
                result = subprocess.run(
                    ["powershell.exe", "-NoProfile", "-Command", command],
                    capture_output=True, text=True,
                    timeout=min(10.0, max(0.1, deadline - time.monotonic())),
                )
            except subprocess.TimeoutExpired:
                # This is passive preparation only, within the existing 30s
                # budget. Never retry network traffic or reconnect in pressure.
                print("TLS_RESOURCE_SOFTAP_ADDRESS_QUERY_TIMEOUT")
                continue
            values = result.stdout.split()
            if result.returncode == 0 and len(values) == 1:
                address = ipaddress.IPv4Address(values[0])
                if address in network and address != self.ap_address:
                    return str(address)
            time.sleep(0.25)
        raise RuntimeError("Windows SoftAP profile has no current DHCP address")

    def close(self) -> None:
        print(f"TLS_RESOURCE_SOFTAP_TIMELINE={datetime.now(timezone.utc).isoformat()},HOLD_RELEASE_REQUEST", flush=True)
        self.hold.unlink(missing_ok=True)
        try:
            if self.process is not None:
                try:
                    self.process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    # Preserve the log and profile-cleanup failure as a gate
                    # error instead of terminating before the helper's finally.
                    raise RuntimeError("temporary WLAN helper cleanup timed out")
                text = self.log.read_text(encoding="utf-8", errors="replace")
                for line in text.splitlines():
                    if line.startswith("WINDOWS_SOFTAP_UDP_CLIENT_"):
                        print(line)
                if self.process.returncode:
                    raise RuntimeError("temporary WLAN helper did not complete cleanup")
                if "WINDOWS_SOFTAP_UDP_CLIENT_PROFILE_REMOVED" not in text:
                    raise RuntimeError("temporary WLAN profile removal was not confirmed")
        finally:
            if self.output is not None:
                self.output.close()
            connect_when = getattr(self, "connect_when", None)
            if connect_when is not None:
                connect_when.unlink(missing_ok=True)
