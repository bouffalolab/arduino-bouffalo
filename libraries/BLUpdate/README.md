# BLUpdate (experimental)

`BLUpdate` stages a BL616CL application in the inactive firmware partition. It
is independent of the UNO R4 bridge and of the transport: a sketch can supply
bytes received over Wi-Fi, serial, or another stream. It requires the matching
`bl616cl-libs` 0.12.0 and `bl-flash-tools` 0.3.0 packages. These package
versions are development build identifiers; they have not been published here.

The standard Arduino build emits `<sketch>.ino.bin.blfota` next to the compiled
image. This file consists of a 512-byte vendor OTA header with a BL616CL target
label, followed by the raw `.bin` image. Send it as binary; the `.bin`, `.ota`,
and `.bin.hash` files are not interchangeable inputs to this API.

1. Read exactly 512 header bytes and call `BLUpdate.begin(header, 512)`.
2. Call `BLUpdate.write(offset, data, length)` in order for the following image
   bytes. The first call must contain at least 256 bytes. `size()` is the raw
   image length and `written()` is the accepted byte count.
3. Call `finish()` to verify the staged image's SHA-256 by reading flash back.
   It does not select the new image. Call `abort()` to abandon a session.
4. Call `commit()` only after successful verification. It selects the new slot
   for the next boot; the application controls when to restart. A new session
   cannot begin after commit until reboot.

All operations return `bool`; `state()` and `error()` provide diagnostics.
The caller must serialize calls, implement transport timeouts, and abort on
failure or disconnection. `examples/SerialUpdate` demonstrates a minimal
prompted binary stream. It waits for `U`, then requests `HEADER 512` and each
`DATA n` block. After `COMMITTED`, reset the board. Open the serial port once:
on the Ai-M64P-32S-Kit, opening CH340 changes DTR/RTS and resets the target.

The default 4 MiB layout has two 1.5 MiB firmware slots and a separate 416 KiB
LittleFS region. The manufacturing `mfg` entry aliases the second slot, so an
update overwrites any manufacturing image stored there. Both running and new
firmware must use a compatible layout. Normal serial upload writes the firmware
to FW0 and resets both partition-table copies; it preserves LittleFS and the
protected media, parameter, key, data, and factory regions. Keep a backup before
changing an existing device's layout.

SHA-256 detects corruption; the chip label and checksum do **not** authenticate
firmware. This library provides no signed-image validation, anti-rollback,
automatic health rollback, or network authentication. Do not expose the example
protocol to untrusted networks. The Ai-M64P-32S-Kit has passed A/B sketch and
Wi-Fi transport tests. A physical power interruption after the first block
returned to the previous firmware with test data intact. Other interruption
points and the UNO RA4 bridge remain separate acceptance items.
