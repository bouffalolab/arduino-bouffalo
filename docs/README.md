# Bouffalo Arduino 0.1.0

Start with [installation](INSTALL.md), then read the [supported scope](SUPPORT.md).
Use [recovery](RECOVERY.md) after an interrupted upload. The [release notes](RELEASE-NOTES.md)
and [license notices](LICENSING.md) describe the exact reviewed combination.

This release targets Linux x86_64 and the UNO R4 WiFi board with a **BL616CL 1V0**
coprocessor. It is not firmware for an unmodified ESP32-based UNO R4 WiFi.

There are two different Arduino boards in this product:

| Program being built | Arduino board selection | Repository |
| --- | --- | --- |
| BL616CL Core sketch or USB/Wi-Fi/BLE bridge | Bouffalo Lab: UNO R4 WiFi (BL616CL coprocessor) | This platform; bridge application in `bouffalolab/arduino-bouffalo-uno-r4-wifi-usb-bridge` |
| RA4M1 user sketch, WiFiS3, ArduinoBLE or PhoneMatrix | Arduino UNO R4 Boards: Arduino UNO R4 WiFi | Arduino Renesas core 1.6.0; bridge repository `examples/PhoneMatrix` |

Selecting the BL616CL board does not upload a sketch to RA4M1. Selecting the RA4M1
board does not update the BL616CL bridge. A discovered USB port alone does not
determine which processor the selected board compiles for.

The platform is generic; it does not implement the RA4M1 AT protocol or CMSIS-DAP
application. Those belong to the separate bridge sketch. The independent
`bouffalolab/arduino-bouffalo-lib-builder` produces fixed dependency packages; an
ordinary Arduino build does not download or build the Bouffalo SDK.

Public download URLs become usable only after the owner-approved release assets
and package index have been published. A successful CI run is build evidence,
not public release approval or a new hardware acceptance result.
