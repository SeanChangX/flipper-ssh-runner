// SPDX-License-Identifier: GPL-3.0-only
package tui

import (
	"fmt"
	"strings"
	"sync"

	bhelp "github.com/charmbracelet/bubbles/help"
	bkey "github.com/charmbracelet/bubbles/key"
	"github.com/charmbracelet/bubbles/spinner"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/charmbracelet/x/ansi"
)

// programOptions: the alternate screen, and the mouse unless it is turned off.
func programOptions(o Options) []tea.ProgramOption {
	opts := []tea.ProgramOption{tea.WithAltScreen()}
	if o.Mouse {
		opts = append(opts, tea.WithMouseCellMotion())
	}
	return opts
}

// ---------------- the ? help overlay ----------------

func kb(keys, desc string) bkey.Binding {
	return bkey.NewBinding(bkey.WithKeys(strings.Fields(keys)...), bkey.WithHelp(keys, desc))
}

// helpGroups: every key of the current tab, then the global ones, then the file fields.
func (m Model) helpGroups() [][]bkey.Binding {
	var tab []bkey.Binding
	switch m.tab {
	case tabHosts:
		tab = []bkey.Binding{kb("a", "add a host"), kb("e enter", "edit"), kb("d", "delete")}
	case tabCommands:
		tab = []bkey.Binding{kb("a", "add a command"), kb("e enter", "edit"), kb("d", "delete"), kb("J K", "move down / up")}
	case tabKeys:
		tab = []bkey.Binding{kb("i", "import a private key"), kb("d", "delete")}
	case tabScripts:
		tab = []bkey.Binding{kb("i", "add a .sh script"), kb("d", "delete")}
	case tabBackups:
		tab = []bkey.Binding{kb("enter", "restore (changes shown first)"), kb("b", "back up now"),
			kb("x", "export as .zip / .tar.gz"), kb("i", "import an export"), kb("d", "delete")}
	}
	global := []bkey.Binding{kb("tab 1-5", "next tab / jump"), kb("↑ ↓ k j", "select"), kb("g G", "first / last"),
		kb("p", "push to the Flipper"), kb("r", "reload"), kb("q", "quit"), kb("?", "this help")}
	fields := []bkey.Binding{kb("tab", "complete a path"), kb("tab tab", "list the matches"), kb("ctrl+o", "browse files"),
		kb("ctrl+s", "save a form"), kb("esc", "cancel"), kb("y n", "answer (enter = no)")}
	mouse := []bkey.Binding{kb("click", "tab, row, key hint"), kb("wheel", "scroll")}
	if !m.o.Mouse {
		mouse = []bkey.Binding{kb("--no-mouse", "mouse off")}
	}
	return [][]bkey.Binding{tab, global, fields, mouse}
}

func (m Model) helpView() string {
	h := bhelp.New()
	h.Styles.FullKey = sKey
	h.Styles.FullDesc = sHelp
	h.Styles.FullSeparator = sDim
	h.FullSeparator = "    "
	g := m.helpGroups() // two rows of two columns: it fits an 80-column terminal
	return sTitle.Render("Keys: "+tabNames[m.tab]) + "\n\n" + h.FullHelpView(g[:2]) + "\n\n" + h.FullHelpView(g[2:]) +
		"\n\n" + help([][2]string{{"?", "close"}, {"esc", "close"}})
}

// ---------------- progress while busy ----------------

// busyProgress is written by the running command and read by View (another goroutine).
type busyProgress struct {
	mu   sync.Mutex
	what string
	i, n int
	path string
}

func (p *busyProgress) reset(what string) {
	p.mu.Lock()
	p.what, p.i, p.n, p.path = what, 0, 0, ""
	p.mu.Unlock()
}

// step is the progress callback of conf.BackupProgress / conf.ApplyProgress.
func (p *busyProgress) stepper(what string) func(i, n int, path string) {
	return func(i, n int, path string) {
		p.mu.Lock()
		p.what, p.i, p.n, p.path = what, i, n, path
		p.mu.Unlock()
	}
}

func (m Model) busyView() string {
	m.prog.mu.Lock()
	what, i, n, path := m.prog.what, m.prog.i, m.prog.n, m.prog.path
	m.prog.mu.Unlock()
	if what == "" {
		what = "Working"
	}
	s := m.spin.View() + " " + sText.Render(what+"...")
	if n > 0 {
		s += "\n\n" + sAccent.Render(fmt.Sprintf("[%d/%d] ", i, n)) + sDim.Render(path)
	}
	return s
}

func newSpinner() spinner.Model {
	return spinner.New(spinner.WithSpinner(spinner.MiniDot), spinner.WithStyle(sAccent))
}

// ---------------- the mouse ----------------

// screenLine: the rendered screen's line y as plain text.
func (m Model) screenLine(y int) string {
	lines := strings.Split(ansi.Strip(m.View()), "\n")
	if y < 0 || y >= len(lines) {
		return ""
	}
	return lines[y]
}

// col: the screen column where byte offset i of a plain line starts.
func col(line string, i int) int { return lipgloss.Width(line[:i]) }

// hintKey: the key of the "key action" hint under column x of a hint line ("a add · e edit").
func hintKey(line string, x int) (string, bool) {
	const sep = "  ·  "
	if !strings.Contains(line, sep) {
		return "", false
	}
	start := 0
	for start < len(line) {
		end := strings.Index(line[start:], sep)
		if end < 0 {
			end = len(line) - start
		}
		seg := line[start : start+end]
		// the hint itself, without the margin or a dialog's border around it
		item := strings.Trim(seg, " │")
		if item != "" {
			from := start + strings.Index(seg, item)
			if x >= col(line, from) && x < col(line, from+len(item)) {
				return strings.Fields(item)[0], true
			}
		}
		start += end + len(sep)
	}
	return "", false
}

// keyFor turns a hint's key text into the key it stands for ("n/enter" is n).
func keyFor(k string) (tea.KeyMsg, bool) {
	k = strings.Split(k, "/")[0]
	switch k {
	case "enter":
		return tea.KeyMsg{Type: tea.KeyEnter}, true
	case "esc":
		return tea.KeyMsg{Type: tea.KeyEsc}, true
	case "tab":
		return tea.KeyMsg{Type: tea.KeyTab}, true
	case "backspace":
		return tea.KeyMsg{Type: tea.KeyBackspace}, true
	case "ctrl+o":
		return tea.KeyMsg{Type: tea.KeyCtrlO}, true
	case "ctrl+s":
		return tea.KeyMsg{Type: tea.KeyCtrlS}, true
	case "?":
		return tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune("?")}, true
	}
	if len([]rune(k)) == 1 && k != "↑" && k != "↓" && k != "←" {
		return tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(k)}, true
	}
	return tea.KeyMsg{}, false
}

// Screen rows: the header is 4 lines under the top padding; a table's first row is 3 lines
// into the body (a blank line, the column heads, a blank line).
const (
	tabsY    = padTop + 2
	firstRow = padTop + 4 + 3
)

// tableWindow: the first visible row and how many fit, as table() draws them.
func (m Model) tableWindow() (start, visible int) {
	bodyH := max(m.ih()-lipgloss.Height(m.header())-lipgloss.Height(m.footer()), 1)
	visible = max(bodyH-7, 1)
	if sel := m.row[m.tab]; sel >= visible {
		start = sel - visible + 1
	}
	return start, visible
}

func (m Model) mouse(msg tea.MouseMsg) (tea.Model, tea.Cmd) {
	if msg.Action != tea.MouseActionPress {
		return m, nil
	}
	switch msg.Button {
	case tea.MouseButtonWheelUp, tea.MouseButtonWheelDown:
		k := map[tea.MouseButton]tea.KeyMsg{tea.MouseButtonWheelUp: {Type: tea.KeyUp}, tea.MouseButtonWheelDown: {Type: tea.KeyDown}}[msg.Button]
		switch m.mode {
		case mList, mPick:
			return m.Update(k)
		case mPath:
			if len(m.matches) > 0 {
				return m.Update(k)
			}
		}
		return m, nil
	case tea.MouseButtonLeft:
	default:
		return m, nil
	}
	line := m.screenLine(msg.Y)
	if m.mode == mList && msg.Y == tabsY {
		for i := range tabNames {
			label := fmt.Sprintf("%s %d", tabNames[i], m.tabCount(i))
			if at := strings.Index(line, label); at >= 0 && msg.X >= col(line, at)-2 && msg.X < col(line, at)+lipgloss.Width(label)+2 {
				m.tab = i
				if i == tabBackups {
					m.refreshBackups()
				}
				return m, nil
			}
		}
	}
	if m.mode == mList && m.cfg != nil && msg.Y >= firstRow {
		start, visible := m.tableWindow()
		if i := start + msg.Y - firstRow; msg.Y-firstRow < visible && i < m.count() {
			m.row[m.tab] = i
			return m, nil
		}
	}
	if k, ok := hintKey(line, msg.X); ok {
		if km, ok := keyFor(k); ok {
			return m.Update(km)
		}
	}
	return m, nil
}
