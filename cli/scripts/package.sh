#!/usr/bin/env bash
# Local packaging: cli/scripts/package.sh; output in .cache/cli-dist, nothing is published.
# The version is the newest release in CHANGELOG.md, or SSHRUNNER_VERSION when set.
# GoReleaser runs through `go run` (a pinned version, fetched into the Go module cache only).
#
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(cd .. && pwd)
export GOPATH="$ROOT/.cache/go" GOMODCACHE="$ROOT/.cache/go/mod" GOCACHE="$ROOT/.cache/go/build"
export GOFLAGS=-modcacherw GOTOOLCHAIN=local
SSHRUNNER_VERSION="${SSHRUNNER_VERSION:-$(sed -nE 's/^## \[([0-9]+\.[0-9]+\.[0-9]+)\].*/\1/p' "$ROOT/CHANGELOG.md" | head -1)}"
SSHRUNNER_COMMIT="$(git rev-parse --short HEAD)"
SSHRUNNER_DATE="$(git log -1 --format=%cs)"
export SSHRUNNER_VERSION SSHRUNNER_COMMIT SSHRUNNER_DATE
echo "packaging sshrunner $SSHRUNNER_VERSION ($SSHRUNNER_COMMIT, $SSHRUNNER_DATE)"
GORELEASER_CURRENT_TAG="v$SSHRUNNER_VERSION" \
    go run github.com/goreleaser/goreleaser/v2@v2.18.3 release --snapshot --clean --skip=publish "$@"
