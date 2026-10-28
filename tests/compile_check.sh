#!/usr/bin/env bash
# 平台编译检查：在 SDK 不可访问的环境中用 arduino-cli 编译测试 sketch，并检查 map 与镜像。
# 用法：tests/compile_check.sh <底层包目录> <工具链目录> <烧录工具包目录> [隐藏路径...]
#
# 1. 平台复制到临时 sketchbook 的 hardware/bouffalolab/bl，arduino-cli 配置与数据目录都在临时目录；
# 2. 底层包、工具链与烧录工具包经 --build-property 指定（本地包索引安装在后续步骤验证）；
# 3. 在 bubblewrap 中把隐藏路径（SDK 检出、SDK 构建目录等）换成空目录后编译；
# 4. 每个 sketch 用 tests/check_map.py 检查 map（包含 WiFi.h 的 sketch 允许 Wi-Fi 组），用 tests/check_image.py 检查后处理产物。
set -euo pipefail

PKG=$(realpath -e "$1")
TC=$(realpath -e "$2")
FT=$(realpath -e "$3")
shift 3
HIDE=()
for h in "$@"; do
    HIDE+=("$(realpath -e "$h")")  # 路径写错时 bwrap 不报错，隔离会静默失效
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
        --build-property "runtime.tools.xuantie-gcc-riscv.path=$TC" \
        --build-property "runtime.tools.bl-flash-tools.path=$FT" \
        --build-path "$out" --warnings default "$sketch" >"$TMP/$name.log" 2>&1; then
        echo "PASS $name: 编译"
        grep -E '^(Sketch uses|Global variables|Flash )' "$TMP/$name.log" | sed 's/^/  /'
        allow=()
        if grep -q '^#include <WiFi.h>' "$sketch"/*.ino; then
            allow=(--allow-group wifi)  # WiFi 库经 library.properties 的 ldflags 链入 Wi-Fi 组
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
