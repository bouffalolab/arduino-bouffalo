# 0.1.0 release review

This is the reviewed initial BL616CL platform/bridge combination, distinct from the
repository's legacy `v0.1` tag. Publication requires owner approval. The public
release inventory identifies exact source commits, dependency origins and assets;
never infer binary identity from a matching version string alone.

- Platform 0.1.0; prebuilt libs 0.9.0; flash tools 0.2.0; Xuantie GCC V2.6.1.
- SDK commit `1cd1fd514df7ed0dfc1221be871f5aae03f31bbc`, with four pinned patches.
- Original hardware-tested bridge binary: 1,096,640 bytes, SHA-256
  `9d345c2d1efb2fccfc2f44d076a4c4627c5365737b462156c738677bd350d33c`.
- That BL binary was built from bridge commit `456feb808f5b80aa2115cf7f501955c90db43781`.
  Later source comments, RA compatibility material, phone example and CI are
  separate source changes; the inventory preserves both identities.
- Renesas core 1.6.0 and ArduinoBLE 2.1.0 are the RA-side reference combination.

## Known limitations

1. Three historical USB uploads failed among 77 attempts: two handshakes and one
   CRC. A separate later set of 100 uploads passed. Root cause is unresolved;
   the owner accepted this as a Low, nonblocking known issue. Do not combine the
   sets into a claimed production failure rate. Escalate unrecoverable or data-loss cases.
2. Original ArduinoBLE 2.1.0 can fail BlueZ public-address bonded reconnection
   when the peer supplies no IdKey. Apply the exact, opt-in RA compatibility patch
   when needed. Eleven patched reconnections passed; keys were only in RAM.
3. STA and AP share one channel. Stop provisioning AP before joining a different
   channel, or configure a compatible same-channel topology. An AP-first client
   session can be disrupted when the module's STA joins another channel.
4. TLS certificate validity dates are not checked. Software private-key slots
   are not a hardware security element. Eight sockets share constrained memory;
   no fixed number of concurrent TLS handshakes is guaranteed.
5. File AT writes are at most 255 bytes per frame, reads at most 1023 bytes.
   Preferences values are at most 1023 bytes; namespace/key names are 1–15 bytes,
   with 64 logical key slots. Actual shared free space can run out first.
6. LittleFS mount failure does not automatically format data. Failed file writes
   or appends are not promised to roll back. Atomic Preferences replacement needs
   temporary free space. Custom layouts do not imply automatic data migration.
7. Bridge firmware version 0.1.0 is independent of the official ESP32 bridge version.
   Do not use an ESP32 connectivity updater or ESP32 image on BL616CL.
8. Counter prints shortly after boot; opening Serial Monitor late can miss the
   boot line. The manual IDE test used an explicitly recorded monitor-wait change;
   that convenience change is not silently included in the distributed example.

The original soak ran until the owner-requested 16:00 endpoint, about 1 h 56 min.
18,843 checks passed. An earlier interrupted cleanup had an END timeout; three
normal-end rounds later passed 265 checks. Physical power-removal recovery passed.
These observations do not prove unlimited lifetime or removal of every historical fault.

PhoneMatrix's accepted RA binary SHA-256 is
`deffba9de2cadaa0bb587af2862d05fcad8efe8d256601b6cd1b57dce12eb248`.
Its eleven manual phone cases are independent of the later GUI-uploaded RA image.
See [support](SUPPORT.md), [recovery](RECOVERY.md), and [licensing](LICENSING.md).
