# RA4M1 OTA Update Marker

This minimal temporary Sketch exists only for the BL616CL-owned RA4M1 OTA
write-path gate. Its `RA4_OTA_UPDATE_MARKER:BOOT` output proves that the
BL616CL bridge, rather than host BOSSA, completed the committed OTA cache to
RA4M1 `SAM-BA X/Y/Z/K#` lifecycle.

The automated gate restores `ra4-wifis3-scan-smoke` after either success or
failure. Do not use this Sketch as a normal user application.
