# RA4M1 SWD SRAM scratch

This temporary Sketch provides a dedicated, linker-visible SRAM buffer for
CMSIS-DAP D6 validation. The buffer is initialized once during `setup()` and
is not accessed by `loop()`, so SWD can write it while the RA4M1 core is
halted without touching the application's stack, heap, vector table, or other
runtime state.

Compile and inspect the address:

```bash
arduino-cli compile \
  --fqbn arduino:renesas_uno:unor4wifi \
  --build-path build/ra4-swd-sram-scratch \
  examples/ra4-swd-sram-scratch

arm-none-eabi-nm -n \
  build/ra4-swd-sram-scratch/ra4-swd-sram-scratch.ino.elf \
  | grep ' swd_scratch$'
```

The Sketch can be uploaded through the existing final-carrier SAM-BA path.
Uploading replaces the current RA4M1 Sketch temporarily; it does not write
BL616CL Flash. After D6, restore `ra4-wifis3-scan-smoke`.

The D6 runner writes only the 64-byte region reported by the
`RA4_SWD_SCRATCH:ADDR=` runtime marker and verifies every word by SWD
readback. It never writes RA4M1 Flash.
