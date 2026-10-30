#!/usr/bin/env python3
"""Keep passive STA disconnect/reconnect wiring covered by source gates."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
wifi = (ROOT / "UNOR4BL616CLBridge" / "wifi_service.cpp").read_text(encoding="ascii")
backend = (ROOT / "UNOR4BL616CLBridge" / "bouffalo_wifi_backend.cpp").read_text(encoding="ascii")

for required in (
    "WifiEventCode::Disconnected",
    "WifiEventCode::GotIp",
    "auto_reconnect_enabled_",
    "backend_.set_auto_reconnect",
    "backend_.start_sta_connect",
    "status_tracker_.on_event(WifiStatusEvent::StaDisconnected)",
):
    assert required in wifi, f"STA recovery wiring missing: {required}"

for required in (
    "wifiMgmr.disable_autoreconnect = auto_reconnect ? 0U : 1U",
    "wifiMgmr.disable_autoreconnect = enabled ? 0U : 1U",
    "wifi_mgmr_sta_connect",
):
    assert required in backend, f"native reconnect wiring missing: {required}"

print("BL616CL_STA_RECOVERY_SOURCE_PASS")
