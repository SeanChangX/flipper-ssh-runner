// SPDX-License-Identifier: GPL-3.0-only

// Package plural writes a count with its noun in the right number: "1 key", "2 keys".
package plural

import "strconv"

// N is n and the word, in the plural unless n is 1. The words this tool counts all take -s.
func N(n int, one string) string {
	if n == 1 {
		return "1 " + one
	}
	return strconv.Itoa(n) + " " + one + "s"
}
