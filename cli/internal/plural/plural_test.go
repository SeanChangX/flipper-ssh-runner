// SPDX-License-Identifier: GPL-3.0-only
package plural

import "testing"

func TestN(t *testing.T) {
	for n, want := range map[int]string{0: "0 private keys", 1: "1 private key", 2: "2 private keys"} {
		if got := N(n, "private key"); got != want {
			t.Errorf("%d: %q", n, got)
		}
	}
}
