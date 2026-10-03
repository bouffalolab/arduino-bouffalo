# Vendor BLE controller archives

Everything else in the Arduino runtime is compiled from
`tools/sdk/bouffalo_sdk` at Arduino build time.  The archives in this
directory are the exception: they are closed-source controller builds that the
public SDK release does not ship.

## bl616cl/libbtblecontroller_bl616cl_uarthci.a

- Version string: `lib_version_btblecontroller_1.6.208`
- SHA-256: `d2b948e13209a78faad86f43ec14b3e65cfd7fc5664580e3118b05144661ffec`
- Size: 11243864 bytes

The public `bouffalo_sdk` v2.3.35 release ships only the `m0b1` and `m2s1`
BL616CL controller archives (`components/wireless/bluetooth/btblecontroller/lib/`).
`uarthci` (HCI-over-UART transport) is required by the Arduino HCI bridge and
is built by the BLE team from the private bluetooth component
(`bouffalo/components/bluetooth` on the Bouffalo Gerrit).  Replace this file
when the BLE team publishes a new build; keep the version string and SHA-256
in `manifest.json` in sync.

`build_sdk_runtime.py` stages this archive into the SDK tree for the duration
of the probe link (the SDK's Kconfig links
`libbtblecontroller_{chip}_{variant}.a` unconditionally) and removes it again,
so the submodule checkout stays clean.
