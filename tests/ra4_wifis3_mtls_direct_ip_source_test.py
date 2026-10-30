#!/usr/bin/env python3
"""Keep the mTLS direct-IP diagnostic path aligned between fixture and runner."""

from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
TEMPLATE = (
    ROOT_DIR
    / "examples"
    / "ra4-wifis3-mtls-client-smoke"
    / "ra4-wifis3-mtls-client-smoke.ino.in"
)
RUNNER = ROOT_DIR / "tools" / "run_ra4_wifis3_mtls_gate.py"


def require(source: str, text: str, message: str) -> None:
    if text not in source:
        raise RuntimeError(message)


def main() -> int:
    template = TEMPLATE.read_text(encoding="ascii")
    runner = RUNNER.read_text(encoding="ascii")
    require(
        template,
        'constexpr char kIpPrefix[] = "STARTIP ";',
        "mTLS fixture must recognize STARTIP",
    )
    require(
        template,
        "RA4_MTLS_CLIENT:DNS_BYPASS",
        "mTLS fixture must expose direct-IP DNS bypass",
    )
    require(
        template,
        "RA4_MTLS_CLIENT:TCP_IP_PROBE=",
        "mTLS fixture must prove peer TCP reachability before TLS",
    )
    require(
        template,
        "? client.connect(hostname, port)",
        "mTLS fixture must retain hostname connect",
    )
    require(
        template,
        ": client.connect(dns_ip, port);",
        "mTLS fixture must use the peer IP for direct-IP connect",
    )
    require(
        template,
        'constexpr char kIpWithoutCredentialsPrefix[] = "STARTIPNOCRED ";',
        "mTLS fixture must support the direct-IP no-client-credential control",
    )
    require(
        template,
        "if (use_client_credentials) {",
        "mTLS fixture must conditionally install the client credential",
    )
    require(
        template,
        "print_tls_diagnostic();\n  Serial.println(\"RA4_MTLS_CLIENT:TLS_CONNECT_PASS\");",
        "mTLS fixture must report heap diagnostics after a successful handshake",
    )
    require(
        runner,
        '"--direct-ip"',
        "mTLS runner must expose direct-IP mode",
    )
    require(
        runner,
        '"--static-address", type=ipaddress.IPv4Address',
        "mTLS runner must allow a validated static-address override",
    )
    require(
        runner,
        '"--without-client-credentials"',
        "mTLS runner must expose the client-credential control",
    )
    require(
        runner,
        '"--optional-client-certificate"',
        "mTLS peer must accept the no-client-credential control",
    )
    require(
        runner,
        "str(host) if args.direct_ip else TLS_HOSTNAME",
        "mTLS peer must expect IP SNI in direct-IP mode",
    )
    require(
        runner,
        "x509.IPAddress(server_ip)",
        "mTLS server certificate must include the direct-IP SAN",
    )
    require(
        runner,
        "RA4_MTLS_CLIENT:TCP_IP_PROBE=PASS",
        "mTLS runner must require the TCP reachability probe",
    )
    print("RA4_WIFIS3_MTLS_DIRECT_IP_SOURCE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
