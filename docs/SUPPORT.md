# Support and acceptance scope

Scope: Linux x86_64, BL616CL UNO R4 WiFi module 1V0, default 4 MiB flash layout.
The platform is version 0.1.0 with libs 0.10.0, flash tools 0.2.0 and GCC RISC-V 14.3.0.
The table's hardware evidence was recorded with libs 0.9.0 and Xuantie 2.6.1. The
GCC 14.3.0 build has so far been retested on hardware for Wi-Fi STA networking,
C library/TLS runtime checks, and bridge USB/CMSIS-DAP/BLE start only.
RA tests use Renesas UNO 1.6.0 / WiFiS3 and ArduinoBLE 2.1.0 where applicable.
Hardware results remain bound to the firmware hashes in the release inventory.

`supported` describes the verified installation/build contract. Firmware features
remain `experimental` even when the listed hardware scenario passed. `unimplemented`
means unavailable; compilation or a compatibility placeholder does not change that.

| Capability | Status | Hardware evidence and boundary |
| --- | --- | --- |
| Standard package installation, dependency selection, compile and upload | supported in the declared combination | Empty CLI installations, SDK-hidden/read-only builds; separate real uploads and user IDE observations |
| CDC, vendor HID, CMSIS-DAP/SWD and RA4M1 upload | experimental, tested | Real RA4M1 endpoint; other hosts/mappings/frequencies not inferred |
| Interrupted write, physical power removal and ROM recovery | experimental, tested | Interrupted FW0 recovery and data/other-region identity verified; historical upload errors remain |
| External-host HCI, central/peripheral GATT | experimental, tested | Real RA4M1 and AX200; seven official examples passed, five lack accessories/adaptation |
| Pairing, encryption and RAM-key reconnection | experimental, tested subset | Public addresses; BlueZ without IdKey requires the explicit ArduinoBLE 2.1.0 RA patch |
| Wi-Fi STA/AP, hidden AP, address control and reconnect | experimental, tested | Concurrent STA/AP must use one channel; default AP limit five has four usable association slots |
| IPv4 TCP server/client, TLS and mutual TLS | experimental, tested | Eight shared sockets; no fixed two-TLS-session guarantee; certificate date checks are disabled |
| UDP unicast/broadcast/multicast | experimental, tested | Four UDP objects, 1460-byte datagrams; delivery is not guaranteed |
| SNTP, GETTIME, hostname and DHCP hostname | experimental, tested | Hostname is RAM state; clock synchronization does not enable TLS date validation |
| LittleFS/File/Preferences | experimental, tested | Shared 416 KiB; full-disk/reclaim/reset persistence and recovery/data protection tested |
| Combined USB/BLE/Wi-Fi/storage | experimental, bounded observation | About 1 h 56 min, 18,843 checks with zero failures; not a 24-hour claim |
| PhoneMatrix on RA4M1 | experimental, tested | Eleven phone scenarios on Xiaomi 14 Pro, HyperOS 3.0.31, nRF Connect 4.29.1; Android major version not independently verified |
| Windows/macOS, 1V1, other chips/boards | unaccepted | No support claim from Linux builds or copied pin mappings |
| OTA, independent full SOFTSE, keyboard/mouse HID | unimplemented / out of this release | Vendor HID is a DAP transport, not a keyboard/mouse |
| General ADC/PWM/interrupt/bus APIs and C++ threading | unimplemented or unaccepted | Consult the platform README; bridge acceptance is not full generic-platform acceptance |

Missing official BLE cases: LedControl and ButtonLED need button wiring;
BatteryMonitor needs a defined A0 input; EncryptedBatteryMonitor also needs
board/security adaptation; SensorTagButton needs a TI CC2650 SensorTag.
All twelve examples compiling does not mean all twelve passed on hardware.

No RPA, power-persistent bond recovery, arbitrary partition migration, cross-channel
STA/AP, or indefinite stability is claimed. ArduinoBLE transport `end()` is empty
in the pinned version; explicitly issue HCIEND to release the Controller. Timed
`BLE.poll(timeout)` and the original malformed wait command are unaccepted.

CI compiles Blink, SerialEcho, WiFiScan, ControllerUSB, LittleFS Persistence,
Preferences Counter and the complete bridge. Host protocol tests use virtual AP,
socket, storage and unencrypted TLS substitutes. These are auxiliary checks and
cannot establish radio behavior, TLS validation or flash persistence.
