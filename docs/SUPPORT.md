# Support and acceptance scope

Scope: Linux x86_64, BL616CL UNO R4 WiFi module 1V0, default 4 MiB flash layout.
The platform is version 0.2.0 with libs 0.12.0, flash tools 0.3.0 and GCC RISC-V 14.3.0.
The exact 0.2.0 release archives have not been retested on hardware.
The table's hardware evidence was recorded with libs 0.9.0 and Xuantie 2.6.1. The
GCC 14.3.0 build (libs 0.11.0, bouffalo_sdk v2.3.38) has so far been retested on
hardware for Wi-Fi STA networking, C library and per-task TLS runtime checks, HTTPS,
bridge USB/CMSIS-DAP/BLE start, RA4M1 upload through the bridge, and the RA WiFiS3
AT network matrix (DHCP, TCP, TLS, mTLS, timeouts). Two of three matrix runs passed
all 139 checks; one stopped after an 8 s TLS handshake stall, root cause not determined.
RA tests use Renesas UNO 1.6.0 / WiFiS3 and ArduinoBLE 2.1.0 where applicable.
Hardware results remain bound to the firmware hashes in the release inventory.

`supported` describes the verified installation/build contract. Firmware features
remain `experimental` even when the listed hardware scenario passed. `unimplemented`
means unavailable; compilation or a compatibility placeholder does not change that.

| Capability | Status | Hardware evidence and boundary |
| --- | --- | --- |
| Standard package installation, dependency selection, compile and upload | supported in the declared combination | Empty CLI installations, SDK-hidden/read-only builds; separate real uploads and user IDE observations |
| CDC, vendor HID, CMSIS-DAP/SWD and RA4M1 upload | experimental, tested | Real RA4M1 endpoint; other hosts/mappings/frequencies not inferred |
| Hardware UART `Serial`/`Serial1` (`HardwareSerial`) | experimental, tested | Ai-M64P-32S-Kit, libs 0.12.0 and tools 0.3.0: UART1 GPIO14-GPIO15 loopback 9,600 to 2,000,000 baud with 4 KiB each; all 24 Arduino `SERIAL_*` formats plus 1.5 stop bits checked bit by bit on the wire; UART0 exchanged all 24 formats and 9,600 to 2,000,000 baud with the onboard CH340. See the limits below |
| Hardware timers `BLTimer` (TIMER0/TIMER1 periodic and one-shot callbacks) | experimental, tested | Ai-M64P-32S-Kit, libs 0.12.0: periods 10 µs to 1 s exact to the tick against `micros()`, match-to-callback latency at most 9 µs with Serial traffic, one-shot, stop/start/period change, two timers and invalid-request failures; not measured on a pin or on UNO R4 WiFi. See [BLTimer](../libraries/BLTimer/README.md) |
| Analog input `analogRead()` (Ai-M64P-32S-Kit A0–A9) | experimental, partially tested | Ai-M64P-32S-Kit, libs 0.12.0, no external voltage source: channel mapping of all ten inputs, resolution scaling and zero padding, steady-input noise below 1 LSB at 12 bits, `-1` for other pins, and readings while a soft AP or scans run. Zero and full scale per channel with external GND/3V3 and accuracy at intermediate voltages are unaccepted. See the analog limits below |
| PWM output `analogWrite()` (Ai-M64P-32S-Kit GPIO0–31) and the platform extension `analogWriteFrequency()` | experimental, tested | Ai-M64P-32S-Kit, libs 0.12.0, sampled on the board through its GPIO8-GPIO22, GPIO14-GPIO15, GPIO18-GPIO19 and GPIO20-GPIO23 jumpers: 1 kHz by default, duty within 0.01 % of full scale from 8 to 31 bits, constant levels at 0 and full scale, three channels at once with a shared period, the GPIO % 4 channel map on GPIO0–7, GPIO12 and GPIO13 by ADC sampling, refused requests and return to digital use. `analogWriteFrequency()` at 1, 50, 490, 610, 611 and 1000 Hz, 20 kHz, 100 kHz and 1 MHz programmed the documented divider and period; duty was within 0.01 % up to 1 kHz and, above that, within the about 65 ns sampling step; running pins kept their duty across a change, and 0, 1000001 and 4294967295 Hz were ignored. Edges and frequency were not measured with an instrument; the other pins follow the datasheet map. See the PWM limits below |
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
| BL616CL application OTA | experimental development build | Ai-M64P-32S-Kit Wi-Fi A/B, serial upload and storage tested with libs 0.12.0 and tools 0.3.0; partial-write power interruption tested; RA4 bridge unaccepted; see [BLUpdate](../libraries/BLUpdate/README.md) |
| Independent full SOFTSE | unimplemented / out of this release | Not accepted in this platform release |
| Keyboard/mouse HID | unimplemented / out of this release | Vendor HID is a DAP transport, not a keyboard/mouse |
| General interrupt/bus APIs, `tone()` and C++ threading | unimplemented or unaccepted | Consult the platform README; bridge acceptance is not full generic-platform acceptance |

UART limits: the UART clock is 40 MHz and the port runs at 40 MHz / round(40 MHz / baud).
`begin()` rejects requests whose divisor falls outside 2 to 65,534 (below about 611 baud or
above about 26.6 MBd; divisor 1 corrupted data). Rejected settings, including MARK/SPACE
parity, leave the port closed and `operator bool` false. Hardware flow control, UART2 and
receive parity/framing error reporting are unimplemented. The RX ring defaults to 256 bytes
(`setRxBufferSize()`, `rxOverflowCount()`); writes block once the 32-byte TX FIFO is full.
UNO R4 WiFi `Serial1`/`Serial2` share this code but were not retested on that board.

Analog limits (Ai-M64P-32S-Kit): A0–A9 are GPIO0–GPIO7, GPIO12 and GPIO13 (ADC channels 0–9;
headers J2-2, J2-3, J2-16, J1-13, J1-3, J1-4, J2-19, J1-20, J1-5 and J1-6). The single-ended range is
0 to 3.2 V from the internal reference, the only `analogReference()` mode (`AR_DEFAULT`); other
modes are ignored. Keep inputs between 0 V and the 3.3 V supply. Inputs at or above 3.2 V read the
top calibrated code, which depends on the chip's gain trim (1021 at 10 bits on the tested board), not
exactly 1023. `analogReadResolution()` accepts 1 to 31 bits (default 10) and ignores other values;
the converter runs its 16-bit oversampled mode with 12 effective bits, so wider results are padded
with zero low bits. Values are corrected with the efuse offset and gain trims and with ground and
20 mV points the driver measures once, during the first `analogRead()` (about 7 ms); later calls take
about 0.23 ms. `analogRead()` switches the pin to analog input, so call `pinMode()` before digital use;
other pins return -1 and stay unchanged. Do not call it from interrupts or from several tasks at once.
The input is high impedance: a floating pin keeps a drifting charge. With a soft AP or scans
running, this board's 3.3 V supply dipped to between 2.9 and 3.2 V in two runs, and readings near
40 mV moved by up to about 50 mV (standard deviation 1.2 to 2.9 LSB at 12 bits, 0.4 LSB idle).
UNO R4 WiFi declares no analog inputs; `analogRead()`
returns -1 there. The IDE's built-in analog examples such as AnalogReadSerial work with A0–A9.

PWM limits (Ai-M64P-32S-Kit): `analogWrite()` drives GPIO0–GPIO31 from the PWM0 controller, at
1 kHz by default (40 MHz crystal clock divided by 40000). `analogWriteFrequency(hz)` is a platform
extension, not part of the Arduino API; it has the same form as in STM32duino. It sets 1 Hz to 1 MHz
for every PWM pin at once and ignores other values. It divides the clock by the smallest d (1 to 611)
that keeps a period within 65535 counts, so a period has N = round(40 MHz / (d × hz)) steps and the
real frequency is 40 MHz / (d × N): 50 Hz gives 50.000375 Hz with 61538 steps, 490 Hz gives
490.0039 Hz, 20 kHz has 2000 steps and 1 MHz has 40. The call waits for the current period to end,
up to about 1 s at 1 Hz; then the counter restarts and running pins keep their duty, recomputed for
the new period. The Core selects the single-ended pin routing, in which GPIO n carries the active-high output of channel n % 4 (the table
is in the variant's `pins_arduino.h`). The four channels share one counter, so every output has the
same frequency and rises at the same time, and each channel drives one pin at a time. A request on
another pin of a channel in use, on GPIO32–GPIO36 (USB, `Serial` and boot pins) or on a pin outside
the table is ignored and leaves that pin unchanged; `analogWrite()` returns no status. The duty is
round(value × N / (2^bits − 1)) / N, where `analogWriteResolution()` takes 1 to 31 bits (default 8)
and ignores other values, so steps are 25 ns at 1 kHz and wider values lose their low bits; values
at or below 0 hold the pin low and values at or above full scale hold it high. `pinMode()`,
`digitalWrite()` and `analogRead()` on a PWM pin stop its output and free its channel;
`digitalWrite()` leaves the pin a digital output at that level, as on AVR. PWM uses no hardware
timer, so `BLTimer` keeps TIMER0 and TIMER1; `tone()` and `noTone()` are not implemented and fail
to link. Do not call `analogWrite()` from interrupts or from several tasks at once. UNO R4 WiFi
declares no PWM pins and ignores `analogWrite()`. The IDE's built-in Fade and AnalogInOutSerial
examples use pin 9 (GPIO9, channel 1); `LED_BUILTIN` (GPIO16) is on channel 0.

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
