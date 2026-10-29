#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD="$ROOT/build/arduinoble-host-scan-response"

cmake -S "$ROOT/libraries/ArduinoBLE/extras/test" -B "$BUILD" \
  "-DCMAKE_CXX_FLAGS=-I$ROOT/libraries/ArduinoBLE/extras/test/include -I$ROOT/libraries/ArduinoBLE/extras/test/include/util -include $ROOT/tests/fakes/arduinoble_host_compat.h -DCATCH_CONFIG_NO_POSIX_SIGNALS -fsanitize=address -g -fno-omit-frame-pointer"
cmake --build "$BUILD" -j 4
for target in TEST_TARGET_UUID TEST_TARGET_DISC_DEVICE TEST_TARGET_ADVERTISING_DATA; do
  "$BUILD/bin/$target"
done

"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -g \
  -I "$ROOT/libraries/ArduinoBLE/src/utility" \
  "$ROOT/tests/arduinoble_acl_credit_test.cpp" -o "$BUILD/acl-credit-test"
"$BUILD/acl-credit-test"
echo 'ARDUINOBLE_ACL_CREDIT_LIFECYCLE_PASS'
