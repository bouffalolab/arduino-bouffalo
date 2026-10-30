#!/usr/bin/env python3
"""Keep the BL616CL hostname/IPv6 final-carrier gate on its native path."""

from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
WIFI_SERVICE = ROOT_DIR / "UNOR4BL616CLBridge" / "wifi_service.cpp"
WIFI_BACKEND = ROOT_DIR / "UNOR4BL616CLBridge" / "bouffalo_wifi_backend.cpp"
RUNNER = ROOT_DIR / "tools" / "run_bl616cl_hostname_ipv6_final_carrier.py"


def require(source: str, text: str, message: str) -> None:
    if text not in source:
        raise RuntimeError(message)


def main() -> int:
    service = WIFI_SERVICE.read_text(encoding="ascii")
    backend = WIFI_BACKEND.read_text(encoding="ascii")
    runner = RUNNER.read_text(encoding="ascii")

    require(service, 'kHostnameCommand[] = "+HOSTNAME"', "WiFi service lacks HOSTNAME")
    require(service, 'kIpv6Command[] = "+IPV6"', "WiFi service lacks IPV6")
    require(service, "handle_hostname(command, transport)", "HOSTNAME is not dispatched")
    require(service, "handle_ipv6(command, transport)", "IPV6 is not dispatched")
    require(backend, "netif_set_hostname(netif, sta_hostname)", "hostname is not applied to STA netif")
    require(backend, "wifi_sta_ipv6_enable(1)", "IPv6 is not enabled through the SDK")
    require(backend, "ip6_addr_islinklocal(candidate)", "IPv6 readback is not link-local")
    require(runner, "load_private_credentials", "gate must load credentials privately")
    require(runner, "BL616CL_HOSTNAME_WRITE_READBACK_PASS", "gate lacks hostname readback evidence")
    require(runner, "BL616CL_HOSTNAME_STA_RECONNECT_READBACK_PASS", "gate lacks post-connect hostname evidence")
    require(runner, "BL616CL_IPV6_LINK_LOCAL_PASS", "gate lacks IPv6 evidence")
    require(runner, "BL616CL_HOSTNAME_RESTORED", "gate must restore hostname state")
    require(
        runner,
        'WIFI_MODE_RESPONSE = b"+WIFIMODE: 1\\r\\n\\r\\nOK\\r\\n"',
        "gate must retain the ESP32-compatible WIFIMODE response framing",
    )
    require(
        runner,
        'parser.add_argument("--response-timeout-ms", type=int, default=10000)',
        "gate must allow the native IPv6 netif request its verified response window",
    )
    require(runner, "RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS", "gate must restore scan")
    print("BL616CL_HOSTNAME_IPV6_SOURCE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
