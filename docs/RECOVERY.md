# Upload and recovery

Identify the processor and physical USB device before opening a port. Linux tty
numbers can change after reset/reconnect. Use `/dev/serial/by-id` for a unique serial
number or `/dev/serial/by-path` for a fixed USB location. Resolve the link to a tty
if a vendor tool does not accept symbolic links. Do not stop another user's monitor.

Normal BL firmware enumerates as `349b:0011`; BL ROM download mode is `ffff:ffff`.
A USB CDC baud-rate selection is a control setting, not a physical USB bit rate.
Reopening a monitor at normal speed should not restart the running sketch.

## Upload a BL Core sketch

1. Select **UNO R4 WiFi (BL616CL coprocessor)** and its running USB serial port.
2. Compile and upload. The normal Core's 1200-baud touch enters ROM, and Arduino
   waits for the new download port before flashing.
3. If firmware is hung, lacks the touch handler, or ROM is already active, hold
   bootstrap GPIO36 high during reset using the board's download controls. Select
   the ROM port and retry the same standard upload.
4. After a successful upload, reopen Serial Monitor on the re-enumerated running
   port. Select the device again if the tty number changed.

## The bridge is different

The bridge uses 1200-baud touch to control **RA4M1 upload**, so it disables the Core's
automatic BL616CL touch reset. Updating the BL bridge therefore needs explicit BL
ROM entry through the board's download controls or the validated FTDI control path.
Changing an Arduino board selection alone does not put BL616CL into ROM.

For RA4M1 sketches, select **Arduino UNO R4 WiFi** from the Renesas board package
and upload through the running bridge. Keep BL and RA firmware roles distinct.

## Preserve stored data

The default 4 MiB layout has adjacent equal 1.5 MiB FW0/FW1 slots, followed by
LittleFS at `0x310000`, size `0x68000` (416 KiB). The existing media start is retained.
MFG names the factory-test firmware overlapping the future FW1/OTA-B area.

Standard same-layout uploads write boot2 at `0x0`, the partition table at `0xE000`,
and FW0 at `0x10000`. They preserve LittleFS, FW1 and media. Do not issue a full-chip
erase, call format, or change partitions to recover a failed firmware write.
An interrupted upload leaves FW0 incomplete; re-enter ROM and re-upload the exact
known-good default-layout image. Recovery was tested after real physical power loss.

Use the release inventory and SHA-256 values before flashing an exported image.
An ELF, map, raw image, partition table and layout are a matching set. Do not substitute
another chip's boot2, an ESP32 bridge image, or an arbitrary newer library package.
If layout or persistent format changes, back up data and plan migration explicitly;
an index rollback alone neither downgrades a board nor guarantees data compatibility.
