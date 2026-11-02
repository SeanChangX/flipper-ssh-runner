#!/usr/bin/env bash
# The files the release archives carry next to the binary, made by GoReleaser's before hook
# (it cannot fill in the contents of archive files itself). Output: archive/ (git-ignored):
#   README.md                          a short README with the version filled in
#   LICENSE                            the repository's license
#   licenses/THIRD_PARTY_NOTICES.txt   the license (and NOTICE, if any) of the Go standard
#                                      library and of every module linked into sshrunner, in
#                                      one file: MIT and BSD ask for their notices to travel
#                                      with the binary
# Needs SSHRUNNER_VERSION (X.Y.Z), as .goreleaser.yaml does.
#
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
: "${SSHRUNNER_VERSION:?set SSHRUNNER_VERSION to the release version (X.Y.Z)}"
DOC="https://github.com/SeanChangX/flipper-ssh-runner/blob/v$SSHRUNNER_VERSION"
OUT=archive
rm -rf "$OUT" && mkdir -p "$OUT/licenses"
cp ../LICENSE "$OUT/LICENSE"

sed -e "s|@VERSION@|$SSHRUNNER_VERSION|g" -e "s|@DOC@|$DOC|g" > "$OUT/README.md" <<'EOF'
# sshrunner @VERSION@

`sshrunner` is the PC tool of SSH Runner: it edits the hosts, global commands, keys and
scripts of the SSH Runner Flipper Zero app from a computer, over USB (the connection qFlipper
uses) or on the SD card in a card reader, with a backup before every change.

## Install

- Archive (`.tar.gz`, `.zip`): unpack it and put `sshrunner` (`sshrunner.exe` on Windows) on
  your `PATH`. The binary is static: nothing else to install.
- Debian or Ubuntu: `sudo apt install ./sshrunner_@VERSION@_<arch>.deb`; Fedora:
  `sudo dnf install ./sshrunner_@VERSION@_<arch>.rpm`. Both install `/usr/bin/sshrunner`.
- On Linux, the Flipper's port needs the `dialout` group: `sudo usermod -aG dialout $USER`,
  then log in again.

## Usage

```sh
sshrunner          # the interactive editor
sshrunner --help   # every command, for scripts
```

Close qFlipper and leave the SSH Runner app while `sshrunner` is connected.

## More

- Full guide: @DOC@/cli/README.md
- Setting up the board and the Flipper app: @DOC@/docs/QUICKSTART.md
- License: GPL-3.0-only (`LICENSE`); the third-party licenses are in
  `licenses/THIRD_PARTY_NOTICES.txt`.
EOF

# The SPDX identifier of a license text (the kinds the modules use; anything else stops here).
spdx() {
    if grep -q "Apache License" "$1" && grep -q "Version 2.0" "$1"; then echo Apache-2.0
    elif grep -q "Permission is hereby granted, free of charge" "$1"; then echo MIT
    elif grep -qE "Permission to use, copy, modify, and(/or)? distribute" "$1"; then echo ISC
    elif grep -q "Redistribution and use in source and binary forms" "$1"; then
        if grep -qiE "Neither the name|names of its contributors" "$1"; then echo BSD-3-Clause; else echo BSD-2-Clause; fi
    elif grep -q "Mozilla Public License" "$1"; then echo MPL-2.0
    else echo unknown
    fi
}

RULE=$(printf '%.0s=' {1..79})
# One section: a heading line, the license's SPDX identifier, the license text, any NOTICE.
section() { # name version spdx license-file [notice-file]
    printf '\n%s\n%s %s\nLicense: %s\n%s\n\n' "$RULE" "$1" "$2" "$3" "$RULE"
    cat "$4"
    if [ -n "${5:-}" ]; then printf '\n--- NOTICE ---\n\n'; cat "$5"; fi
}

NOTICES="$OUT/licenses/THIRD_PARTY_NOTICES.txt"
GOROOT=$(go env GOROOT)
GOVERSION=$(go env GOVERSION)
{
    echo "sshrunner $SSHRUNNER_VERSION: third-party notices"
    echo
    echo "sshrunner is GPL-3.0-only (LICENSE, next to this folder). It is statically linked with"
    echo "the Go standard library and the modules below, each under its own license, all"
    echo "compatible with GPL-3.0. One section per component: its name and version, its license"
    echo "(SPDX identifier), the license text, and its NOTICE file if it has one."
    section "Go standard library" "$GOVERSION" "$(spdx "$GOROOT/LICENSE")" "$GOROOT/LICENSE"
} > "$NOTICES"

for os in linux darwin windows; do
    for arch in amd64 arm64; do
        GOOS=$os GOARCH=$arch go list -deps \
            -f '{{with .Module}}{{if not .Main}}{{.Path}} {{.Version}} {{.Dir}}{{end}}{{end}}' ./cmd/sshrunner
    done
done | sort -u | while read -r mod ver dir; do
    lic=$(ls "$dir" | grep -iE '^(licen[cs]e|copying)(\.md|\.txt)?$' | head -1 || true)
    notice=$(ls "$dir" | grep -iE '^notice(\.md|\.txt)?$' | head -1 || true)
    if [ -n "$lic" ]; then
        id=$(spdx "$dir/$lic")
        if [ "$id" = unknown ]; then
            echo "unrecognised license in $mod ($lic): check it and teach spdx() here" >&2
            exit 1
        fi
        section "$mod" "$ver" "$id" "$dir/$lic" ${notice:+"$dir/$notice"} >> "$NOTICES"
    elif [ "$mod" = github.com/mattn/go-localereader ]; then
        # ships no license file; its README's "License" section says MIT
        printf '\n%s\n%s %s\nLicense: MIT\n%s\n\n%s\n' "$RULE" "$mod" "$ver" "$RULE" \
            "The module ships no license file; its README's License section states MIT." >> "$NOTICES"
    else
        echo "no license file in $mod: check its license and list it here" >&2
        exit 1
    fi
done
mods=$(($(grep -cx "$RULE" "$NOTICES") / 2 - 1))
echo "archive files for $SSHRUNNER_VERSION: README.md, LICENSE, the notices of $mods modules"
