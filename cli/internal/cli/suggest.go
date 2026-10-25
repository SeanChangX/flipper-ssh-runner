// SPDX-License-Identifier: GPL-3.0-only
package cli

import (
	"fmt"
	"strings"
)

// The commands, for "did you mean" (as git and cobra-based tools answer a typo).
var (
	commands           = []string{"status", "hosts", "pull", "push", "import-key", "backup", "restore", "backups", "completion", "version"}
	backupsSubcommands = []string{"list", "export", "import", "delete"}
)

func unknownCommand(cmd string) error {
	if s := suggest(cmd, commands); s != "" {
		return fmt.Errorf("unknown command %q; did you mean %q? (see sshrunner --help)", cmd, s)
	}
	return fmt.Errorf("unknown command %q (see sshrunner --help)", cmd)
}

// suggest: the closest of `options` to `word`, if it is close enough to be a typo (at most 2
// edits, and fewer than half the word), or the word with an "s" added or removed.
func suggest(word string, options []string) string {
	w := strings.ToLower(word)
	best, bestD := "", 3
	for _, o := range options {
		if w+"s" == o || w == o+"s" {
			return o
		}
		if d := distance(w, o); d < bestD && d*2 < len(w)+1 {
			best, bestD = o, d
		}
	}
	return best
}

// distance: the optimal-string-alignment edit distance (insert, delete, substitute, or swap
// two neighbours; each costs 1), so "stauts" is one edit from "status".
func distance(a, b string) int {
	d := make([][]int, len(a)+1)
	for i := range d {
		d[i] = make([]int, len(b)+1)
		d[i][0] = i
	}
	for j := range d[0] {
		d[0][j] = j
	}
	for i := 1; i <= len(a); i++ {
		for j := 1; j <= len(b); j++ {
			cost := 1
			if a[i-1] == b[j-1] {
				cost = 0
			}
			d[i][j] = min(d[i-1][j]+1, d[i][j-1]+1, d[i-1][j-1]+cost)
			if i > 1 && j > 1 && a[i-1] == b[j-2] && a[i-2] == b[j-1] {
				d[i][j] = min(d[i][j], d[i-2][j-2]+1)
			}
		}
	}
	return d[len(a)][len(b)]
}
