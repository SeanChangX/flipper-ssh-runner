#!/usr/bin/env bash
# Runs idf.py inside the pinned espressif/idf container. Usage: tools/idf.sh build
# Footprint: docker run --rm, only this folder is mounted, the build tree
# and ccache live in .cache/ inside the repo; no named volumes.
set -euo pipefail
cd "$(dirname "$0")/.."
IMAGE=$(grep -E '^IDF_IMAGE=' firmware-esp32/IDF_VERSION | cut -d= -f2)
# The build folder in the container: the gate's incremental tree by default; release.sh
# passes its own, emptied first.
B=${IDF_BUILD_DIR:-/project/.cache/build/esp32}
# Native Linux: run as the calling user so .cache/ never gets root-owned files.
USER_ARGS=()
if [ "$(uname -s)" = Linux ] && ! grep -qi microsoft /proc/version 2>/dev/null; then
    mkdir -p .cache/home
    USER_ARGS=(--user "$(id -u):$(id -g)" -e HOME=/project/.cache/home)
fi
MSYS_NO_PATHCONV=1 docker run --rm "${USER_ARGS[@]}" \
    -v "$(pwd -W 2>/dev/null || pwd):/project" \
    -w /project/firmware-esp32 \
    -e CCACHE_DIR=/project/.cache/ccache -e IDF_CCACHE_ENABLE=1 \
    -e IDF_COMPONENT_MANAGER=0 -e PYTHONDONTWRITEBYTECODE=1 \
    "$IMAGE" idf.py -B "$B" -D SDKCONFIG="$B/sdkconfig" "$@"
