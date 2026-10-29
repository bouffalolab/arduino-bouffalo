"""Validate complete BLE/TLS diagnostic captures without rejecting signed errors."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from run_ra4_ble_windows_long_gatt_gate import validate_heap_boundaries


def main():
    rows = []
    for cycle, stages in (
        (0, ("BEFORE_BLE", "AFTER_BLE", "ADVERTISING")),
        (1, ("BEFORE_TLS", "TLS_CONNECTED", "TLS_STOPPED")),
    ):
        for stage in stages:
            values = [6, -9984] + [0] * 17
            rows.append(f"BLE_TLS_COEX:HEAP_BOUNDARY={cycle},{stage},"
                        + ",".join(map(str, values)))
    validate_heap_boundaries(rows, 1)
    invalid = [rows[:-1], rows[::-1], rows + rows[-1:],
               [row.replace("6,-9984", "-1,-9984") for row in rows],
               [row + ",0" for row in rows],
               [row.replace("6,-9984", "6,bad") for row in rows]]
    for capture in invalid:
        try:
            validate_heap_boundaries(capture, 1)
        except (RuntimeError, ValueError):
            continue
        raise AssertionError("invalid diagnostic capture accepted")
    print("BLE_TLS_HEAP_BOUNDARIES_PASS")


if __name__ == "__main__":
    main()
