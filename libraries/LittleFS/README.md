# LittleFS

Platform storage for BL616CL using the board's program Flash. Requires the fixed
`bl616cl-libs` 0.6.0 or newer (current local candidate: 0.10.0) and `bl-flash-tools 0.2.0` packages. The library has no bridge/AT/RA4M1
references. Internal read/program/erase/sync callbacks call the vendor Flash
API directly. Mount reads the active partition table, finds the `lfs` entry, and
validates its physical bounds and separation from other partitions. No MTD
read/write wrapper or separate public block-device library is required. A
missing/invalid partition fails; addresses are never guessed.

`begin()` mounts without formatting. `format()` explicitly initializes the
reserved partition and refuses while mounted. `end()` refuses while files are
open. These failures use `LFS_ERR_BADF`. All operations return a result;
`lastError()` reports negative littlefs errors. `File::write()` returns bytes
written, `read(buffer,n)` returns bytes or -1, single-byte `read()` returns -1 at
EOF or error (distinguish with `lastError()`). `sync()` and `close()` report
commit errors; `flush()` is the Arduino void API, so inspect `lastError()`.

Paths start with `/`; parent traversal is rejected. littlefs limits each name
to 255 bytes. Modes are `r`, `r+`, `w`, `w+`, `a`, `a+`; `w` truncates and `a`
appends. `File` implements Arduino `Stream` and `Print`, size/position/seek and
shared-handle copy semantics: closing one copy closes every copy. The filesystem
object must outlive its files. Directory create/remove are provided; directory
iteration is not implemented. `usedBytes()` reports allocated blocks (including
metadata), not the sum of logical file lengths; free capacity is less than
`totalBytes()` because littlefs needs metadata and copy-on-write space.

Use all filesystem/file calls serially from one task, never an ISR. Multiple
task access requires caller serialization. There is no global SDK libc/VFS
redirect and no background filesystem task. Successful `sync()`/`close()`
commits data; interrupted uncommitted writes may be lost. Rename of a committed
temporary file provides littlefs's atomic replacement for future Preferences.
No mount failure invokes format. Formatting clears logical files, not secure
erasure of every previously used data byte.

Layout v1 reserves `[0x310000,0x378000)` (416 KiB, 104 x 4 KiB blocks,
256-byte read/program/cache) and limits firmware to 1.5 MiB starting at 0x10000.
FW1 occupies `[0x190000,0x310000)` and has the same 1.5 MiB size.
The `mfg` entry retains its name and aliases FW1: factory-test firmware before
shipping, OTA B slot afterward. Builder owns the partition TOML, linker bound
resources. boot2, media, PSM, KEY, DATA and factory addresses are retained. The normal Arduino
upload recipe writes boot2, partition table and firmware only, with section
erase; filesystem images are not included. Do not use whole-chip erase to
update firmware. Old platform/layout uploads, OTA layouts, shrinking storage
and layout changes are migrations: first back up all Flash and export files,
verify exact devices and bounds, then explicitly approve/perform migration.
There is no automatic on-device layout or disk-format migration.

The pinned SDK provides littlefs 2.11 (disk format 2.1), BSD-3-Clause; its license
is shipped in the dependency package as `LICENSE.littlefs`. Only the upstream
engine is linked. The SDK autoformat port and KV/VFS adapters are not used.


## Default and custom partitions

Requires Arduino CLI 1.5.1 (the validated version) with advanced size recipes
and Linux x86_64 tools. Default: `Tools > Partition Scheme > Default`. Custom:
select `Custom` and put `partitions.toml` in the sketch root. Copy the package's
`boot/partition_cfg_4M_lfs_v1.toml` as the starting point. The on-Flash name is
`lfs` because the vendor image tool accepts only 1-7 ASCII partition-name
characters. The Arduino library/API name remains `LittleFS`.

For CLI/CI, `--build-property build.bl_partition_file=/absolute/layout.toml`
selects an explicit file, regardless of the menu; relative paths start at the
sketch directory. A missing Custom/explicit file fails without falling back.
The current board contract preserves boot2, PT locations, media and factory/
parameter regions, keeps equal contiguous FW slots at 0x10000, and makes `mfg`
alias FW1 exactly. Adjust slot sizes and the filesystem area within those
bounds. No SDK rebuild or installation-directory edit is necessary.

The prebuild hook saves `layout.toml` and `layout.json` in the build directory;
the linker bound, image post-processing, advanced size report and upload use
this snapshot. The final image and partition SHA-256 values are checked before
upload. Normal upload runs a writable FlashCommand copy in the build directory,
so installed tools remain unchanged.

`arduino-cli compile --output-dir DIR` exports image-prefixed `.bin.layout.json`,
`.bin.layout.toml` and `.bin.partition.bin` sidecars; keep these with the image
for `arduino-cli upload --input-dir DIR`. Export compiled binary also creates a
`bl-flash/` bundle in the sketch directory with the image, `partition.bin`,
`layout.json` and `layout.toml`. Do not delete or replace a sidecar independently.
No standalone upload re-reads today's sketch partition file or substitutes a
default table. Changing a layout does not migrate files or enable OTA.
