# RA4M1 SWD Flash Loader

This directory contains a small project-controlled Thumb loader that runs from
RA4M1 SRAM. It implements only the Renesas RA4M1 `FLASH_LP` version 3
code-flash erase/program/exit sequence needed by the D7 validation.

The source is based on the public Renesas FSP implementation
`r_flash_lp.c`. It is intentionally not a copy of the complete FSP driver:
there is no FSP control block, interrupt path, startup code, libc, Arduino
runtime, data Flash operation, access-window operation, ID-code operation, or
bank-swap operation.

Build it with:

```bash
python3 tools/ra4_swd_flash_loader/build_ra4_swd_flash_loader.py
```

The loader is linked at `0x20004000` and uses no Flash-resident code. The
host-controlled mailbox is normally placed at `0x20006000`, a 2 KiB staging
buffer at `0x20006800`, and the temporary loader stack uses `0x20007C00` as
its initial SP.

Supported operations are one 2 KiB Code Flash erase, a range of aligned 8-byte
program operations, and cleanup/return-to-read-mode. All addresses are checked
again inside the loader and are limited to RA4M1 Code Flash
`0x00004000..0x0003FFFF` (the Arduino application image starts at `0x4000`).
Data Flash, configuration words, access-window
settings, boot configuration, and BL616CL Flash are outside the loader's
command surface.

The D7 host runner writes a known RA4M1 application, verifies a full SWD
readback, confirms its CDC runtime marker, then restores the standard WiFiS3
scan Sketch:

```bash
python3 tools/run_bl616cl_swd_d7_flash_program.py
```

For the D8 repeated-programming subgate, use the bounded two-cycle runner:

```bash
python3 tools/run_bl616cl_swd_d8_repeat_program.py --cycles 2
```

The D8 runner also performs a controlled stop after a completed mailbox
operation, when the loader has already left Flash P/E mode. It runs a complete
SWD program/verify recovery directly against that incomplete application,
then restores the standard scan Sketch once at the end. It intentionally does not simulate power loss or terminate an
active Code Flash P/E operation.

Every session starts with reset-halt. The loader establishes HOCO 48 MHz,
ICLK 48 MHz, FCLK 24 MHz and the required Flash read wait state, independently
of the interrupted application. It checks the UNO R4 OFS1 clock selection but
never changes option bytes. Both host and loader reject bootloader addresses.
Failure to reach READY, complete a mailbox command, compare readback, run the
marker or restore scan fails the gate.

The D8 bounded long-session gate uses five complete cycles plus the final
recovery session:

```bash
python3 tools/run_bl616cl_swd_d8_repeat_program.py --cycles 5 --long-session
```
