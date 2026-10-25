#!/usr/bin/env bash
# The licenses of every module linked into sshrunner, for the release archives (MIT and BSD
# ask for their notices to travel with the binary). Output: third_party/ (git-ignored).
#
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
rm -rf third_party && mkdir -p third_party
for os in linux darwin windows; do
    GOOS=$os go list -deps -f '{{with .Module}}{{if not .Main}}{{.Path}}@{{.Version}} {{.Dir}}{{end}}{{end}}' ./cmd/sshrunner
done | sort -u | while read -r mod dir; do
    out="third_party/$(echo "${mod%@*}" | tr '/' '_').txt"
    lic=$(ls "$dir" | grep -iE '^(licen[cs]e|copying)(\.md|\.txt)?$' | head -1 || true)
    if [ -n "$lic" ]; then
        cp "$dir/$lic" "$out"
    elif [ "${mod%@*}" = github.com/mattn/go-localereader ]; then
        # ships no license file; its README's "License" section says MIT
        printf '%s: MIT (stated in the module'"'"'s README; it ships no license file)\n' "$mod" > "$out"
    else
        echo "no license file in $mod: check its license and list it here" >&2
        exit 1
    fi
    echo "$mod" >> third_party/MODULES.txt
done
echo "$(wc -l < third_party/MODULES.txt) modules"
