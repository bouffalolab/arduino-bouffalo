# Preferences

A standard Arduino platform library (`libraries/Preferences/src`) layered on
`LittleFS`. Include `<Preferences.h>`; Arduino discovers and compiles the source
and its declared `LittleFS` dependency. No bridge, RA4M1, AT, ESP32 NVS, SDK build
hook or extra linker archive is involved. Requires the matching LittleFS platform
and fixed `bl616cl-libs 0.6.0` / `bl-flash-tools 0.2.0` packages.

The public API is adapted from Espressif Arduino-ESP32 2.0.9 `Preferences.h`
(`https://github.com/espressif/arduino-esp32/tree/2.0.9/libraries/Preferences`)
(Apache-2.0, see LICENSE); the persistence implementation is independent.
Supported typed calls cover signed/unsigned 8/16/32/64-bit values, long aliases,
bool, float, double, String/C strings and bytes, plus begin/end, clear/remove,
isKey/getType/freeEntries. Floats/doubles use native IEEE binary blobs on the
validated BL616CL target. Integer values use explicitly little-endian encoding.

## Usage and errors

`begin(namespace, readOnly=false, partition=nullptr)` mounts without formatting.
Default, empty or `"lfs"` partition labels refer to the same platform partition;
other labels fail. A read-only begin on a missing namespace fails. Provision a
new LittleFS explicitly using its own API; no Preferences operation formats it.
See `examples/Counter` for checked use. Call all Preferences and LittleFS methods
serially in task context; there are no internal locks or ISR support.

Names and keys are 1–15 bytes. Each namespace has at most 64 keys; one value is at
most 1023 bytes. Typed reads require the stored type; missing or mismatched values
return the supplied default. `lastError()` distinguishes NOENT, INVAL, BADF,
NOSPC, IO, CORRUPT and NOMEM using negative littlefs codes. A closed/unopened or
read-only writer fails with BADF. Inspect errors rather than treating all defaults
as successful storage reads. A second begin on a started instance fails.

Puts return committed data bytes. Strings exclude their NUL terminator; an empty
string returns zero **with lastError()==0** and remains an existing PT_STR key.
Zero-byte blobs are rejected. C-string gets return bytes **including** the NUL;
a short output buffer fails without copying. Byte gets return the blob size;
a null buffer or zero maximum length queries its length. Values can change type
through a put. `clear()` affects only the opened namespace; `end()` performs no
write. This is a compatible API subset, not the ESP32 NVS implementation.

`freeEntries()` reports **64 minus the current namespace's key count**. It is not
NVS global free-entry statistics or available Flash bytes. Physical space may
run out sooner, and atomic replacement needs additional temporary space. Storage
is shared with ordinary files; there is no reserved quota or secure erasure.

## On-disk format and update policy

Preferences owns `/.preferences/<hex namespace bytes>.kv`; the sibling `.tmp` is
an uncommitted replacement. Platform LittleFS remains an administrative raw API.
Bridge file AT rejects this root and its descendants (including `/./` and repeated
slash aliases), preventing accidental manipulation through that frontend.

Version 1 starts with `BLP1`, u16 LE version 1, u16 LE record count. Each record has
key[16] including NUL, u8 type (PT_I8..PT_BLOB = 0..9), u8 reserved zero, u16 LE
length and exactly that many data bytes. Strings omit NUL. Unknown versions and
malformed structures fail without reset or automatic migration. There is no
additional value CRC; this is not a guarantee against all media bit corruption.

An update streams existing records into `.tmp` with a 256-byte copy buffer,
closes it successfully, then atomically renames it over `.kv`. It does not copy
a whole namespace onto the stack or heap. Failed or interrupted writes retain
the last committed snapshot; orphan `.tmp` is ignored and overwritten by the
next own update. Multiple instances used serially reread the current snapshot.
The String getter allocates only the requested value (at most 1024 bytes).

Normal Arduino uploads with the same layout preserve the filesystem. Layout
changes and future storage versions require explicit backup/export and migration;
there is no implicit conversion. Explicit LittleFS.format clears all namespaces
and files, while Preferences.clear clears only one namespace.

## Validation boundary

Experimental on BL616CL 1V0. Direct platform API, original RA4M1 Modem binary
round trips, real full-capacity failure and recovery, reset during writes,
user-confirmed complete power loss, unchanged full-Flash readback across power
loss, filesystem preservation across standard upload, and real mount failure without
filesystem modification have passed.
Acceptance evidence is maintained in the ARDUINO-48
task, with real hardware separated from POSIX/fake protocol checks. Build success
alone is not functionality acceptance. Other chips/boards and concurrent access
are not validated.
