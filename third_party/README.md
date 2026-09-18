# Third-party Source Inputs

`bouffalo_sdk/` is a pinned Bouffalo SDK Git submodule used by the BL616CL
runtime-bundle generator. It is intentionally absent until initialized; run:

```bash
python3 tools/prepare_bouffalo_sdk_source.py --init
```

The initializer first validates that the root Git index has the tracked
submodule gitlink, then uses `bouffalo_sdk.lock.json` and sparse checkout to
materialize only the bridge build closure and its required recursive
submodules. `--check` validates root/submodule commits, remote URLs and tracked
cleanliness.

Do not edit files inside `third_party/bouffalo_sdk/`. Project-owned board
overlays, defconfig and bundle-generation compatibility work live under
`hardware/bouffalo/bl616cl/tools/runtime_bundle/`; future vendor-source patches
must be explicitly recorded there rather than applied in place.
