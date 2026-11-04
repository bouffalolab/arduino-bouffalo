# WiFi for Arduino BL

This experimental platform library implements the Arduino WiFiS3/WiFiNINA-style interface on
BL616CL. Include `WiFi.h` for STA/AP, scan, addressing, DNS and ping, and
`WiFiClient`, `WiFiServer`, `WiFiUDP` and `WiFiSSLClient` for network I/O.
The current local candidate pairs platform 0.1.0 with `bl616cl-libs` 0.11.0 and
`bl-flash-tools` 0.2.1 on Linux x86_64. The board is the BL616CL UNO R4 WiFi 1V0;
other host systems and the 1V1 module have not been accepted.

## Ownership and use

The C++ API owns Arduino compatibility semantics. Private `wifi_port`,
`net_port` and `tls_port` interfaces adapt the fixed SDK's wl80211, lwIP and
mbedTLS. Generic networking belongs here; AT commands and RA4M1 policy belong
to the separate bridge application. Serialize calls for each connection in
application task context. Client copies share a socket and do not close it
on destruction: call `stop()` explicitly. A server retains clients returned
by `available()`; `accept()` transfers a new connection to the caller.

STA and AP must use the same channel for accepted simultaneous operation.
DHCP hostname, SNTP epoch time and STA link-local IPv6 are supported; this does
not promise routed IPv6 or cross-channel STA/AP. UDP datagrams are bounded to
1460 bytes. Multicast explicitly rejects `224.0.0.1`, the SDK's implicit
all-hosts group. See `examples/WiFiMulticast` and `examples/WiFiTime`.

TLS verifies chain trust and the peer hostname using bundled or explicit PEM
roots. Certificate validity dates remain unchecked under the existing policy,
even after SNTP synchronization. Client identities use software PEM keys;
this does not provide a secure element. Each TLS session uses substantial heap,
so connection success does not promise a fixed concurrent session count.
Credential pointers supplied by the sketch must remain valid until replaced
and while connecting. Read the public headers for return values and timeouts.

## Build integration

`library.properties` selects a response file in the fixed dependency tool
package with `precompiled=true` and `ldflags`. Arduino CLI 1.5.1 requires a
nonempty `src/bl616cl/` directory before appending those flags; its marker is
intentional. Actual archives, headers, ABI flags and startup resources belong
to `bl616cl-libs`, not this library directory. This directory gate is an
implementation-dependent compatibility choice, distinct from the documented
`src/{build.mcu}/lib*.a` bundled-library layout. Link/map checks must be repeated
when upgrading Arduino CLI. Ordinary sketches never build the SDK.

## Evidence and licensing

Individual Wi-Fi, network, time and multicast hardware results are recorded in
ARDUINO-45 and ARDUINO-49 through ARDUINO-51; the unified local-candidate
regression belongs to ARDUINO-53. Build, host-socket and fake checks are
auxiliary evidence. This README does not create new hardware acceptance.
The library follows the repository's LGPL-2.1 license, reproduced in `LICENSE`.
SDK libraries and manufacturer tools retain their separately recorded licenses.
