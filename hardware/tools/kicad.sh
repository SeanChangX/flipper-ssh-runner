#!/usr/bin/env bash
# Runs a command inside the pinned kicad/kicad container, in hardware/.
# Usage: hardware/tools/kicad.sh kicad-cli sch erc ...   or   hardware/tools/kicad.sh python3 tools/gen_sch.py
# Footprint: docker run --rm, only this repo is mounted, KiCad's config and
# every output live in .cache/ inside the repo; no named volumes.
set -euo pipefail
cd "$(dirname "$0")/../.."
# CI passes the same image pinned by digest (KICAD_IMAGE, .github/workflows/hardware.yml).
IMAGE=${KICAD_IMAGE:-kicad/kicad:10.0.7-full}
mkdir -p .cache/kicad/home .cache/hardware
USER_ARGS=()
if [ "$(uname -s)" = Linux ] && ! grep -qi microsoft /proc/version 2>/dev/null; then
    USER_ARGS=(--user "$(id -u):$(id -g)")
fi
# The stock global library tables, so ERC/DRC resolve KiCad's own symbol and footprint libraries.
SETUP='c=$HOME/.config/kicad/10.0; mkdir -p $c; for t in sym-lib-table fp-lib-table; do
    [ -f $c/$t ] || cp /usr/share/kicad/template/$t $c/$t; done; exec "$@"'
MSYS_NO_PATHCONV=1 docker run --rm "${USER_ARGS[@]}" \
    -v "$(pwd -W 2>/dev/null || pwd):/project" \
    -w /project/hardware \
    -e HOME=/project/.cache/kicad/home -e PYTHONDONTWRITEBYTECODE=1 \
    "$IMAGE" sh -c "$SETUP" sh "$@"
