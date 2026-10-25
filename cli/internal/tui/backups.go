// SPDX-License-Identifier: GPL-3.0-only
package tui

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/conf"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/plural"
)

// The Backups tab: the local backups, newest first, to restore, export, import or delete.

func (m *Model) refreshBackups() {
	list, err := conf.ListBackups(m.o.BackupDir)
	if err != nil {
		m.say("Backups: "+err.Error(), true)
	}
	m.backups = list
	if m.row[tabBackups] >= len(list) {
		m.row[tabBackups] = max(len(list)-1, 0)
	}
}

func (m Model) selectedBackup() (conf.BackupInfo, bool) {
	if len(m.backups) == 0 {
		return conf.BackupInfo{}, false
	}
	return m.backups[m.row[tabBackups]], true
}

// pruneAfter keeps the newest KeepBackups backups; `made` (just made) always stays.
func pruneAfter(dir string, keep int, made string) string {
	gone, err := conf.PruneBackups(dir, keep, made)
	if err != nil {
		return "; old backups not pruned: " + err.Error()
	}
	if len(gone) > 0 {
		return "; " + plural.N(len(gone), "old backup") + " deleted"
	}
	return ""
}

// backupKey handles the Backups tab's own keys; ok is false for the editor's global keys.
func (m Model) backupKey(k tea.KeyMsg) (Model, tea.Cmd, bool) {
	b, have := m.selectedBackup()
	switch k.String() {
	case "enter":
		if have {
			return m.startRestore(b), nil, true
		}
		return m, nil, true
	case "x":
		if have {
			cwd, _ := os.Getwd()
			mm, cmd := m.openPath(pathExport, filepath.Join(cwd, b.ID+".zip"))
			mm.pathFor = b.ID
			return mm, cmd, true
		}
		return m, nil, true
	case "i":
		mm, cmd := m.openPath(pathArchive, "")
		return mm, cmd, true
	case "b":
		m.mode, m.bkKeys = mBackupNow, true // complete by default, like any backup
		return m, nil, true
	case "d", "delete":
		if have {
			dir, id := m.o.BackupDir, b.ID
			m.confirm(fmt.Sprintf("Delete backup %s? This cannot be undone.", b.Created.Local().Format(time.DateTime)), func() tea.Cmd {
				return func() tea.Msg {
					if err := conf.DeleteBackup(dir, id); err != nil {
						return doneMsg{err: err}
					}
					return doneMsg{text: "Deleted backup " + id}
				}
			})
		}
		return m, nil, true
	}
	return m, nil, false
}

// startRestore shows what the backup would change on the Flipper, then asks.
func (m Model) startRestore(b conf.BackupInfo) Model {
	if m.st == nil {
		m.say("Not connected", true)
		return m
	}
	man, files, err := conf.ReadBackup(b.Dir)
	if err != nil {
		m.say("Cannot restore: "+err.Error(), true)
		return m
	}
	plan, err := conf.RestorePlan(m.st, man, files)
	if err != nil {
		m.say("Cannot restore: "+err.Error(), true)
		return m
	}
	if len(plan) == 0 {
		m.say("Nothing to restore: "+m.st.Describe()+" already matches this backup", false)
		return m
	}
	q := fmt.Sprintf("Restore the backup of %s to %s?", b.Created.Local().Format(time.DateTime), m.st.Describe())
	if p, err := m.plan(); err == nil && len(p) > 0 {
		q += " Your " + plural.N(len(p), "unpushed edit") + " will be dropped."
	}
	st, dir, keep, prog := m.st, m.o.BackupDir, m.o.KeepBackups, m.prog
	m.confirm(q, func() tea.Cmd {
		return func() tea.Msg {
			// the whole state, private keys included: undoing must bring back exactly what was there
			bdir, _, err := conf.BackupProgress(st, dir, true, prog.stepper("Backing up"))
			if err != nil {
				return doneMsg{err: fmt.Errorf("backup before the restore failed, nothing changed: %w", err)}
			}
			if err := conf.ApplyProgress(st, plan, prog.stepper("Restoring")); err != nil {
				return doneMsg{err: err}
			}
			return doneMsg{text: fmt.Sprintf("Restored %s; the state before is in %s", plural.N(len(plan), "change"), filepath.Base(bdir)) +
				pruneAfter(dir, keep, bdir)}
		}
	})
	m.preview = plan
	return m
}

func (m Model) exportBackup(file string) (Model, tea.Cmd) {
	var b conf.BackupInfo
	for _, x := range m.backups {
		if x.ID == m.pathFor {
			b = x
		}
	}
	if b.ID == "" {
		m.say("That backup is gone", true)
		m.mode = mList
		return m, nil
	}
	if st, err := os.Stat(file); err == nil && st.IsDir() {
		file = filepath.Join(file, b.ID+".zip")
	}
	if conf.ArchiveKind(file) == "" {
		m.say("Name the file .zip or .tar.gz", true)
		return m, nil
	}
	if err := conf.ExportBackup(b.Dir, file); err != nil {
		m.say(err.Error(), true)
		return m, nil
	}
	msg := "Exported to " + tildePath(file)
	if b.Keys {
		msg += " (it holds private keys: keep it safe)"
	}
	m.say(msg, false)
	m.mode = mList
	return m, nil
}

func (m Model) importBackup(file string) (Model, tea.Cmd) {
	dir, err := conf.ImportBackup(file, m.o.BackupDir)
	if err != nil {
		m.say("Not imported: "+err.Error(), true)
		return m, nil
	}
	m.refreshBackups()
	for i, b := range m.backups {
		if b.Dir == dir {
			m.row[tabBackups] = i
		}
	}
	m.say("Imported "+filepath.Base(dir)+" (enter restores it)", false)
	m.mode = mList
	return m, nil
}

func (m Model) updateBackupNow(k tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch k.String() {
	case "esc", "q", "n":
		m.mode = mList
		m.say("Cancelled", false)
	case "k", " ", "space":
		m.bkKeys = !m.bkKeys
	case "enter", "y":
		if m.st == nil {
			m.mode = mList
			m.say("Not connected", true)
			return m, nil
		}
		st, dir, keep, keys, prog := m.st, m.o.BackupDir, m.o.KeepBackups, m.bkKeys, m.prog
		m.mode = mBusy
		prog.reset("Backing up")
		return m, tea.Batch(m.spin.Tick, func() tea.Msg {
			bdir, man, err := conf.BackupProgress(st, dir, keys, prog.stepper("Backing up"))
			if err != nil {
				return doneMsg{err: err}
			}
			return doneMsg{text: fmt.Sprintf("Backed up %s to %s", plural.N(len(man.Files), "file"), filepath.Base(bdir)) + pruneAfter(dir, keep, bdir)}
		})
	}
	return m, nil
}

func (m Model) backupNowView() string {
	box := sTitle.Render("Back up now") + "\n\n" +
		sText.Render("Copies the data folder of "+m.st.Describe()) + "\n" +
		sText.Render("to "+tildePath(m.o.BackupDir)) + "\n\n"
	mark := "[ ]"
	if m.bkKeys {
		mark = "[x]"
	}
	box += sAccent.Render(mark) + sText.Render(" Include private keys") + "\n"
	if m.bkKeys {
		box += sDim.Render("    Stored only on this computer, readable by you only.") + "\n"
	} else {
		box += sWarn.Render("    Off: this backup cannot bring the private keys back.") + "\n"
	}
	return box + "\n" + help([][2]string{{"enter", "back up"}, {"k", "keys on/off"}, {"esc", "cancel"}})
}

// backupRows: the Backups tab's table.
func (m Model) backupRows() (head []string, rows [][]string, note string) {
	head = []string{"CREATED", "SOURCE", "HOSTS", "KEYS", "SIZE"}
	for _, b := range m.backups {
		keys := "no"
		if b.Keys {
			keys = "yes"
		}
		rows = append(rows, []string{b.Created.Local().Format(time.DateTime), b.Source, strconv.Itoa(b.Hosts), keys, humanSize(b.Size)})
	}
	keep := "keeps all"
	if m.o.KeepBackups > 0 {
		keep = fmt.Sprintf("keeps the newest %d", m.o.KeepBackups)
	}
	note = "In " + tildePath(m.o.BackupDir) + " (" + keep + "). Every push and restore backs up first."
	return
}

func humanSize(n int64) string {
	switch {
	case n >= 1<<20:
		return fmt.Sprintf("%.1f MB", float64(n)/(1<<20))
	case n >= 1<<10:
		return fmt.Sprintf("%.1f KB", float64(n)/(1<<10))
	}
	return fmt.Sprintf("%d B", n)
}

// ---------------- path fields: Tab completion and the picker ----------------

func (m Model) openPath(kind pathKind, value string) (Model, tea.Cmd) {
	m.mode, m.pathKind, m.matches, m.matchSel = mPath, kind, nil, 0
	m.path.SetValue(value)
	m.path.CursorEnd()
	m.path.Placeholder = map[pathKind]string{pathKey: "~/.ssh/id_ed25519", pathScript: "./backup.sh",
		pathArchive: "./sshrunner-backup.zip", pathExport: "./backup.zip"}[kind]
	return m, m.path.Focus()
}

// pathKey handles completion keys in a path field; ok is false for keys the field edits with.
func (m Model) pathKeyMsg(k tea.KeyMsg) (Model, tea.Cmd, bool) {
	switch k.String() {
	case "tab":
		if len(m.matches) > 0 { // second Tab: cycle the list
			m.matchSel = (m.matchSel + 1) % len(m.matches)
			return m, nil, true
		}
		v, matches := m.rules.complete(m.path.Value(), m.pathKind)
		m.path.SetValue(v)
		m.path.CursorEnd()
		if len(matches) > 1 && v == m.lastTab {
			m.matches, m.matchSel = matches, 0
		}
		if len(matches) == 0 {
			m.say("No match", true)
		} else {
			m.say("", false)
		}
		m.lastTab = v
		return m, nil, true
	case "up", "down":
		if len(m.matches) > 0 {
			d := map[string]int{"up": -1, "down": 1}[k.String()]
			m.matchSel = (m.matchSel + d + len(m.matches)) % len(m.matches)
			return m, nil, true
		}
	case "enter":
		if len(m.matches) > 0 { // accept the highlighted match
			v, _ := m.rules.complete(m.path.Value(), m.pathKind)
			dir := v[:len(v)-len(lastPart(m.rules, v))]
			m.path.SetValue(dir + m.matches[m.matchSel])
			m.path.CursorEnd()
			m.matches, m.lastTab = nil, ""
			return m, nil, true
		}
	case "esc":
		if len(m.matches) > 0 {
			m.matches = nil
			return m, nil, true
		}
	case "ctrl+o":
		m.pick = newPicker(m.pathKind, pickerStart(m.rules, m.path.Value(), m.pathKind))
		m.mode = mPick
		m.matches, m.lastTab = nil, ""
		m.say("", false) // a "No match" from an earlier Tab is not about the picker
		return m, nil, true
	}
	m.matches, m.lastTab = nil, ""
	return m, nil, false
}

// lastPart: what follows the last separator (the part Tab completes).
func lastPart(r pathRules, v string) string {
	v = trimQuote(v)
	for i := len(v) - 1; i >= 0; i-- {
		if r.isSep(v[i]) {
			return v[i+1:]
		}
	}
	return v
}

func trimQuote(v string) string {
	if len(v) > 0 && v[0] == '"' {
		v = v[1:]
	}
	return v
}

func (m Model) updatePick(k tea.KeyMsg) (tea.Model, tea.Cmd) {
	chosen, done := m.pick.key(k.String())
	if !done {
		return m, nil
	}
	m.mode = mPath
	if chosen != "" {
		if m.pathKind == pathExport {
			if b, ok := m.selectedBackup(); ok {
				chosen = filepath.Join(chosen, b.ID+".zip")
			}
		}
		m.path.SetValue(chosen)
		m.path.CursorEnd()
	}
	m.pick = nil
	return m, m.path.Focus()
}

func (m Model) pathView() string {
	what := map[pathKind]string{pathKey: "Import a private key", pathScript: "Add a script",
		pathArchive: "Import a backup", pathExport: "Export the backup"}[m.pathKind]
	box := sTitle.Render(what) + "\n\n" + sLabel.Render("File") + inputView(m.path) + "\n"
	if len(m.matches) > 0 {
		for i, s := range m.matches {
			if i >= 8 {
				box += sDim.Render(fmt.Sprintf("  ... %d more", len(m.matches)-8)) + "\n"
				break
			}
			if i == m.matchSel {
				box += strings17() + sRowOn.Render(" "+s+" ") + "\n"
			} else {
				box += strings17() + sText.Render(" "+s) + "\n"
			}
		}
	}
	switch m.pathKind {
	case pathExport:
		if b, ok := m.selectedBackup(); ok && b.Keys {
			box += "\n" + sWarn.Render("This backup holds private keys: keep the file safe.") + "\n"
		}
		box += "\n" + sDim.Render(".zip or .tar.gz") + "\n"
	case pathArchive:
		box += "\n" + sDim.Render("A .zip or .tar.gz exported by sshrunner; it is checked first.") + "\n"
	}
	keys := [][2]string{{"enter", "ok"}, {"tab", "complete"}, {"ctrl+o", "browse"}, {"esc", "cancel"}}
	if len(m.matches) > 0 {
		keys = [][2]string{{"↑↓/tab", "choose"}, {"enter", "take"}, {"esc", "close list"}}
	}
	return box + "\n" + help(keys)
}

func strings17() string { return lipgloss.NewStyle().Width(17).Render("") }

func (m Model) pickView(height int) string {
	p := m.pick
	what := map[pathKind]string{pathKey: "Choose a private key (id_*, *.pem)", pathScript: "Choose a script (*.sh)",
		pathArchive: "Choose a backup archive (.zip, .tar.gz)", pathExport: "Choose a folder"}[p.kind]
	box := sTitle.Render(what) + "\n" + sDim.Render(tildePath(p.dir)) + "\n"
	if p.filter != "" {
		box += sAccent.Render("filter: ") + sText.Render(p.filter) + "\n"
	} else {
		box += sDim.Render("type to filter") + "\n"
	}
	box += "\n"
	visible := max(height-12, 3)
	start := 0
	if p.sel >= visible {
		start = p.sel - visible + 1
	}
	for i := start; i < len(p.rows) && i < start+visible; i++ {
		r := p.rows[i]
		name := r.name
		if r.isDir && !r.up {
			name += string(filepath.Separator)
		}
		if i == p.sel {
			box += sRowOn.Render(" "+name+" ") + "\n"
		} else if r.isDir || r.here {
			box += " " + sAccent.Render(name) + "\n"
		} else {
			box += " " + sText.Render(name) + "\n"
		}
	}
	if p.note != "" {
		box += sWarn.Render(p.note) + "\n"
	}
	if p.err != "" {
		box += sBad.Render(p.err) + "\n"
	}
	return box + "\n" + help([][2]string{{"enter", "open / choose"}, {"backspace", "up"}, {"esc", "back"}})
}
