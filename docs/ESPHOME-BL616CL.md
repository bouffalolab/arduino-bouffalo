# ESPHome BL616CL integration

Use this Arduino checkout together with the ESPHome `feature/bouffalo-bl616cl`
branch. The Arduino branch is `feature/esphome-bl616cl`. The integration uses
the existing Arduino CLI runtime and a dedicated
`tools/partitions/partition_cfg_esphome_4M.toml` profile.

The implementation and validation guide is in the companion ESPHome repository:
[`esphome/components/bouffalo/README.md`](../../esphome/esphome/components/bouffalo/README.md).

The core now supplies real efuse MAC, TRNG and reset operations. Wi-Fi has
asynchronous startup/scan, separate IP readiness, current STA channel and
hostname/DNS handling. Arduino setup/loop declarations use C++ linkage.
The SDK submodule and default partition profile are unchanged.

Compile checks passed for ESPHome GPIO/logger, encrypted API/mDNS/manual IPv4,
and the complete OTA/I2C/UART sensor node. Arduino Blink also passed regression
compilation. No board was available for runtime tests.

First ESPHome installation writes a combined factory image as raw bytes at
address 0; the image includes boot2, both partition tables and application slot 0.
Network OTA uses a distinct ESPHome BL616CL envelope and rejects the default
partition layout. Do not send a factory image to network OTA.

The new profile removes the overlapping mfg entry and gives both firmware slots
0x1B4000 bytes. KEY/DATA/factory keep their offsets; media is reduced to 4 KiB
and PSM moves to 0x379000 with 64 KiB, because EasyFlash stores every preference
key as a LittleFS file and the original 32 KiB PSM ran out of metadata space on
the first boot. There is no application health rollback or signed-image
verification in this initial port.

On 2026-10-18 the minimal ESPHome fixture and then the full Wi-Fi/API/OTA node
were verified on a BL616CL DK. The node joined a hidden WPA3-SAE AP with PMF
enabled (`params.pmf_cfg = 1`), obtained an IP by DHCP, served Native API with
Noise encryption, was controlled from Home Assistant through the ESPHome
integration by IP, and accepted a network OTA (encrypted v2 protocol, commit
success). mDNS is not usable: the wl80211 lwIP netif has no IGMP flag, so
joining the mDNS multicast group stalls the main loop. Sensors and Preferences
restore/reset are still unverified on hardware.
