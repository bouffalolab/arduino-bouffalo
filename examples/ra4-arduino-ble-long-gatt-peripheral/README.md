# RA4M1 ArduinoBLE Long GATT Peripheral

This sketch advertises a readable and writable 512-byte characteristic. The
paired central sketch performs eight repeated read/write/readback transactions;
the peripheral validates each committed generation and reports the cumulative
write count.

The runtime output contains only fixed markers, lengths, and checksums. It does
not print BLE addresses or characteristic payloads.
