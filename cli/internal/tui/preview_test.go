// SPDX-License-Identifier: GPL-3.0-only
package tui

import (
	"fmt"
	"os"
	"testing"

	"github.com/charmbracelet/lipgloss"
	"github.com/charmbracelet/x/ansi"
	"github.com/muesli/termenv"
)

// SSHRUNNER_PREVIEW=1 go test ./internal/tui -run Preview -v: the screens as plain text, to
// review the layout without a terminal.
func TestPreview(t *testing.T) {
	if os.Getenv("SSHRUNNER_PREVIEW") == "" {
		t.Skip("set SSHRUNNER_PREVIEW=1")
	}
	m, _ := setup(t)
	m = keys(t, m, "a", "pi", "tab", "pi", "tab", "raspberrypi.local", "ctrl+s")
	// SSHRUNNER_PREVIEW=ansi: colours kept (dark terminal), for a screenshot tool
	color := os.Getenv("SSHRUNNER_PREVIEW") == "ansi"
	if color {
		lipgloss.SetColorProfile(termenv.TrueColor)
		lipgloss.SetHasDarkBackground(true)
	}
	show := func(name string, m Model) {
		v := m.View()
		if color {
			if os.Getenv("SSHRUNNER_PREVIEW_ONLY") == name {
				fmt.Print(v)
			}
			return
		}
		fmt.Printf("===== %s\n%s\n", name, ansi.Strip(v))
	}
	show("hosts", m)
	show("host form", keys(t, m, "e"))
	show("push", keys(t, m, "p"))
	show("keys", keys(t, m, "3"))
}
