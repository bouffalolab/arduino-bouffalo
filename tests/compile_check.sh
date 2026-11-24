#!/usr/bin/env bash
# Compile platform test sketches with arduino-cli while SDK paths are inaccessible; check maps and images.
# Usage: tests/compile_check.sh <libs-package> <toolchain> <flash-tools> [hidden-path...]
#
# 1. Copy the platform into a temporary sketchbook hardware/bouffalolab/bl; keep CLI config/data temporary too.
# 2. Select dependency/toolchain/flash packages with --build-property; index installation is tested separately.
# 3. In bubblewrap replace hidden SDK checkout/build paths with empty directories before compilation.
# 4. Check each sketch's map and post-processed image. Sketches including WiFi.h may use the Wi-Fi link group and
#    BLEController.h the BLE group; LittleFS, Preferences and BLUpdate sketches must carry the on-demand boot2
#    record, which loads the partition table, and other sketches must not.
# 5. Require exactly one call to __libc_init_array: Core main() or the package's SYSINIT constructor record.
set -euo pipefail

PKG=$(realpath -e "$1")
TC=$(realpath -e "$2")
FT=$(realpath -e "$3")
shift 3
HIDE=()
for h in "$@"; do
    HIDE+=("$(realpath -e "$h")")  # bwrap accepts nonexistent paths silently, so reject misspellings before isolation.
done

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FQBN=bouffalolab:bl:unor4wifi_bl616cl
TMP=$(mktemp -d)
mkdir -p "$TMP/sketchbook/hardware/bouffalolab" "$TMP/data" "$TMP/downloads"
cp -r "$ROOT" "$TMP/sketchbook/hardware/bouffalolab/bl"
rm -rf "$TMP/sketchbook/hardware/bouffalolab/bl/.git"
cat >"$TMP/arduino-cli.yaml" <<EOF
directories:
  data: $TMP/data
  downloads: $TMP/downloads
  user: $TMP/sketchbook
EOF

BWRAP=(bwrap --dev-bind / /)
for h in "${HIDE[@]}"; do
    BWRAP+=(--tmpfs "$h")
done

fail=0
for sketch in "$ROOT"/tests/sketches/*/ "$ROOT"/libraries/BLEController/examples/ControllerUSB/ \
    "$ROOT"/libraries/LittleFS/examples/Persistence/ "$ROOT"/libraries/Preferences/examples/Counter/ \
    "$ROOT"/libraries/BLUpdate/examples/SerialUpdate/; do
    name=$(basename "$sketch")
    out=$TMP/build-$name
    if "${BWRAP[@]}" arduino-cli compile --config-file "$TMP/arduino-cli.yaml" -b "$FQBN" \
        --build-property "runtime.tools.bl616cl-libs.path=$PKG" \
        --build-property "runtime.tools.gcc-riscv.path=$TC" \
        --build-property "runtime.tools.bl-flash-tools.path=$FT" \
        --build-path "$out" --warnings default "$sketch" >"$TMP/$name.log" 2>&1; then
        echo "PASS $name: 编译"
        grep -E '^(Sketch uses|Global variables|Flash )' "$TMP/$name.log" | sed 's/^/  /'
        allow=()
        if grep -q '^#include <WiFi.h>' "$sketch"/*.ino; then
            allow+=(--allow-group wifi)  # WiFi selects its link group through library.properties ldflags.
        fi
        if grep -q '^#include <BLEController.h>' "$sketch"/*.ino; then
            allow+=(--allow-group ble)
        fi
        if grep -qE '^#include <(LittleFS|Preferences|BLUpdate)\.h>' "$sketch"/*.ino; then
            allow+=(--expect-record boot2)
        fi
        python3 -I "$ROOT/tests/check_map.py" "$PKG/manifest.json" "$out/$name.ino.map" "${allow[@]}" >"$TMP/$name.map.txt" || fail=1
        sed 's/^/  /' "$TMP/$name.map.txt"
        python3 -I "$ROOT/tests/check_image.py" "$out" "$name.ino" >"$TMP/$name.image.txt" || fail=1
        sed 's/^/  /' "$TMP/$name.image.txt"
        calls=$("$TC/bin/riscv64-unknown-elf-objdump" -d "$out/$name.ino.elf" | grep -c '<__libc_init_array>$' || true)
        if [ "$calls" = 1 ]; then echo "  __libc_init_array 调用点 1 个: PASS"; else echo "  __libc_init_array 调用点 $calls 个: FAIL"; fail=1; fi
    else
        echo "FAIL $name: 编译，日志 $TMP/$name.log"
        tail -20 "$TMP/$name.log" | sed 's/^/  /'
        fail=1
    fi
done
echo "临时目录：$TMP"
exit $fail
