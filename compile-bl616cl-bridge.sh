#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLI="${ARDUINO_CLI:-arduino-cli}"
WORK="${ROOT}/build/migration-workspace"
BUILD="${BL616CL_BUILD_ROOT:-${ROOT}/build/bridge-image}"
mkdir -p "${WORK}/hardware/bouffalo"
ln -sfn "${ROOT}" "${WORK}/hardware/bouffalo/bl616cl"
printf 'directories:\n  user: %s\n' "${WORK}" > "${WORK}/arduino-cli.yaml"
"${CLI}" compile --config-file "${WORK}/arduino-cli.yaml" --fqbn bouffalo:bl616cl:unor4_bl616cl_provisional --show-properties "${ROOT}/UNOR4BL616CLBridge" > "${WORK}/effective-properties.txt"
python3 - "${WORK}/effective-properties.txt" <<'PYTHON'
import sys
from pathlib import Path
props=dict(line.split('=',1) for line in Path(sys.argv[1]).read_text().splitlines() if '=' in line)
assert 'bridge_profile.h' in props['build.bridge_cpp_profile'], 'validated bridge configuration was overridden'
assert 'build/bridge-runtime/' in props['compiler.sdk.path'], 'runtime is not the independent target build'
print('BRIDGE_EFFECTIVE_BUILD_PROFILE_PASS')
PYTHON
"${CLI}" compile --config-file "${WORK}/arduino-cli.yaml" --fqbn bouffalo:bl616cl:unor4_bl616cl_provisional --build-path "${BUILD}" --build-property "compiler.cpp.extra_flags=${BL616CL_CPP_EXTRA_FLAGS:-}" "${ROOT}/UNOR4BL616CLBridge"
python3 "${ROOT}/tools/bridge_runtime/verify_runtime_bundle.py" --bundle "${ROOT}/build/bridge-runtime/tools/sdk/bl616cl/bridge" --sdk "${ROOT}/third_party/bouffalo_sdk"
python3 "${ROOT}/tools/check_bl616cl_memory_budget.py" "${BUILD}/UNOR4BL616CLBridge.ino.map"
python3 "${ROOT}/tools/check_bl616cl_partition_layout.py" --application "${BUILD}/UNOR4BL616CLBridge.ino.bin"
sha256sum "${BUILD}/UNOR4BL616CLBridge.ino.bin"
