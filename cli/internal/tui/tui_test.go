// SPDX-License-Identifier: GPL-3.0-only
package tui

import (
	"strings"
	"testing"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/appdata"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/conf"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/flipper"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/store"
)

// a fake Flipper with one host, driven like a user: keys in, the model and the screen out
func setup(t *testing.T, opts ...func(*Options)) (Model, *flipper.Fake) {
	t.Helper()
	f := flipper.NewFake()
	f.Dirs["/ext/apps_data"], f.Dirs[store.AppDataDir], f.Dirs[store.AppDataDir+"/hosts"] = true, true, true
	h := appdata.NewHost()
	h.Alias, h.User, h.Host = "nas", "admin", "192.168.1.10"
	text, _ := h.Text()
	f.Files[store.AppDataDir+"/hosts/nas.conf"] = []byte(text)
	c, err := flipper.New(f)
	if err != nil {
		t.Fatal(err)
	}
	o := Options{Store: store.NewFlipper(c, "COM3"), BackupDir: t.TempDir()}
	for _, f := range opts {
		f(&o)
	}
	m := New(o)
	if _, ok := m.Init()().(tea.BatchMsg); !ok { // the window title, and the load
		t.Fatal("Init does not batch the title with the load")
	}
	m = send(t, m, m.load()())
	m = send(t, m, tea.WindowSizeMsg{Width: 110, Height: 30})
	return m, f
}

func send(t *testing.T, m Model, msg tea.Msg) Model {
	t.Helper()
	nm, cmd := m.Update(msg)
	m = nm.(Model)
	// run follow-up commands that finish at once (load, remove, push), not cursor blinks or
	// spinner ticks; a batch is opened up
	queue := []tea.Cmd{cmd}
	for i := 0; len(queue) > 0 && i < 20; i++ {
		c := queue[0]
		queue = queue[1:]
		if c == nil {
			continue
		}
		switch out := c().(type) {
		case tea.BatchMsg:
			queue = append(queue, out...)
		case loadedMsg, doneMsg, removeMsg:
			nm, next := m.Update(out)
			m = nm.(Model)
			queue = append(queue, next)
		}
	}
	return m
}

func keys(t *testing.T, m Model, ks ...string) Model {
	t.Helper()
	for _, k := range ks {
		var msg tea.KeyMsg
		switch k {
		case "enter":
			msg = tea.KeyMsg{Type: tea.KeyEnter}
		case "tab":
			msg = tea.KeyMsg{Type: tea.KeyTab}
		case "esc":
			msg = tea.KeyMsg{Type: tea.KeyEsc}
		case "ctrl+s":
			msg = tea.KeyMsg{Type: tea.KeyCtrlS}
		case "right":
			msg = tea.KeyMsg{Type: tea.KeyRight}
		default:
			msg = tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(k)}
		}
		m = send(t, m, msg)
	}
	return m
}

func TestAddHostAndPush(t *testing.T) {
	m, f := setup(t)
	if !strings.Contains(m.View(), "nas") || !strings.Contains(m.View(), "admin@192.168.1.10:22") {
		t.Fatalf("host not listed:\n%s", m.View())
	}
	// a: the form; alias, user, host; tab past port; ctrl+s
	m = keys(t, m, "a", "pi", "tab", "pi", "tab", "pi.local", "ctrl+s")
	if m.mode != mList || len(m.cfg.Hosts) != 2 {
		t.Fatalf("mode %v, %d hosts, err %q", m.mode, len(m.cfg.Hosts), m.form.err)
	}
	if !strings.Contains(m.View(), "1 unpushed") {
		t.Fatalf("no unpushed badge:\n%s", m.View())
	}
	m = keys(t, m, "p")
	if m.mode != mConfirm || !strings.Contains(m.View(), "+ hosts/pi.conf") {
		t.Fatalf("push dialog:\n%s", m.View())
	}
	m = keys(t, m, "y")
	if _, ok := f.Files[store.AppDataDir+"/hosts/pi.conf"]; !ok {
		t.Fatalf("not pushed; status %q", m.status)
	}
	if strings.Contains(m.View(), "unpushed") {
		t.Fatalf("still unpushed after the push:\n%s", m.View())
	}
}

func TestFormRefusesInvalidHost(t *testing.T) {
	m, _ := setup(t)
	m = keys(t, m, "a", "-bad", "ctrl+s")
	if m.mode != mForm || !strings.Contains(m.form.err, "alias") {
		t.Fatalf("mode %v err %q", m.mode, m.form.err)
	}
	m = keys(t, m, "esc")
	if m.mode != mList || len(m.cfg.Hosts) != 1 {
		t.Fatal("cancel did not leave the hosts as they were")
	}
}

func TestDeleteAsksAndQuitWarns(t *testing.T) {
	m, _ := setup(t)
	m = keys(t, m, "d")
	if m.mode != mConfirm {
		t.Fatal("delete did not ask")
	}
	m = keys(t, m, "y")
	if len(m.cfg.Hosts) != 0 {
		t.Fatal("not deleted")
	}
	m = keys(t, m, "q") // an unpushed delete: quitting asks first
	if m.mode != mConfirm || !strings.Contains(m.ask, "unpushed") {
		t.Fatalf("quit did not warn: %v %q", m.mode, m.ask)
	}
}

func TestCommandsTab(t *testing.T) {
	m, _ := setup(t)
	m = keys(t, m, "2", "a", "uptime", "enter", "a", "df -h", "enter", "K")
	if len(m.cfg.Commands) != 2 || m.cfg.Commands[0] != "df -h" {
		t.Fatalf("commands %v", m.cfg.Commands)
	}
	plan, err := m.plan()
	if err != nil || len(plan) != 1 || plan[0].Path != conf.CommandsFile {
		t.Fatalf("plan %v %v", plan, err)
	}
}

// the dialog keeps its width from the placeholder to the first key and on
func TestFormWidthSteady(t *testing.T) {
	fd := textField("Command", "", "uptime", appdata.CmdMax)
	fd.input.Width = 50
	f := &form{title: "New command", fields: []*field{fd}}
	f.focusAt(0)
	want := lipgloss.Width(f.view())
	for _, v := range []string{"u", "uan", strings.Repeat("x", 80)} {
		fd.input.SetValue(v)
		if got := lipgloss.Width(f.view()); got != want {
			t.Errorf("%q: width %d, want %d", v, got, want)
		}
	}
}

// typing stops at the field's limit, and a counter shows from 80% full
func TestFieldLimitAndCounter(t *testing.T) {
	m, _ := setup(t)
	m = keys(t, m, "2", "a", strings.Repeat("x", 100))
	if strings.Contains(m.form.view(), "/127") {
		t.Fatal("counter shown below 80%")
	}
	m = keys(t, m, strings.Repeat("y", 50))
	if got := m.form.get("Command").input.Value(); len(got) != appdata.CmdMax {
		t.Fatalf("%d characters taken, want %d", len(got), appdata.CmdMax)
	}
	if !strings.Contains(m.form.view(), "127/127") {
		t.Fatalf("no counter:\n%s", m.form.view())
	}
	for label, limit := range map[string]int{"Alias": appdata.AliasMax, "User": appdata.UserMax, "Host": appdata.HostMax, "Port": 5} {
		m2, _ := setup(t)
		m2 = keys(t, m2, "a")
		if got := m2.form.get(label).input.CharLimit; got != limit {
			t.Errorf("%s: limit %d, want %d", label, got, limit)
		}
	}
}
