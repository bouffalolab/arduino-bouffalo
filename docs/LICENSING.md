# Sources and licenses

The root Apache-2.0 license does not replace third-party component terms. Preserve
original copyright notices and complete texts in platform libraries, dependency
packages and the release-materials archive. That archive also includes the exact
old binary source snapshots and build/relink evidence references.

| Component | Source and terms |
| --- | --- |
| ArduinoCore-API 1.5.2 | Commit `cd91833d90b4fe50e428021ba5051e2b7ceafc84`; unchanged source in `cores/bouffalo/api`, LGPL-2.1 text in `cores/bouffalo/LICENSE.ArduinoCore-API` |
| ArduinoBLE 2.1.0 | Upstream library plus the explicit, separately supplied RA compatibility patch and original LGPL text |
| Free-DAP | BSD-3-Clause notices and modification attribution in the bridge repository |
| littlefs | Pinned SDK source and original BSD-3-Clause license; builder profile/patch inputs retained |
| FreeRTOS, lwIP, mbed TLS, wpa_supplicant, CherryUSB | Original MIT/BSD/component licenses and notices retained with the dependency materials |
| bl-layout runtime | Original Python 3.9.17, PyInstaller 6.15.0 including its bootloader exception, and tomli 2.2.1 texts |
| GCC RISC-V 14.3.0 (picolibc) and vendor flash tools | Exact publisher packages and checksums; GCC license/Runtime Library Exception, picolibc texts in the toolchain's `share/licenses/` and vendor provenance retained |

The project owner confirmed on 2026-10-30 that Bouffalo Lab provides the wireless/
Controller redistribution and USB-identity permissions for this fixed combination.
This is the recorded owner-provided basis, not an invented independent legal certificate.
At the owner's direction, embedded third-party vendor-tool/source mapping review
is outside this acceptance round. Exclusion is not a claim that every embedded
component was individually audited or is unconditionally Apache-2.0.

Publisher source locations are `bouffalolab/bouffalo_sdk`, `bouffalolab/toolchain`,
and `bouffalolab/bflb_flash_tools` on GitHub (the last mirrors the SDK's
`tools/bflb_tools`; the builder takes the same files from the SDK release). The
builder names the SDK release tag, the toolchain and the patches in `sources.json`;
each package manifest records the SDK commit that tag named. The pinned toolchain
archive `gcc-riscv-14.3.0-linux-x86_64.tar.xz` has SHA-256
`93128c0da22cbbcf8473fae8c38b63232ebcd8d51969a14dc8491e77721b63a5`.

Do not distribute a user's Flash backup, saved credentials, private keys, pairing
keys, or raw personal test data. Such backups are not part of the release archive.
