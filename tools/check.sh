#!/usr/bin/env bash
# Local gate for Linux / WSL. Same steps as tools/check.ps1 (Windows) and
# .github/workflows/check.yml; keep all three in sync.
#
#   tools/check.sh                 # everything
#   tools/check.sh --skip firmware # quick iteration (not a merge gate)
#
# Footprint: everything generated stays inside the repo, in venv/ or .cache/.
# Python: ./venv if it is a Linux venv; otherwise .cache/venv-linux (on WSL the repo
# usually sits on /mnt/c next to a Windows venv/, which Linux cannot use).
# uFBT: .cache/ufbt/linux/<sdk> (Windows and Linux toolchains cannot share a directory).
# Docker runs with --rm, bind-mounts only this folder, no named volumes.
# Docker must already be running; this script never starts it.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SKIP=()
while [ $# -gt 0 ]; do
    case "$1" in
    --skip) SKIP+=("$2"); shift 2 ;;
    *) echo "usage: $0 [--skip proto-mirror|c-format|host-tests|tools-tests|fap|firmware|firmware-efusetest|firmware-rev0_1]..." >&2; exit 2 ;;
    esac
done

CACHE="$ROOT/.cache"
export PYTHONDONTWRITEBYTECODE=1
export PYTHONPYCACHEPREFIX="$CACHE/pycache"
export PIP_CACHE_DIR="$CACHE/pip"
if [ -x "$ROOT/venv/bin/python" ]; then
    VENV="$ROOT/venv"
else
    VENV="$CACHE/venv-linux"
fi
if [ ! -x "$VENV/bin/python" ]; then
    echo "-- creating venv at $VENV"
    python3 -m venv "$VENV" && "$VENV/bin/pip" install -q -r requirements.txt || exit 1
fi
PY="$VENV/bin/python"
UFBT="$VENV/bin/ufbt"
IDF_IMAGE=$(grep -E '^IDF_IMAGE=' firmware-esp32/IDF_VERSION | cut -d= -f2)
mkdir -p "$CACHE/build"

declare -A RESULT
ORDER=()

skipped() { local s; for s in "${SKIP[@]:-}"; do [ "$s" = "$1" ] && return 0; done; return 1; }

step() {
    local name=$1; shift
    ORDER+=("$name")
    if skipped "$name"; then RESULT[$name]="SKIPPED"; echo "== $name : skipped"; return; fi
    echo "== $name"
    local t0=$SECONDS
    if "$@"; then RESULT[$name]="PASS ($((SECONDS - t0)) s)"; else RESULT[$name]="FAIL"; fi
}

need_docker() {
    docker info >/dev/null 2>&1 || { echo "Docker is not reachable: on Linux start Docker Engine (systemctl) and be in the docker group; on Windows start Docker Desktop. Then re-run." >&2; return 1; }
}

# Native Linux Docker Engine writes bind-mounted files as root by default, which leaves
# root-owned trees under .cache/: run as the calling user with HOME inside .cache/.
# (Docker Desktop on Windows/WSL maps ownership itself.)
DOCKER_USER=()
if [ "$(uname -s)" = Linux ] && ! grep -qi microsoft /proc/version 2>/dev/null; then
    mkdir -p "$CACHE/home"
    DOCKER_USER=(--user "$(id -u):$(id -g)" -e HOME=/project/.cache/home)
fi

idf() {
    local workdir=$1; shift
    docker run --rm "${DOCKER_USER[@]}" -v "$ROOT:/project" -w "$workdir" \
        -e CCACHE_DIR=/project/.cache/ccache -e IDF_CCACHE_ENABLE=1 \
        -e IDF_COMPONENT_MANAGER=0 -e PYTHONDONTWRITEBYTECODE=1 \
        "$IDF_IMAGE" "$@"
}

s_proto_mirror() { "$PY" tools/sync_proto.py --check; }

# the C code is clang-formatted (.clang-format from the Flipper firmware; tools/format_c.sh)
s_c_format() {
    need_docker || return 1
    idf /project bash tools/format_c.sh --check
}

s_host_tests() {
    need_docker || return 1
    idf /project bash tools/host_tests.sh
}

# the PC config tool (cli/, Go): its module and build caches stay in .cache/go
s_cli() {
    if ! command -v go >/dev/null; then
        # no Go here: the pinned image of the version cli/go.mod names, caches in .cache/go
        need_docker || return 1
        local ver
        ver=$(sed -nE 's/^go ([0-9.]+)$/\1/p' cli/go.mod)
        docker run --rm "${DOCKER_USER[@]}" -v "$ROOT:/project" -w /project/cli \
            -e GOPATH=/project/.cache/go -e GOMODCACHE=/project/.cache/go/mod -e GOCACHE=/project/.cache/go/build \
            -e GOFLAGS=-modcacherw -e GOTOOLCHAIN=local -e CGO_ENABLED=0 "golang:$ver" \
            sh -c 'go mod tidy -diff && go vet ./... && go test ./... && go build -buildvcs=false -o /dev/null ./cmd/sshrunner'
        return
    fi
    (cd cli && GOPATH="$CACHE/go" GOMODCACHE="$CACHE/go/mod" GOCACHE="$CACHE/go/build" GOFLAGS=-modcacherw \
        GOTOOLCHAIN=local CGO_ENABLED=0 sh -c 'go mod tidy -diff && go vet ./... && go test ./... && go build -o /dev/null ./cmd/sshrunner')
}

s_tools_tests() {
    [ -d tools/tests ] || { echo "no tools tests yet"; return 0; }
    "$PY" -m pytest -q tools/tests
}

s_fap() {
    local ok=0 name url tag home have
    for name in UNLEASHED OFFICIAL; do
        url=$(grep -E "^${name}_SDK_URL=" flipper-app/SDK_VERSION | cut -d= -f2-)
        tag=$(grep -E "^${name}_TAG=" flipper-app/SDK_VERSION | cut -d= -f2-)
        home="$CACHE/ufbt/linux/${name,,}"
        have=$("$PY" -c 'import json,sys; print(json.load(open(sys.argv[1])).get("url",""))' "$home/current/ufbt_state.json" 2>/dev/null || true)
        if [ "$have" != "$url" ]; then
            echo "-- fetching $name SDK $tag"
            UFBT_HOME="$home" "$UFBT" update --hw-target=f7 --url="$url" || { ok=1; continue; }
        fi
        echo "-- building against $name $tag"
        # `faps` = build + API check, without writing .vscode/ into the app folder
        (cd flipper-app && UFBT_HOME="$home" "$UFBT" faps) || { ok=1; continue; }
        mkdir -p "flipper-app/dist/${name,,}"
        cp flipper-app/dist/ssh_runner.fap "flipper-app/dist/${name,,}/ssh_runner.fap"
    done
    return $ok
}

s_firmware() {
    need_docker || return 1
    idf /project/firmware-esp32 idf.py -B /project/.cache/build/esp32 \
        -D SDKCONFIG=/project/.cache/build/esp32/sdkconfig build
}

s_firmware_efusetest() {
    idf /project/firmware-esp32 idf.py -B /project/.cache/build/esp32-efusetest \
        -D SDKCONFIG=/project/.cache/build/esp32-efusetest/sdkconfig \
        -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.efusetest" build
}

# the other board: SSH Runner rev 0.1 (sdkconfig.rev0_1 picks it; the default is the prototype)
s_firmware_rev0_1() {
    idf /project/firmware-esp32 idf.py -B /project/.cache/build/esp32-rev0_1 \
        -D SDKCONFIG=/project/.cache/build/esp32-rev0_1/sdkconfig \
        -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.rev0_1" build
}

step proto-mirror s_proto_mirror
step c-format s_c_format
step host-tests s_host_tests
step tools-tests s_tools_tests
step cli s_cli
step fap s_fap
step firmware s_firmware
step firmware-efusetest s_firmware_efusetest
step firmware-rev0_1 s_firmware_rev0_1

echo
echo "== summary"
failed=0
for k in "${ORDER[@]}"; do
    printf "  %-17s %s\n" "$k" "${RESULT[$k]}"
    [ "${RESULT[$k]}" = "FAIL" ] && failed=1
done
if [ $failed -ne 0 ]; then echo "check.sh: FAILED"; exit 1; fi
if [ ${#SKIP[@]} -gt 0 ]; then echo "check.sh: passed with skipped steps (not a merge gate)"; exit 0; fi
echo "check.sh: ALL GREEN"
