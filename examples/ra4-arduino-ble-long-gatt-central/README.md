# RA4M1 ArduinoBLE Long GATT Central

This sketch scans for the long-GATT peripheral, connects, discovers attributes,
then performs eight repeated transactions on one connection. Each transaction
contains a 512-byte read, a 512-byte write using ATT Prepare Write and Execute
Write, and a 512-byte readback.

The runtime output contains only fixed markers, lengths, and checksums. It does
not print BLE addresses or characteristic payloads.
