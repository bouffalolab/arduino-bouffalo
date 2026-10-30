# BL616CL USB CDC + HID Feasibility Smoke

This DK-only smoke verifies the BL616CL native USB device path required by the
future UNO R4 bridge. It uses the project-local bridge runtime bundle and the
`bouffalo:bl616cl:bl616cl_bridge_smoke` FQBN, so its board pin mapping remains
the independent `bl616cldk` mapping.

The image deliberately contains only USB feasibility behavior:

- USB device identity: VID/PID `0x2341:0x1002`;
- one CDC ACM function, with a bounded 64-byte loopback;
- one vendor-defined HID function with 64-byte input, output, and feature
  reports;
- HID output reports are echoed as HID input reports;
- HID feature reports are stored and returned by `GET_REPORT`.

It is not a CMSIS-DAP implementation, does not access SWD or RA4M1, and does
not initialize Wi-Fi or BLE. Its HID descriptor is shaped like the existing
bridge interface: vendor usage page `0xFF00`, no report ID, and 64-byte
input/output/feature reports.

## Required physical setup

Two independent USB paths are required:

1. The CKLink-Lite USB connection for FlashCube flashing and 2 Mbaud log
   capture. The script accepts only the recorded dedicated CKLink identity.
2. A host connection to the DK's BL616CL native USB D+/D- path
   (GPIO32/GPIO33). This must enumerate separately from CKLink-Lite.

The currently observed CKLink interface alone is not a native-USB result.

## Build and hardware test

Build only:

```bash
./test-bl616cl-usb-composite-feasibility.sh
```

After the native USB port is connected to the host, flash and validate:

```bash
./test-bl616cl-usb-composite-feasibility.sh --flash
```

The hardware mode writes boot2 at `0x0`, the generated partition table at
`0xE000`, and the application at `0x10000`; it does not perform a whole-chip
erase. It then requires all of the following:

- CKLink log marker `BL616CL_USB_SMOKE:STACK_READY`;
- a native USB device with serial `BL616CL-USB-SMOKE`;
- a CDC ACM interface that echoes a short marker;
- a HID interface exposing a `hidraw` node that echoes a 64-byte report.

The host verifier also records whether the feature-report ioctl is available.
Feature-report success is required only when `--require-feature-io` is passed,
because some host environments expose the HID interface without permitting
`hidraw` feature access to an unprivileged user.
