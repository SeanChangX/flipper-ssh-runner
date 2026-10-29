#!/usr/bin/env bash
# Builds and runs every host unit-test suite. Runs inside the pinned espressif/idf
# container (the local gate, tools/check.ps1) or on any Linux box with gcc + cmake (CI).
# Build trees go to .cache/build/<suite> inside the repo (footprint rule).
set -euo pipefail
cd "$(dirname "$0")/.."
B=${BUILD_ROOT:-$PWD/.cache/build}
run() {
    local src=$1 name=$2
    cmake -S "$src" -B "$B/$name" -DCMAKE_BUILD_TYPE=Debug >/dev/null
    cmake --build "$B/$name" -j"$(nproc)" >/dev/null
    ctest --test-dir "$B/$name" --output-on-failure
}
run proto proto
if [ -f flipper-app/tests/CMakeLists.txt ]; then
    run flipper-app/tests flipper-app
fi
if [ -f firmware-esp32/test_host/CMakeLists.txt ]; then
    run firmware-esp32/test_host firmware-host
fi
