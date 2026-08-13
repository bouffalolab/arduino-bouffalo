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
