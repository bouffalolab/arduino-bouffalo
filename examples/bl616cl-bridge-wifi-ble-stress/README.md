# BL616CL Wi-Fi + BLE Bounded Stress Smoke

This standalone DK-board smoke extends the three-cycle coexistence check into
ten bounded Wi-Fi scan + BLE HCI Reset cycles.

For every cycle it requires both a Wi-Fi scan completion event and an HCI Reset
Command Complete event. It records:

- scan completion time;
- HCI completion time;
- complete-cycle time;
- current aggregate free heap bytes from `kfree_size(0)`;
- lowest observed aggregate free heap bytes.

It emits scan counts only, never SSIDs, BSSIDs, credentials, or packet
payloads. It does not connect to an AP or access RA4M1.

Run:

```bash
./test-bl616cl-bridge-wifi-ble-stress.sh
```

Success requires the final marker:

```text
BL616CL_SMOKE:COEX_STRESS_DONE cycles=10
```

The heap figures are a bounded smoke-test observation, not a full memory-leak
proof. This test does not validate BLE advertising/connection traffic, Wi-Fi
STA connectivity, USB composite operation, or RA4M1 ArduinoBLE compatibility.
