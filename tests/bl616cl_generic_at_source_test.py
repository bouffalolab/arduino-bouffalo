#!/usr/bin/env python3
"""Check generic ESP32-compatible AT command semantics on BL616CL."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVICE = (ROOT / "UNOR4BL616CLBridge" / "ra4_at_service.cpp").read_text(encoding="ascii")
RECOVERY = (ROOT / "UNOR4BL616CLBridge" / "bl616cl_recovery_control.cpp").read_text(encoding="ascii")

for command in ("+RESET", "+RESTARTBOOTLOADER", "+EXIT"):
    assert command in SERVICE, f"missing {command}"
assert "bl616cl_reset_request()" in SERVICE
assert "bl616cl_recovery_request()" in SERVICE
assert "command.mode != at::CommandMode::Run" in SERVICE
assert "serial_.flush()" in SERVICE
assert "uart_.diagnostics().buffered_tx_bytes == 0U" in SERVICE
assert "pending_reset_" in SERVICE
assert "reset_requested" in RECOVERY
assert "kResponseDrainWindowMs" in RECOVERY
assert "request_ready_at" in RECOVERY
assert "bl616cl_recovery_window_elapsed" in RECOVERY
print("BL616CL_GENERIC_AT_SOURCE_PASS")
