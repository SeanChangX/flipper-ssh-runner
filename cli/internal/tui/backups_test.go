// SPDX-License-Identifier: GPL-3.0-only
package tui

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/charmbracelet/x/ansi"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/conf"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/store"
)

func key(t *testing.T, m Model, k tea.KeyType) Model {
	t.Helper()
	return send(t, m, tea.KeyMsg{Type: k})
}

// backupsTab: the editor on its Backups tab with n backups made from the fake Flipper.
func backupsTab(t *testing.T, n int) Model {
	t.Helper()
	m, _ := setup(t)
	for i := 0; i < n; i++ {
		if _, _, err := conf.Backup(m.st, m.o.BackupDir, false); err != nil {
			t.Fatal(err)
		}
	}
	return keys(t, m, "5")
}

func TestBackupsTabListsNewestFirst(t *testing.T) {
	m := backupsTab(t, 3)
	if m.tab != tabBackups || len(m.backups) != 3 {
		t.Fatalf("tab %d, %d backups", m.tab, len(m.backups))
	}
	// the exact order (same-second names too) is tested in conf; here: newest at the top
	if m.backups[0].Created.Before(m.backups[2].Created) {
		t.Fatalf("order %v %v %v", m.backups[0].ID, m.backups[1].ID, m.backups[2].ID)
	}
	v := ansi.Strip(m.View())
	for _, want := range []string{"Backups 3", "CREATED", "Flipper on COM3", tildePath(m.o.BackupDir), "b back up now"} {
		if !strings.Contains(v, want) {
			t.Fatalf("missing %q:\n%s", want, v)
		}
	}
}

func TestBackupsTabEmpty(t *testing.T) {
	m := backupsTab(t, 0)
	if v := ansi.Strip(m.View()); !strings.Contains(v, "No backups yet") {
		t.Fatalf("\n%s", v)
	}
	m = keys(t, m, "d") // nothing to delete: no dialog
	if m.mode != mList {
		t.Fatal("delete on an empty list asked")
	}
}

func TestBackupNowAndPruning(t *testing.T) {
	m := backupsTab(t, 0)
	m.o.KeepBackups = 2
	for i := 0; i < 3; i++ {
		m = keys(t, m, "b")
		if m.mode != mBackupNow {
			t.Fatal("b does not open Back up now")
		}
		m = key(t, m, tea.KeyEnter)
	}
	if len(m.backups) != 2 {
		t.Fatalf("%d backups kept, want 2", len(m.backups))
	}
	if !strings.Contains(m.status, "1 old backup deleted") {
		t.Fatalf("status %q", m.status)
	}
	if !m.backups[0].Keys {
		t.Fatal("a backup leaves the private keys out by default")
	}
	m = keys(t, m, "b")
	if !m.bkKeys || !strings.Contains(ansi.Strip(m.View()), "readable by you only") {
		t.Fatal("keys on by default, with where they are kept")
	}
	m = keys(t, m, "k") // off
	if m.bkKeys || !strings.Contains(ansi.Strip(m.View()), "cannot bring the private keys back") {
		t.Fatal("keys off: says what is lost")
	}
	m = key(t, m, tea.KeyEnter)
	if m.backups[0].Keys {
		t.Fatal("keys included although turned off")
	}
}

func TestRestoreShowsThePlanFirst(t *testing.T) {
	m, f := setup(t)
	if _, _, err := conf.Backup(m.st, m.o.BackupDir, false); err != nil {
		t.Fatal(err)
	}
	delete(f.Files, store.AppDataDir+"/hosts/nas.conf") // the host is gone from the Flipper
	m = send(t, m, m.load()())
	m = keys(t, m, "5")
	m = key(t, m, tea.KeyEnter)
	if m.mode != mConfirm || !strings.Contains(ansi.Strip(m.View()), "+ hosts/nas.conf") {
		t.Fatalf("no preview:\n%s", ansi.Strip(m.View()))
	}
	if _, ok := f.Files[store.AppDataDir+"/hosts/nas.conf"]; ok {
		t.Fatal("restored before the answer")
	}
	m = key(t, m, tea.KeyEnter) // enter is the safe answer: no
	if m.mode != mList || m.status != "Cancelled" {
		t.Fatalf("enter did not cancel: %v %q", m.mode, m.status)
	}
	m = key(t, m, tea.KeyEnter)
	m = keys(t, m, "y")
	if _, ok := f.Files[store.AppDataDir+"/hosts/nas.conf"]; !ok {
		t.Fatalf("not restored: %q", m.status)
	}
	if len(m.backups) != 2 { // the state before the restore was backed up
		t.Fatalf("%d backups", len(m.backups))
	}
}

func TestExportImportDeleteInTheTab(t *testing.T) {
	m := backupsTab(t, 1)
	file := filepath.Join(t.TempDir(), "out.tar.gz")
	m = keys(t, m, "x")
	if m.mode != mPath || !strings.HasSuffix(m.path.Value(), ".zip") {
		t.Fatalf("export field %q", m.path.Value())
	}
	m.path.SetValue(file)
	m = key(t, m, tea.KeyEnter)
	if _, err := os.Stat(file); err != nil {
		t.Fatalf("not exported: %q", m.status)
	}
	id := m.backups[0].ID
	m = keys(t, m, "d")
	if m.mode != mConfirm {
		t.Fatal("delete did not ask")
	}
	m = keys(t, m, "y")
	if len(m.backups) != 0 {
		t.Fatal("not deleted")
	}
	m = keys(t, m, "i")
	m.path.SetValue(file)
	m = key(t, m, tea.KeyEnter)
	if len(m.backups) != 1 || m.backups[0].ID != id {
		t.Fatalf("import: %q", m.status)
	}
	m = keys(t, m, "i")
	m.path.SetValue(file)
	m = key(t, m, tea.KeyEnter)
	if m.mode != mPath || !m.bad || !strings.Contains(m.status, "already") {
		t.Fatalf("second import: %q", m.status)
	}
}

func TestPathFieldTabAndBrowse(t *testing.T) {
	m, _ := setup(t)
	d := tree(t)
	m.rules = pathRules{home: d}
	m = keys(t, m, "3", "i")
	m.path.SetValue("~/id")
	m = key(t, m, tea.KeyTab)
	if m.path.Value() != "~/id_" || len(m.matches) != 0 {
		t.Fatalf("first tab: %q %v", m.path.Value(), m.matches)
	}
	m = key(t, m, tea.KeyTab) // second Tab: the list
	if len(m.matches) != 2 || !strings.Contains(ansi.Strip(m.View()), "id_rsa") {
		t.Fatalf("second tab: %v", m.matches)
	}
	m = key(t, m, tea.KeyDown)
	m = key(t, m, tea.KeyEnter) // take id_rsa
	if m.path.Value() != "~/id_rsa" || m.mode != mPath {
		t.Fatalf("taken: %q", m.path.Value())
	}
	m.path.SetValue(d + "/nope")
	m = key(t, m, tea.KeyTab) // no match: says so
	if m.status != "No match" {
		t.Fatalf("%q", m.status)
	}
	m.path.SetValue(d + "/")
	m = key(t, m, tea.KeyCtrlO)
	if m.status != "" { // the old "No match" is not about the picker
		t.Fatalf("stale status %q", m.status)
	}
	if m.mode != mPick || m.pick.kind != pathKey {
		t.Fatal("ctrl+o does not browse")
	}
	m = keys(t, m, "w", "o", "r", "k")
	m = key(t, m, tea.KeyEnter)
	if m.mode != mPath || m.path.Value() != filepath.Join(d, "work.pem") {
		t.Fatalf("picked %q", m.path.Value())
	}
}

var update = flag.Bool("update", false, "rewrite the golden files in testdata/frames")

// TestFrames renders the editor's screens as plain text and compares each with its golden file
// in testdata/frames/<name>.golden; `go test ./internal/tui -run Frames -update` rewrites them
// after an intended change. The screens render as a user sees them: the defaults (keep 10,
// mouse on), a Flipper on COM3, a 110x30 terminal, and a home folder (shown as ~) holding the
// backups and ~/.ssh. The clock is fixed and shown in UTC, and paths are written with /, so one
// golden file serves every OS.
func TestFrames(t *testing.T) {
	local, clock := time.Local, conf.Now
	t.Cleanup(func() { time.Local, conf.Now = local, clock })
	time.Local = time.UTC
	stamp := time.Date(2026, 10, 24, 9, 0, 0, 0, time.UTC)
	conf.Now = func() time.Time { stamp = stamp.Add(time.Minute); return stamp }
	golden := filepath.Join("testdata", "frames")
	seen := map[string]bool{}
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("USERPROFILE", home)
	user := func(o *Options) {
		o.BackupDir = filepath.Join(home, ".local", "share", "sshrunner", "backups")
		o.KeepBackups = conf.DefaultKeepBackups
		o.Mouse = true
	}
	ssh := filepath.Join(home, ".ssh")
	os.MkdirAll(ssh, 0o700)
	for _, f := range []string{"id_ed25519", "id_ed25519.pub", "id_rsa", "id_rsa.pub", "work.pem", "known_hosts", "config"} {
		os.WriteFile(filepath.Join(ssh, f), nil, 0o600)
	}
	write := func(name string, m Model) {
		t.Helper()
		v := ansi.Strip(m.View())
		checkBoxes(t, name, v)
		if filepath.Separator != '/' {
			v = strings.ReplaceAll(v, string(filepath.Separator), "/")
		}
		lines := strings.Split(v, "\n")
		for i := range lines {
			lines[i] = strings.TrimRight(lines[i], " ")
		}
		got := strings.Join(lines, "\n") + "\n"
		file := filepath.Join(golden, name+".golden")
		seen[name+".golden"] = true
		if *update {
			if err := os.MkdirAll(golden, 0o755); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(file, []byte(got), 0o644); err != nil {
				t.Fatal(err)
			}
			return
		}
		want, err := os.ReadFile(file)
		if err != nil {
			t.Errorf("frame %s: %v (go test ./internal/tui -run Frames -update writes it)", name, err)
			return
		}
		if d := lineDiff(string(want), got); d != "" {
			t.Errorf("frame %s differs from %s (- golden, + now; -update rewrites it):\n%s", name, file, d)
		}
	}
	defer func() {
		all, _ := filepath.Glob(filepath.Join(golden, "*.golden"))
		for _, f := range all {
			if !seen[filepath.Base(f)] {
				t.Errorf("%s: no frame renders it any more", f)
			}
		}
	}()
	e, _ := setup(t, user)
	write("backups-empty", keys(t, e, "5"))

	m, f := setup(t, user)
	conf.Backup(m.st, m.o.BackupDir, false)
	conf.Backup(m.st, m.o.BackupDir, true)
	delete(f.Files, store.AppDataDir+"/hosts/nas.conf")
	m = send(t, m, m.load()())
	m = keys(t, m, "5")
	write("backups-list", m)
	write("backups-restore-preview", key(t, m, tea.KeyEnter))
	write("backups-delete", keys(t, m, "d"))
	write("backups-now", keys(t, m, "b"))
	write("backups-now-no-keys", keys(t, m, "b", "k"))
	x := keys(t, m, "x")
	x.path.SetValue("./sshrunner-backup.zip")
	write("backups-export", x)
	i := keys(t, m, "i")
	i.path.SetValue("~/Downloads/sshrunner-20261024-090000.zip")
	write("backups-import", i)

	k, _ := setup(t, user)
	k = keys(t, k, "3", "i")
	k.path.SetValue("~/.ssh/id")
	k = key(t, k, tea.KeyTab)
	k = key(t, k, tea.KeyTab)
	write("path-tab-list", k)
	k.matches = nil
	k.path.SetValue("~/.ssh/")
	write("path-browse", key(t, k, tea.KeyCtrlO))

	h, _ := setup(t, user)
	write("help-backups", keys(t, h, "5", "?"))
	write("help-hosts", keys(t, h, "?"))
	h.mode = mBusy
	h.prog.stepper("Pushing")(3, 9, "hosts/nas.conf")
	write("progress", h)
	c, _ := setup(t, user)
	write("command-counter", keys(t, c, "2", "a", strings.Repeat("x", 120)))
}

// lineDiff lists the lines that differ between two frames, by line number; "" when equal.
func lineDiff(want, got string) string {
	if want == got {
		return ""
	}
	w, g := strings.Split(want, "\n"), strings.Split(got, "\n")
	var b strings.Builder
	for i := 0; i < len(w) || i < len(g); i++ {
		var a, c string
		if i < len(w) {
			a = w[i]
		}
		if i < len(g) {
			c = g[i]
		}
		if a != c {
			fmt.Fprintf(&b, "line %d:\n- %s\n+ %s\n", i+1, a, c)
		}
	}
	return b.String()
}

// checkBoxes: every line of a dialog box ends at the same column as its top corner.
func checkBoxes(t *testing.T, name, v string) {
	t.Helper()
	right := -1
	for n, l := range strings.Split(v, "\n") {
		switch {
		case strings.Contains(l, "╮"):
			right = lipgloss.Width(l[:strings.LastIndex(l, "╮")])
		case right >= 0 && strings.Contains(l, "╯"):
			if got := lipgloss.Width(l[:strings.LastIndex(l, "╯")]); got != right {
				t.Errorf("%s line %d: box bottom at %d, top at %d", name, n+1, got, right)
			}
			right = -1
		case right >= 0:
			i := strings.LastIndex(l, "│")
			if i < 0 || lipgloss.Width(l[:i]) != right {
				t.Errorf("%s line %d: box border not at column %d:\n%s", name, n+1, right, l)
			}
		}
	}
}

func TestFrameBoxesLineUp(t *testing.T) {
	m, _ := setup(t)
	for _, v := range []Model{keys(t, m, "p"), keys(t, m, "5", "b"), keys(t, m, "?"), keys(t, m, "a")} {
		checkBoxes(t, "live", ansi.Strip(v.View()))
	}
}
