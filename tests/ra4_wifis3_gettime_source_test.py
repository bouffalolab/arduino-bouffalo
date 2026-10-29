#!/usr/bin/env python3
"""Keep WiFiS3 getTime on the project-owned RTC/NTP compatibility path."""

from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
AT_SERVICE = ROOT_DIR / "UNOR4BL616CLBridge" / "ra4_at_service.cpp"
TIME_SERVICE = ROOT_DIR / "UNOR4BL616CLBridge" / "time_service.cpp"
PROTOCOL = ROOT_DIR / "UNOR4BL616CLBridge" / "time_sync_protocol.cpp"
SMOKE = ROOT_DIR / "examples" / "ra4-wifis3-gettime-smoke" / "ra4-wifis3-gettime-smoke.ino"
RUNNER = ROOT_DIR / "tools" / "run_ra4_wifis3_gettime_gate.py"
BUILD = ROOT_DIR / "compile-bl616cl-bridge.sh"
RUNTIME_PROFILE = (
    ROOT_DIR
    
    / "tools" / "bridge_runtime"
    / "profiles"
    / "bridge"
    / "defconfig"
)


def require(source: str, text: str, message: str) -> None:
    if text not in source:
        raise RuntimeError(message)


def main() -> int:
    at_service = AT_SERVICE.read_text(encoding="ascii")
    time_service = TIME_SERVICE.read_text(encoding="ascii")
    protocol = PROTOCOL.read_text(encoding="ascii")
    smoke = SMOKE.read_text(encoding="ascii")
    runner = RUNNER.read_text(encoding="ascii")
    build = BUILD.read_text(encoding="ascii")
    runtime_profile = RUNTIME_PROFILE.read_text(encoding="ascii")

    require(at_service, 'kGetTimeCommand[] = "+GETTIME"', "AT service lacks GETTIME")
    require(at_service, "command.mode == at::CommandMode::Write", "GETTIME must retain write mode")
    require(at_service, "command.argument_count == 0U", "GETTIME must reject unexpected arguments")
    require(at_service, "time_.epoch_seconds()", "GETTIME must read the time service")
    require(time_service, '"pool.ntp.org"', "time sync must retain the baseline NTP hostname")
    require(time_service, '"time.google.com"', "time sync must include a public NTP fallback")
    require(time_service, '"ntp.aliyun.com"', "time sync must include a regional NTP fallback")
    require(time_service, "WifiEventCode::GotIp", "time sync must wait for usable STA IPv4")
    require(time_service, "xTaskCreate(", "time sync must not block the AT task")
    require(time_service, "kNtpRetryIntervalMs", "time sync must retry after a transient failure")
    require(time_service, "retry_not_before_ms_", "time sync must defer retries without blocking")
    require(time_service, "TimeServiceDiagnostic", "time sync must expose credential-safe diagnosis")
    require(time_service, "epoch_seconds() < kUnixEpochYear2000", "time sync must verify RTC writeback")
    require(time_service, "bflb_rtc_set_utc_time", "time sync must set the BL616CL RTC")
    require(time_service, "bflb_rtc_get_utc_time", "GETTIME must validate the RTC calendar")
    require(protocol, "kUnixEpochYear2000", "time protocol must retain the baseline Y2K guard")
    require(protocol, "kNtpTransmitTimestampOffset", "time protocol must parse NTP transmit time")
    require(smoke, "WiFi.getTime()", "RA4 smoke must invoke the public WiFiS3 API")
    require(smoke, "RA4_GETTIME:RESULT=PASS", "RA4 smoke must provide a pass marker")
    require(runner, "load_private_credentials", "getTime runner must load credentials privately")
    require(runner, "RA4_WIFIS3_GETTIME_GATE_PASS", "getTime runner must provide a gate marker")
    require(runner, "RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS", "getTime runner must restore scan")
    require(
        runtime_profile,
        "CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH =y",
        "runtime bundle must preserve the TLS variable-buffer ABI",
    )
    print("RA4_WIFIS3_GETTIME_SOURCE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
