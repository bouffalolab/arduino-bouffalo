#!/usr/bin/env bash
# Compile platform test sketches with arduino-cli while SDK paths are inaccessible; check maps and images.
# Usage: tests/compile_check.sh <libs-package> <toolchain> <flash-tools> [hidden-path...]
#
# 1. Copy the platform into a temporary sketchbook hardware/bouffalolab/bl; keep CLI config/data temporary too.
# 2. Select dependency/toolchain/flash packages with --build-property; index installation is tested separately.
# 3. In bubblewrap replace hidden SDK checkout/build paths with empty directories before compilation.
# 4. Check each sketch's map and post-processed image; sketches including WiFi.h may use the Wi-Fi link group.
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
for sketch in "$ROOT"/tests/sketches/*/; do
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
            allow=(--allow-group wifi)  # WiFi selects its link group through library.properties ldflags.
        fi
        python3 -I "$ROOT/tests/check_map.py" "$PKG/manifest.json" "$out/$name.ino.map" "${allow[@]}" >"$TMP/$name.map.txt" || fail=1
        sed 's/^/  /' "$TMP/$name.map.txt"
        python3 -I "$ROOT/tests/check_image.py" "$out" "$name.ino" >"$TMP/$name.image.txt" || fail=1
        sed 's/^/  /' "$TMP/$name.image.txt"
    else
        echo "FAIL $name: 编译，日志 $TMP/$name.log"
        tail -20 "$TMP/$name.log" | sed 's/^/  /'
        fail=1
    fi
done
echo "临时目录：$TMP"
exit $fail
