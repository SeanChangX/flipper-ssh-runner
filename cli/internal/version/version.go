// Package version is the tool's own version, set at build time:
//
//	go build -ldflags "-X github.com/SeanChangX/flipper-ssh-runner/cli/internal/version.Version=0.2.0 \
//	  -X github.com/SeanChangX/flipper-ssh-runner/cli/internal/version.Commit=$(git rev-parse --short HEAD) \
//	  -X github.com/SeanChangX/flipper-ssh-runner/cli/internal/version.Date=$(git log -1 --format=%cs)" ./cmd/sshrunner
//
// It follows the project's release version (CHANGELOG.md).
//
// SPDX-License-Identifier: GPL-3.0-only
package version

import (
	"fmt"
	"runtime/debug"
)

var (
	Version = "" // e.g. 0.2.0; empty for a development build
	Commit  = ""
	Date    = ""
)

// String is "0.2.0 (a07aa8d, 2026-10-22)"; a development build takes the commit from the
// VCS information Go embeds.
func String() string {
	v, c, d := Version, Commit, Date
	if v == "" {
		v = "dev"
	}
	if c == "" || d == "" {
		if bi, ok := debug.ReadBuildInfo(); ok {
			for _, s := range bi.Settings {
				switch s.Key {
				case "vcs.revision":
					if c == "" && len(s.Value) >= 7 {
						c = s.Value[:7]
					}
				case "vcs.time":
					if d == "" && len(s.Value) >= 10 {
						d = s.Value[:10]
					}
				case "vcs.modified":
					if s.Value == "true" && Version == "" && c != "" {
						c += "-dirty"
					}
				}
			}
		}
	}
	if c == "" {
		return v
	}
	return fmt.Sprintf("%s (%s, %s)", v, c, d)
}
