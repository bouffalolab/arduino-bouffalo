#!/usr/bin/env python3
"""Audit that every WiFiS3 AT command has a BL616CL service owner."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
commands = (Path.home() / ".arduino15/packages/arduino/hardware/renesas_uno/1.6.0/libraries/WiFiS3/src/WiFiCommands.h").read_text(encoding="ascii")
sources = "\n".join(
    path.read_text(encoding="ascii")
    for path in (ROOT / "UNOR4BL616CLBridge").glob("*.cpp")
)

names = re.findall(r'^#define\s+_[A-Z0-9_]+\s+"(\+[A-Z0-9_]+)"', commands, re.MULTILINE)
missing = [name for name in names if name not in sources]
assert not missing, "WiFiS3 commands without BL616CL source owner: " + ", ".join(missing)

for required in ("+WIFISCAN", "+MACSTA", "+IPCLIENT", "+CLIENTSTATUS", "+SSLIPCLIENT", "+CLIENTRECEIVE", "+UDPREAD", "+HCIBEGIN"):
    assert required in names and required in sources, f"missing required command {required}"

print(f"BL616CL_WIFIS3_API_COVERAGE_PASS commands={len(names)}")
