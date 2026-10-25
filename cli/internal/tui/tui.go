// Package tui is sshrunner's interactive editor (Bubble Tea): tabs for hosts, global
// commands, keys, scripts and the local backups, a form per host, and push with the changes
// shown first.
//
// SPDX-License-Identifier: GPL-3.0-only
package tui

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/charmbracelet/bubbles/spinner"
	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/appdata"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/conf"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/plural"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/store"
)

// Options say where the data folder is.
type Options struct {
	Port, Dir, BackupDir string
	KeepBackups          int  // backups kept after a change; 0 keeps all
	Mouse                bool // clicks and the wheel (off: the terminal's own text selection)
	// Store, when set, is used instead of opening one (tests).
	Store store.Store
}

// Run opens the store and runs the editor until the user quits.
func Run(o Options) error {
	_, err := tea.NewProgram(New(o), programOptions(o)...).Run()
	return err
}

const (
	tabHosts = iota
	tabCommands
	tabKeys
	tabScripts
	tabBackups
)

var tabNames = []string{"Hosts", "Commands", "Keys", "Scripts", "Backups"}

type mode int

const (
	mLoading mode = iota
	mList
	mForm
	mPath      // a file path (Tab completes, ctrl+o browses)
	mPick      // the file picker
	mBackupNow // back up now, with or without private keys
	mConfirm   // a yes / no question
	mHelp      // the ? overlay
	mBusy
	mFatal
)

// Model is the editor's state.
type Model struct {
	o       Options
	st      store.Store
	cfg     *conf.Config
	cur     map[string][]byte // the store's managed files as last loaded
	tab     int
	row     [5]int
	mode    mode
	form    *form
	editing int // index edited by the form, -1 for a new item
	path    textinput.Model
	ask     string
	onYes   func() tea.Cmd
	preview []conf.Change // what a confirmed restore changes

	backups  []conf.BackupInfo
	pathKind pathKind
	pathFor  string   // the backup an export is for
	matches  []string // a second Tab's list
	matchSel int
	lastTab  string // the value after the last Tab
	pick     *picker
	bkKeys   bool // back up now: with private keys
	rules    pathRules
	prog     *busyProgress // shared with the running command
	spin     spinner.Model
	status   string
	bad      bool
	w, h     int
}

// New is the editor before the store is open.
func New(o Options) Model {
	p := textinput.New()
	p.Prompt = ""
	p.Width = 48
	p.TextStyle = sText
	p.PlaceholderStyle = sDim
	return Model{o: o, mode: mLoading, path: p, editing: -1, rules: hostRules(), prog: &busyProgress{}, spin: newSpinner()}
}

type loadedMsg struct {
	st  store.Store
	cfg *conf.Config
	cur map[string][]byte
	err error
}

type doneMsg struct {
	text string
	err  error
}

func (m Model) load() tea.Cmd {
	o, st := m.o, m.st
	return func() tea.Msg {
		var err error
		if st == nil {
			switch {
			case o.Store != nil:
				st = o.Store
			case o.Dir != "":
				st, err = store.OpenDir(o.Dir)
			default:
				st, err = store.OpenFlipper(o.Port)
			}
			if err != nil {
				return loadedMsg{err: err}
			}
		}
		cfg, cur, err := conf.Load(st)
		return loadedMsg{st: st, cfg: cfg, cur: cur, err: err}
	}
}

// The terminal's window / tab title: the tool, then where it is working.
func (m Model) Init() tea.Cmd { return tea.Batch(tea.SetWindowTitle("sshrunner"), m.load()) }

// plan is what a push would do now (nil and an error when the edits are invalid).
func (m Model) plan() ([]conf.Change, error) {
	want, err := m.cfg.Files(m.cur)
	if err != nil {
		return nil, err
	}
	return conf.Plan(m.cur, want), nil
}

func (m Model) count() int {
	switch m.tab {
	case tabHosts:
		return len(m.cfg.Hosts)
	case tabCommands:
		return len(m.cfg.Commands)
	case tabKeys:
		return len(m.cfg.Keys)
	case tabBackups:
		return len(m.backups)
	}
	return len(m.cfg.Scripts)
}

func (m *Model) say(text string, bad bool) { m.status, m.bad = text, bad }

func (m Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.w, m.h = msg.Width, msg.Height
		return m, nil
	case tea.MouseMsg:
		return m.mouse(msg)
	case spinner.TickMsg:
		if m.mode != mBusy {
			return m, nil
		}
		var cmd tea.Cmd
		m.spin, cmd = m.spin.Update(msg)
		return m, cmd
	case loadedMsg:
		if msg.err != nil {
			m.mode = mFatal
			m.say(msg.err.Error(), true)
			return m, nil
		}
		// a reload after an action keeps the action's result in the status line
		announce := m.mode == mLoading && (m.status == "" || !m.bad)
		m.st, m.cfg, m.cur, m.mode = msg.st, msg.cfg, msg.cur, mList
		m.refreshBackups()
		if announce {
			m.say(fmt.Sprintf("Loaded from %s", m.st.Describe()), false)
		}
		return m, tea.SetWindowTitle("sshrunner · " + m.st.Describe())
	case removeMsg:
		return m.remove(msg), nil
	case doneMsg:
		m.mode = mList
		if msg.err != nil {
			m.say(msg.err.Error(), true)
			return m, nil
		}
		m.say(msg.text, false)
		return m, m.load() // read back what is there now
	case tea.KeyMsg:
		switch m.mode {
		case mFatal:
			return m, tea.Quit
		case mForm:
			return m.updateForm(msg)
		case mPath:
			return m.updatePath(msg)
		case mPick:
			return m.updatePick(msg)
		case mBackupNow:
			return m.updateBackupNow(msg)
		case mHelp:
			if s := msg.String(); s == "?" || s == "esc" || s == "q" || s == "enter" {
				m.mode = mList
			}
			return m, nil
		case mConfirm:
			switch msg.String() {
			case "y", "Y":
				m.mode, m.preview = mBusy, nil
				m.prog.reset("")
				return m, tea.Batch(m.onYes(), m.spin.Tick)
			case "n", "N", "esc", "q", "enter": // the safe answer is the default
				m.mode, m.preview = mList, nil
				m.say("Cancelled", false)
			}
			return m, nil
		case mList:
			return m.updateList(msg)
		}
		if msg.String() == "ctrl+c" {
			return m, tea.Quit
		}
	}
	if m.mode == mForm {
		return m.updateForm(msg)
	}
	return m, nil
}

func (m Model) updateList(k tea.KeyMsg) (tea.Model, tea.Cmd) {
	n := m.count()
	if m.tab == tabBackups {
		if mm, cmd, ok := m.backupKey(k); ok {
			return mm, cmd
		}
		if s := k.String(); s == "a" || s == "e" || s == "J" || s == "K" {
			return m, nil
		}
	}
	switch k.String() {
	case "ctrl+c":
		return m, tea.Quit
	case "?":
		m.mode = mHelp
		return m, nil
	case "q":
		if p, err := m.plan(); err == nil && len(p) > 0 {
			m.confirm("Quit and drop "+plural.N(len(p), "unpushed change")+"?", func() tea.Cmd { return tea.Quit })
			return m, nil
		}
		return m, tea.Quit
	case "tab", "right", "L":
		m.tab = (m.tab + 1) % len(tabNames)
		if m.tab == tabBackups {
			m.refreshBackups()
		}
	case "shift+tab", "left", "H":
		m.tab = (m.tab + len(tabNames) - 1) % len(tabNames)
		if m.tab == tabBackups {
			m.refreshBackups()
		}
	case "1", "2", "3", "4", "5":
		m.tab = int(k.String()[0] - '1')
		if m.tab == tabBackups {
			m.refreshBackups()
		}
	case "down", "j":
		if m.row[m.tab] < n-1 {
			m.row[m.tab]++
		}
	case "up", "k":
		if m.row[m.tab] > 0 {
			m.row[m.tab]--
		}
	case "g", "home":
		m.row[m.tab] = 0
	case "G", "end":
		m.row[m.tab] = max(n-1, 0)
	case "a":
		return m.openForm(-1)
	case "e", "enter":
		if n > 0 && m.tab != tabKeys && m.tab != tabScripts {
			return m.openForm(m.row[m.tab])
		}
	case "d", "delete":
		if n > 0 {
			m.confirmDelete()
		}
	case "i":
		if m.tab == tabKeys {
			return m.openPath(pathKey, "")
		}
		if m.tab == tabScripts {
			return m.openPath(pathScript, "")
		}
	case "K", "J":
		if m.tab == tabCommands && n > 1 {
			i, j := m.row[m.tab], m.row[m.tab]+map[string]int{"K": -1, "J": 1}[k.String()]
			if j >= 0 && j < n {
				m.cfg.Commands[i], m.cfg.Commands[j] = m.cfg.Commands[j], m.cfg.Commands[i]
				m.row[m.tab] = j
			}
		}
	case "p":
		return m.push()
	case "r":
		if p, err := m.plan(); err == nil && len(p) > 0 {
			m.confirm("Reload and drop "+plural.N(len(p), "unpushed change")+"?", func() tea.Cmd { return m.load() })
			return m, nil
		}
		m.mode = mLoading
		return m, m.load()
	}
	return m, nil
}

func (m *Model) confirm(q string, yes func() tea.Cmd) {
	m.mode, m.ask, m.onYes = mConfirm, q, yes
}

func (m *Model) confirmDelete() {
	i := m.row[m.tab]
	switch m.tab {
	case tabHosts:
		alias := m.cfg.Hosts[i].Alias
		m.confirm(fmt.Sprintf("Delete host %s?", alias), func() tea.Cmd {
			return func() tea.Msg { return removeMsg{tabHosts, i, "Deleted host " + alias + " (push to apply)"} }
		})
	case tabCommands:
		m.confirm(fmt.Sprintf("Delete command %q?", m.cfg.Commands[i]), func() tea.Cmd {
			return func() tea.Msg { return removeMsg{tabCommands, i, "Deleted the command (push to apply)"} }
		})
	case tabKeys:
		name := m.cfg.Keys[i].Name
		var users []string
		for _, h := range m.cfg.Hosts {
			if h.Auth == "key" && h.Key == name {
				users = append(users, h.Alias)
			}
		}
		q := fmt.Sprintf("Delete key %s from the Flipper?", name)
		if len(users) > 0 {
			q = fmt.Sprintf("Delete key %s? %s use it and move to the default key.", name, strings.Join(users, ", "))
		}
		m.confirm(q, func() tea.Cmd {
			return func() tea.Msg { return removeMsg{tabKeys, i, "Deleted key " + name + " (push to apply)"} }
		})
	case tabScripts:
		name := m.cfg.Scripts[i].Name
		m.confirm(fmt.Sprintf("Delete script %s?", name), func() tea.Cmd {
			return func() tea.Msg { return removeMsg{tabScripts, i, "Deleted script " + name + " (push to apply)"} }
		})
	}
}

type removeMsg struct {
	tab, i int
	text   string
}

func (m Model) remove(r removeMsg) Model {
	switch r.tab {
	case tabHosts:
		m.cfg.Hosts = append(m.cfg.Hosts[:r.i], m.cfg.Hosts[r.i+1:]...)
	case tabCommands:
		m.cfg.Commands = append(m.cfg.Commands[:r.i], m.cfg.Commands[r.i+1:]...)
	case tabKeys:
		name := m.cfg.Keys[r.i].Name
		m.cfg.Keys = append(m.cfg.Keys[:r.i], m.cfg.Keys[r.i+1:]...)
		for j := range m.cfg.Hosts { // as the app does when a key goes
			if h := &m.cfg.Hosts[j]; h.Auth == "key" && h.Key == name {
				h.Auth, h.Key = "default", ""
			}
		}
	case tabScripts:
		m.cfg.Scripts = append(m.cfg.Scripts[:r.i], m.cfg.Scripts[r.i+1:]...)
	}
	if m.row[r.tab] >= m.count() && m.row[r.tab] > 0 {
		m.row[r.tab]--
	}
	m.mode = mList
	m.say(r.text, false)
	return m
}

func (m Model) push() (tea.Model, tea.Cmd) {
	plan, err := m.plan()
	if err != nil {
		m.say("Not pushed: "+strings.ReplaceAll(err.Error(), "\n", "; "), true)
		return m, nil
	}
	if len(plan) == 0 {
		m.say("Nothing to push", false)
		return m, nil
	}
	st, dir, keep, prog := m.st, m.o.BackupDir, m.o.KeepBackups, m.prog
	m.confirm(fmt.Sprintf("Push %s to %s?", plural.N(len(plan), "change"), st.Describe()), func() tea.Cmd {
		return func() tea.Msg {
			// the whole state, private keys included: undoing must bring back exactly what was there
			bdir, _, err := conf.BackupProgress(st, dir, true, prog.stepper("Backing up"))
			if err != nil {
				return doneMsg{err: fmt.Errorf("backup before the push failed, nothing pushed: %w", err)}
			}
			if err := conf.ApplyProgress(st, plan, prog.stepper("Pushing")); err != nil {
				return doneMsg{err: err}
			}
			return doneMsg{text: fmt.Sprintf("Pushed %s; backup %s", plural.N(len(plan), "change"), filepath.Base(bdir)) + pruneAfter(dir, keep, bdir)}
		}
	})
	return m, nil
}

// ---------------- forms ----------------

func (m Model) openForm(i int) (tea.Model, tea.Cmd) {
	m.editing = i
	switch m.tab {
	case tabHosts:
		h := appdata.NewHost()
		title := "New host"
		if i >= 0 {
			h = m.cfg.Hosts[i]
			title = "Edit " + h.Alias
		}
		keys := []string{}
		for _, k := range m.cfg.Keys {
			keys = append(keys, k.Name)
		}
		port := strconv.Itoa(h.Port)
		f := &form{title: title, fields: []*field{
			textField("Alias", h.Alias, "nas", appdata.AliasMax),
			textField("User", h.User, "user", appdata.UserMax),
			textField("Host", h.Host, "192.168.1.10", appdata.HostMax),
			textField("Port", port, "22", 5),
			choiceField("Auth", appdata.Auths, h.Auth),
			choiceField("Key", keys, h.Key),
			choiceField("Save password", []string{"no", "yes"}, map[bool]string{true: "yes", false: "no"}[h.Remember]),
			choiceField("Transport", appdata.Transports, h.Transport),
			choiceField("Host key", appdata.Policies, h.Policy),
			linesField("Commands", h.Cmds, "one command per line"),
		}}
		f.get("Auth").hint = "default: the key set as default on the Flipper"
		f.get("Key").shown = func(f *form) bool { return f.get("Auth").value() == "key" }
		f.get("Save password").shown = func(f *form) bool { return f.get("Auth").value() == "password" }
		f.get("Save password").hint = "kept on the ESP32 after the first login, never on the SD card"
		f.get("Transport").hint = "auto: the interface whose network holds the host"
		f.get("Host key").hint = "prompt: ask for a new key, warn when it changes"
		if len(keys) == 0 {
			f.get("Key").choices = []string{"(no keys: import one first)"}
		}
		m.form = f
	case tabCommands:
		v := ""
		title := "New command"
		if i >= 0 {
			v, title = m.cfg.Commands[i], "Edit command"
		}
		fd := textField("Command", v, "uptime", appdata.CmdMax)
		fd.input.Width = 50
		m.form = &form{title: title, fields: []*field{fd}}
	default:
		return m, nil
	}
	m.mode = mForm
	m.form.focus = 0
	return m, m.form.focusAt(0)
}

func (m Model) updateForm(msg tea.Msg) (tea.Model, tea.Cmd) {
	cmd, save, cancel := m.form.update(msg)
	if cancel {
		m.mode = mList
		return m, nil
	}
	if !save {
		return m, cmd
	}
	f := m.form
	switch m.tab {
	case tabHosts:
		port, err := strconv.Atoi(f.get("Port").value())
		if err != nil {
			port = 0
		}
		h := appdata.Host{
			Alias: f.get("Alias").value(), User: f.get("User").value(), Host: f.get("Host").value(), Port: port,
			Auth: f.get("Auth").value(), Remember: f.get("Save password").value() == "yes",
			Transport: f.get("Transport").value(), Policy: f.get("Host key").value(),
		}
		if h.Auth == "key" {
			h.Key = f.get("Key").value()
		}
		if h.Auth != "password" {
			h.Remember = false
		}
		for _, l := range strings.Split(f.get("Commands").value(), "\n") {
			if l = strings.TrimSpace(l); l != "" {
				h.Cmds = append(h.Cmds, l)
			}
		}
		if err := h.Validate(); err != nil {
			f.err = strings.ReplaceAll(err.Error(), "\n", "\n")
			return m, nil
		}
		for j, o := range m.cfg.Hosts {
			if j != m.editing && strings.EqualFold(o.Alias, h.Alias) {
				f.err = "alias: " + o.Alias + " exists"
				return m, nil
			}
		}
		if m.editing >= 0 {
			m.cfg.Hosts[m.editing] = h
		} else {
			if len(m.cfg.Hosts) >= appdata.HostsMax {
				f.err = fmt.Sprintf("at most %d hosts", appdata.HostsMax)
				return m, nil
			}
			m.cfg.Hosts = append(m.cfg.Hosts, h)
			m.row[tabHosts] = len(m.cfg.Hosts) - 1
		}
		m.say("Saved "+h.Alias+" (push to apply)", false)
	case tabCommands:
		c := f.get("Command").value()
		if !appdata.CmdValid(c) {
			f.err = fmt.Sprintf("1-%d characters on one line", appdata.CmdMax)
			return m, nil
		}
		if m.editing >= 0 {
			m.cfg.Commands[m.editing] = c
		} else {
			if len(m.cfg.Commands) >= appdata.GlobalCmdsMax {
				f.err = fmt.Sprintf("at most %d commands", appdata.GlobalCmdsMax)
				return m, nil
			}
			m.cfg.Commands = append(m.cfg.Commands, c)
			m.row[tabCommands] = len(m.cfg.Commands) - 1
		}
		m.say("Saved the command (push to apply)", false)
	}
	m.mode = mList
	return m, nil
}

func (m Model) updatePath(k tea.KeyMsg) (tea.Model, tea.Cmd) {
	if mm, cmd, ok := m.pathKeyMsg(k); ok {
		return mm, cmd
	}
	switch k.String() {
	case "esc":
		m.mode = mList
		return m, nil
	case "enter":
		p := m.rules.cleanPath(m.path.Value())
		switch m.pathKind {
		case pathExport:
			return m.exportBackup(p)
		case pathArchive:
			return m.importBackup(p)
		}
		data, err := os.ReadFile(p)
		if err != nil {
			m.say(err.Error(), true)
			return m, nil
		}
		name := filepath.Base(p)
		if m.pathKind == pathKey {
			info := appdata.InspectKey(data)
			if info.Problem != "" {
				m.say(name+": "+info.Problem, true)
				return m, nil
			}
			if !appdata.KeyNameValid(name) {
				m.say(name+": rename the file (letters, digits, . _ -)", true)
				return m, nil
			}
			pub, _ := os.ReadFile(p + ".pub")
			k := conf.Key{Name: name, Private: data, Pub: pub, Info: info}
			replaced := false
			for j := range m.cfg.Keys {
				if m.cfg.Keys[j].Name == name {
					m.cfg.Keys[j], replaced = k, true
				}
			}
			if !replaced {
				m.cfg.Keys = append(m.cfg.Keys, k)
			}
			m.say(fmt.Sprintf("Imported %s (%s %s; push to apply)", name, info.Type, info.Format), false)
		} else {
			if !appdata.ScriptNameValid(name) {
				m.say(name+": a script is a .sh file", true)
				return m, nil
			}
			m.cfg.Scripts = append(m.cfg.Scripts, conf.Script{Name: name, Data: data})
			m.say("Added "+name+" (push to apply)", false)
		}
		m.mode = mList
		return m, nil
	}
	var cmd tea.Cmd
	m.path, cmd = m.path.Update(k)
	return m, cmd
}

// ---------------- view ----------------

// iw, ih: the screen inside the margins.
func (m Model) iw() int { return max(m.w-2*padX, 20) }
func (m Model) ih() int { return max(m.h-padTop-padBottom, 8) }

func (m Model) View() string {
	if m.w == 0 {
		return ""
	}
	header := m.header()
	footer := m.footer()
	bodyH := max(m.ih()-lipgloss.Height(header)-lipgloss.Height(footer), 1)
	var body string
	switch m.mode {
	case mLoading:
		body = "\n" + sDim.Render("Connecting...")
	case mFatal:
		body = "\n" + sBad.Render("Cannot open the data folder") + "\n\n" + sText.Render(m.status) +
			"\n\n" + sDim.Render("Close qFlipper and SSH Runner, unlock the Flipper, then start sshrunner again. Any key quits.")
	default:
		body = m.table(bodyH)
	}
	body = lipgloss.NewStyle().Height(bodyH).MaxHeight(bodyH).Render(body)
	switch m.mode {
	case mForm:
		body = lipgloss.Place(m.iw(), bodyH, lipgloss.Center, lipgloss.Center, m.form.view())
	case mPath:
		body = lipgloss.Place(m.iw(), bodyH, lipgloss.Center, lipgloss.Center, sDialog.Render(m.pathView()))
	case mPick:
		body = lipgloss.Place(m.iw(), bodyH, lipgloss.Center, lipgloss.Center, sDialog.Render(m.pickView(bodyH)))
	case mBackupNow:
		body = lipgloss.Place(m.iw(), bodyH, lipgloss.Center, lipgloss.Center, sDialog.Render(m.backupNowView()))
	case mHelp:
		body = lipgloss.Place(m.iw(), bodyH, lipgloss.Center, lipgloss.Center, sDialog.Render(m.helpView()))
	case mConfirm:
		box := sTitle.Render(m.ask) + "\n\n"
		if m.preview != nil {
			for i, ch := range m.preview {
				if i >= max(bodyH-12, 3) {
					box += sDim.Render(fmt.Sprintf("... and %d more", len(m.preview)-i)) + "\n"
					break
				}
				box += changeLine(ch) + "\n"
			}
			box += "\n" + sDim.Render("The Flipper's current state is backed up first.") + "\n\n"
		} else if strings.HasPrefix(m.ask, "Push") {
			if plan, err := m.plan(); err == nil {
				for _, ch := range plan {
					box += changeLine(ch) + "\n"
				}
				box += "\n" + sDim.Render("A backup is made first.") + "\n\n"
			}
		}
		box += help([][2]string{{"y", "yes"}, {"n/enter", "no"}})
		body = lipgloss.Place(m.iw(), bodyH, lipgloss.Center, lipgloss.Center, sDialog.Render(box))
	case mBusy:
		body = lipgloss.Place(m.iw(), bodyH, lipgloss.Center, lipgloss.Center, sDialog.Render(m.busyView()))
	}
	return lipgloss.NewStyle().Padding(padTop, padX, padBottom).Render(lipgloss.JoinVertical(lipgloss.Left, header, body, footer))
}

func changeLine(ch conf.Change) string {
	switch ch.Kind {
	case conf.Add:
		return sGood.Render("+ ") + sText.Render(ch.Path)
	case conf.Delete:
		return sBad.Render("- ") + sText.Render(ch.Path)
	}
	return sWarn.Render("~ ") + sText.Render(ch.Path)
}

func (m Model) header() string {
	// the version is for --version, as in other TUIs: the header says what and where
	left := sBrand.Render("SSH Runner") + sDim.Render("  config")
	right := sDim.Render("connecting")
	if m.st != nil {
		right = sDim.Render(m.st.Describe())
		if m.cfg != nil {
			if p, err := m.plan(); err != nil {
				right = sBad.Render("invalid edits") + sDim.Render("   ") + right
			} else if len(p) > 0 {
				right = sBadge.Render(fmt.Sprintf("%d unpushed", len(p))) + sDim.Render("   ") + right
			}
		}
	}
	gap := max(m.iw()-lipgloss.Width(left)-lipgloss.Width(right), 1)
	top := left + strings.Repeat(" ", gap) + right
	var tabs []string
	for i, n := range tabNames {
		label := fmt.Sprintf("%s %d", n, m.tabCount(i))
		if i == m.tab {
			tabs = append(tabs, sTabOn.Render(label))
		} else {
			tabs = append(tabs, sTabOff.Render(label))
		}
	}
	rule := lipgloss.NewStyle().Foreground(cRule).Render(strings.Repeat("─", m.iw()))
	return top + "\n\n" + strings.Join(tabs, " ") + "\n" + rule
}

func (m Model) tabCount(i int) int {
	if m.cfg == nil {
		return 0
	}
	return [5]int{len(m.cfg.Hosts), len(m.cfg.Commands), len(m.cfg.Keys), len(m.cfg.Scripts), len(m.backups)}[i]
}

func (m Model) footer() string {
	rule := lipgloss.NewStyle().Foreground(cRule).Render(strings.Repeat("─", m.iw()))
	st := sDim.Render(m.status)
	if m.bad {
		st = sBad.Render(m.status)
	}
	var keys [][2]string
	switch m.tab {
	case tabHosts:
		keys = [][2]string{{"a", "add"}, {"e", "edit"}, {"d", "delete"}}
	case tabCommands:
		keys = [][2]string{{"a", "add"}, {"e", "edit"}, {"d", "delete"}, {"J/K", "move"}}
	case tabKeys:
		keys = [][2]string{{"i", "import"}, {"d", "delete"}}
	case tabScripts:
		keys = [][2]string{{"i", "add"}, {"d", "delete"}}
	case tabBackups:
		keys = [][2]string{{"enter", "restore"}, {"b", "back up now"}, {"x", "export"}, {"i", "import"}, {"d", "delete"}}
	}
	keys = append(keys, [][2]string{{"p", "push"}, {"r", "reload"}, {"tab", "next"}, {"?", "help"}, {"q", "quit"}}...)
	return rule + "\n" + lipgloss.NewStyle().MaxWidth(m.iw()).Render(st) + "\n" + help(keys)
}

// table draws the current tab as aligned columns with the selected row highlighted.
func (m Model) table(height int) string {
	var head []string
	var rows [][]string
	var note string
	switch m.tab {
	case tabBackups:
		head, rows, note = m.backupRows()
	case tabHosts:
		head = []string{"ALIAS", "TARGET", "AUTH", "TRANSPORT", "HOST KEY", "CMDS"}
		for _, h := range m.cfg.Hosts {
			auth := h.Auth
			if h.Auth == "key" {
				auth = "key: " + h.Key
			} else if h.Auth == "password" && h.Remember {
				auth = "password, saved"
			}
			rows = append(rows, []string{h.Alias, fmt.Sprintf("%s@%s:%d", h.User, h.Host, h.Port), auth, h.Transport, h.Policy, strconv.Itoa(len(h.Cmds))})
		}
		note = "Hosts appear in the app's Hosts list. Passwords are never stored in these files."
	case tabCommands:
		head = []string{"#", "COMMAND"}
		for i, c := range m.cfg.Commands {
			rows = append(rows, []string{strconv.Itoa(i + 1), c})
		}
		note = "Global commands show in every host's run menu."
	case tabKeys:
		head = []string{"NAME", "TYPE", "FORMAT", "PASSPHRASE", "FINGERPRINT"}
		pubOnly := false
		for _, k := range m.cfg.Keys {
			pass := "none"
			if k.Info.Encrypted {
				pass = "yes"
			}
			if k.PublicOnly() {
				pass, pubOnly = "-", true
			}
			typ := k.Info.Type
			if k.Info.Problem != "" {
				typ = "! " + k.Info.Problem
			}
			rows = append(rows, []string{k.Name, typ, k.Info.Format, pass, k.Fingerprint()})
		}
		note = "Private keys stay on the Flipper; encrypted ones ask for the passphrase once."
		if pubOnly {
			note = "\"public key\": only the .pub is in this folder; its private key is not here (pull copies public keys only)."
		}
	case tabScripts:
		head = []string{"NAME", "SIZE", "FIRST LINE"}
		for _, s := range m.cfg.Scripts {
			first := strings.SplitN(string(s.Data), "\n", 2)[0]
			rows = append(rows, []string{s.Name, fmt.Sprintf("%d B", len(s.Data)), first})
		}
		note = "Scripts run on the host through the SSH session, without being copied there."
	}
	widths := make([]int, len(head))
	for i, h := range head {
		widths[i] = lipgloss.Width(h)
	}
	for _, r := range rows {
		for i, c := range r {
			widths[i] = max(widths[i], lipgloss.Width(c))
		}
	}
	// the last column takes what is left
	used := 2
	for i := range widths[:len(widths)-1] {
		widths[i] = min(widths[i], 34)
		used += widths[i] + 3
	}
	widths[len(widths)-1] = max(min(widths[len(widths)-1], m.iw()-used), 8)
	cell := func(s string, w int) string {
		if lipgloss.Width(s) > w {
			r := []rune(s)
			for lipgloss.Width(string(r))+1 > w && len(r) > 0 {
				r = r[:len(r)-1]
			}
			s = string(r) + "…"
		}
		return s + strings.Repeat(" ", w-lipgloss.Width(s))
	}
	line := func(cells []string) string {
		var parts []string
		for i, c := range cells {
			parts = append(parts, cell(c, widths[i]))
		}
		return strings.Join(parts, "   ")
	}
	var b strings.Builder
	b.WriteString("\n " + sDim.Render(line(head)) + "\n\n")
	if len(rows) == 0 {
		empty := map[int]string{tabHosts: "No hosts yet. Press a to add one.", tabCommands: "No global commands. Press a to add one.",
			tabKeys: "No keys. Press i to import one (e.g. ~/.ssh/id_ed25519).", tabScripts: "No scripts. Press i to add a .sh file.",
			tabBackups: "No backups yet. Press b to back up now, or i to import one."}[m.tab]
		b.WriteString(" " + sDim.Render(empty) + "\n")
	}
	sel := m.row[m.tab]
	visible := max(height-7, 1)
	start := 0
	if sel >= visible {
		start = sel - visible + 1
	}
	for i := start; i < len(rows) && i < start+visible; i++ {
		l := line(rows[i])
		if i == sel {
			// the Flipper's inverted look: black on orange, one space either side
			b.WriteString(sRowOn.Render(" "+l+" ") + "\n")
		} else {
			b.WriteString(" " + sText.Render(l) + " \n")
		}
	}
	if len(m.cfg.Unreadable) > 0 && m.tab == tabHosts {
		b.WriteString("\n " + sWarn.Render("Left as is: "+plural.N(len(m.cfg.Unreadable), "file")+" the app cannot read") + "\n")
	}
	b.WriteString("\n " + sDim.Render(note))
	return b.String()
}
