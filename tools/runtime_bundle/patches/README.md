# SDK patches

The runtime bundles checked in under `tools/sdk/{chip}/` are built from the
Bouffalo SDK sources at `bouffalo_sdk_full/bouffalo_sdk`.  Some SDK sources are
locally patched to fix chip bring-up issues.  Every such change is kept here so
that a fresh SDK checkout can be reproduced.

Apply a patch from this directory against the matching upstream project:

```sh
patch -p1 -d /path/to/bouffalo_sdk/drivers/lhal \
      < bflb_usb_v2-ep0-control-transfer-fixes.patch
```

After applying, rebuild the runtime bundle and copy the regenerated archives
into `tools/sdk/{chip}/` (see `../README.md`), then re-run the manifest
generator so the recorded SDK state reflects the patched sources.

## bflb_usb_v2-ep0-control-transfer-fixes.patch

Target project: `bouffalo/drivers/lhal`, file `src/bflb_usb_v2.c`
(recorded upstream commit `b39aa29`, lhal-v1.1.0-494-gb39aa29f).

Fixes observed on BL616CL while bringing up the UNO R4 USB bridge:

1. `usbd_ep_set_stall` for EP0 also writes `USB_CX_DONE`.  Without it the
   controller keeps the control FIFO wedged after a stalled control request
   and the next SETUP is read as a duplicated first word.
2. A zero-length `usbd_ep_start_read` on EP0 is a no-op.  The status stage is
   completed by the controller when the data packet finishes on the wire.
3. The VDMA completion handler writes `USB_CX_DONE` when an EP0 IN transfer
   ends with a short packet, so the controller ACKs the following OUT status
   stage at the right time (macOS enumeration depends on this).

## wifi6-lwipopts-runtime-config-fixes.patch

Target project: `bouffalo/components/wifi6`, files
`wifi6_lwip_adapter/include/lwipopts.h` and
`wifi6_lwip_adapter/tx_buffer_copy.c` (recorded upstream commit `910812db`).

The macsw TX/RX buffer counts moved to runtime configuration in macsw master,
while `lwipopts.h` still used the removed compile-time macros.  Patch the lwIP
queue sizing to the default build-time values and fix the macsw header include
order in `tx_buffer_copy.c`.  Required to build `liblwip.a` with
`CONFIG_WIFI6`/`CONFIG_FHOST` on BL616CL.

## lwip-wl80211-include-and-pbuf-fixes.patch

Target project: `bouffalo/components/net/lwip/lwip`, files `CMakeLists.txt`
and `lwip-port/config/lwipopts.h`.

wl80211 pulls the generic `lwip-port/config/lwipopts.h` through its static
asserts, but the lwIP library did not expose that directory as a public
include path.  Also raise `PBUF_LINK_ENCAPSULATION_HLEN` from 48 to 388 bytes:
wl80211's TX descriptor plus the macsw frame header exceeds the original
reservation and fails a compile-time `CTASSERT` in `wl80211_lwip_tx()`.

## wl80211-ip-got-cb-core-lock-deadlock-fix.patch

Target project: `bouffalo/components/wireless/wl80211`, file `lwip.c`
(recorded upstream commit `3c19c8d1`).

`ip_got_cb` is the STA netif status callback, invoked on the tcpip thread
while the lwIP core mutex is held.  It called
`netifapi_netif_set_default()`, which under `LWIP_TCPIP_CORE_LOCKING=1`
locks the non-recursive core mutex again and deadlocks.  The DHCP ACK was
processed and the IP assigned, but `CODE_WIFI_ON_GOT_IP` was never posted,
the 15 s mgmr DHCP watchdog then disconnected WiFi, and `netifapi_dhcp_stop`
could not complete.  Use the core-locked `netif_set_default()` instead.

## wl80211 runtime bundle selection

The BL616CL runtime bundle uses the wl80211 host stack instead of fhost
(`CONFIG_WL80211=y`, `CONFIG_BL_WPA_SUPPLICANT=y`, no `CONFIG_FHOST`).
wl80211 plus its macsw firmware and lwIP fits the board's RAM budget where the
fhost fullmac host stack did not.

Pinned component revisions used for the checked-in bundle:

- `components/wireless/wl80211` host API/libs: `3c19c8d1`
- `components/wireless/macsw` firmware: `78718af`

The wl80211 component is split between the top-level host files
(`wifi_mgmr.c`, `wl80211_platform.c`, ...) and the `src/` checkout.  The
runtime bundle builds `src/` into `libwl80211_${CHIP}.a` and the top-level
files into `libwl80211_plat.a`.  Keep `defconfig`, `autoconf.h` and both
archives in sync when regenerating.

## mbedtls-config-tls-ecp-have-curves.patch

Target project: `bouffalo/components/crypto/mbedtls`, file
`config-tls-generic.h`.

`config-tls-generic.h` enables `MBEDTLS_ECP_DP_*_ENABLED` but never derives
the `MBEDTLS_ECP_HAVE_*` names that the X.509 OID tables in `oid.c` gate on
(the default `mbedtls_config.h` does this via
`config_adjust_legacy_crypto.h`).  Without the mapping the compiled OID table
contains no named curves and parsing an ECDSA certificate public key fails
with `MBEDTLS_ERR_PK_UNKNOWN_NAMED_CURVE`.  Rebuild `libmbedtls.a` after
applying.

The sketch side uses the same `config-tls-generic.h` plus the same
`CONFIG_MBEDTLS_*` defines as the library build, wired through `platform.txt`
(mbedTLS v3 headers are staged under `tools/sdk/bl616cl/include/sdk/mbedtls`,
with the SDK's `port/hw_acc` alt headers and `mbedtls_port_bouffalo_sdk.h`).

`components/crypto/mbedtls/CMakeLists.txt` also needs
`CONFIG_MBEDTLS_ECP_DP_SECP384R1_ENABLED` in addition to SECP256R1: public
websites commonly serve chains mixing P-256 and P-384 certificates, and the
X.509 OID table / ECP group loader must know both.

## Regenerate libapp.a after changing CONFIG_WIFI6

`bsp/board/{board}/board.c` attaches the WiFi MAC IRQ
(`bflb_irq_attach(WIFI_IRQn, interrupt0_handler, NULL)`) only under
`CONFIG_WIFI6`.  The checked-in `tools/sdk/{chip}/lib_board/libapp.a` must be
copied from the same runtime-bundle build that generated `libmacsw_*.a` and
the WiFi archives.  A stale `libapp.a` (built before `CONFIG_WIFI6`) omits the
IRQ attach, the macsw task never receives the MAC idle interrupt, and the
first STA VIF add blocks forever in `MM_GOING_TO_IDLE`.
