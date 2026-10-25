// SPDX-License-Identifier: GPL-3.0-only
package tui

import (
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/x/ansi"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/store"
)

func click(t *testing.T, m Model, x, y int) Model {
	t.Helper()
	return send(t, m, tea.MouseMsg{X: x, Y: y, Button: tea.MouseButtonLeft, Action: tea.MouseActionPress})
}

func wheel(t *testing.T, m Model, down bool) Model {
	t.Helper()
	b := tea.MouseButtonWheelUp
	if down {
		b = tea.MouseButtonWheelDown
	}
	return send(t, m, tea.MouseMsg{Button: b, Action: tea.MouseActionPress})
}

// where: the screen position of text's first occurrence (column, line).
func where(t *testing.T, m Model, text string) (int, int) {
	t.Helper()
	for y, l := range strings.Split(ansi.Strip(m.View()), "\n") {
		if i := strings.Index(l, text); i >= 0 {
			return col(l, i), y
		}
	}
	t.Fatalf("%q not on screen:\n%s", text, ansi.Strip(m.View()))
	return 0, 0
}

func TestMouseTabsRowsAndHints(t *testing.T) {
	m, _ := setup(t)
	m = keys(t, m, "a", "pi", "tab", "pi", "tab", "pi.local", "ctrl+s") // a second host
	x, y := where(t, m, "Commands 0")
	m = click(t, m, x+1, y)
	if m.tab != tabCommands {
		t.Fatalf("tab %d after a click on Commands", m.tab)
	}
	x, y = where(t, m, "Hosts 2")
	m = click(t, m, x, y)
	if m.tab != tabHosts {
		t.Fatal("back to Hosts")
	}
	_, y = where(t, m, "pi@pi.local")
	m = click(t, m, 10, y)
	if m.row[tabHosts] != 1 {
		t.Fatalf("row %d after a click on the second host", m.row[tabHosts])
	}
	m = wheel(t, m, false)
	if m.row[tabHosts] != 0 {
		t.Fatal("the wheel does not scroll up")
	}
	m = wheel(t, m, true)
	if m.row[tabHosts] != 1 {
		t.Fatal("the wheel does not scroll down")
	}
	x, y = where(t, m, "p push") // a key hint in the footer
	m = click(t, m, x, y)
	if m.mode != mConfirm {
		t.Fatalf("clicking the push hint: mode %v", m.mode)
	}
	x, y = where(t, m, "n/enter no") // a dialog button
	m = click(t, m, x+2, y)
	if m.mode != mList || m.status != "Cancelled" {
		t.Fatalf("clicking no: %v %q", m.mode, m.status)
	}
	m = keys(t, m, "p")
	x, y = where(t, m, "y yes")
	m = click(t, m, x, y)
	if m.mode != mList || !strings.HasPrefix(m.status, "Pushed") {
		t.Fatalf("clicking yes: %v %q", m.mode, m.status)
	}
}

func TestMouseOption(t *testing.T) {
	if len(programOptions(Options{Mouse: true})) != 2 || len(programOptions(Options{})) != 1 {
		t.Fatal("--no-mouse does not leave the mouse off")
	}
}

func TestHelpOverlay(t *testing.T) {
	m, _ := setup(t)
	m = keys(t, m, "5", "?")
	if m.mode != mHelp {
		t.Fatal("? does not open the help")
	}
	v := ansi.Strip(m.View())
	for _, want := range []string{"Keys: Backups", "back up now", "export as .zip", "complete a path", "browse files", "ctrl+o", "push to the Flipper"} {
		if !strings.Contains(v, want) {
			t.Fatalf("no %q in the help:\n%s", want, v)
		}
	}
	m = keys(t, m, "?")
	if m.mode != mList {
		t.Fatal("? does not close the help")
	}
	m = keys(t, m, "1", "?")
	if v := ansi.Strip(m.View()); !strings.Contains(v, "Keys: Hosts") || !strings.Contains(v, "add a host") {
		t.Fatalf("hosts help:\n%s", v)
	}
	m = send(t, m, tea.KeyMsg{Type: tea.KeyEsc})
	if m.mode != mList {
		t.Fatal("esc does not close the help")
	}
}

func TestBusyShowsProgress(t *testing.T) {
	m, _ := setup(t)
	m.mode = mBusy
	m.prog.stepper("Pushing")(3, 9, "hosts/nas.conf")
	v := ansi.Strip(m.View())
	if !strings.Contains(v, "Pushing...") || !strings.Contains(v, "[3/9] hosts/nas.conf") {
		t.Fatalf("\n%s", v)
	}
	_ = store.AppDataDir
}
