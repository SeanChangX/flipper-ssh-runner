// SPDX-License-Identifier: GPL-3.0-only
package tui

import (
	"fmt"
	"strings"

	"github.com/charmbracelet/bubbles/textarea"
	"github.com/charmbracelet/bubbles/textinput"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

type fieldKind int

const (
	kText fieldKind = iota
	kChoice
	kLines // several lines (commands), one per line
)

type field struct {
	label   string
	kind    fieldKind
	input   textinput.Model
	area    textarea.Model
	choices []string
	pick    int
	hint    string
	shown   func(f *form) bool // nil: always
}

func (fd *field) value() string {
	switch fd.kind {
	case kChoice:
		if fd.pick < len(fd.choices) {
			return fd.choices[fd.pick]
		}
		return ""
	case kLines:
		return fd.area.Value()
	}
	return strings.TrimSpace(fd.input.Value())
}

// form is a dialog of fields: tab / shift+tab or up / down move, left / right pick a
// choice, ctrl+s or enter on the last field saves, esc cancels.
type form struct {
	title  string
	fields []*field
	focus  int
	err    string
}

func textField(label, value, hint string, limit int) *field {
	in := textinput.New()
	in.Prompt = ""
	in.SetValue(value)
	in.CharLimit = limit
	in.Placeholder = hint
	in.Width = 36
	in.PlaceholderStyle = sDim
	in.TextStyle = sText
	in.Cursor.Style = sAccent
	return &field{label: label, kind: kText, input: in}
}

// inputView pads the input to a fixed width: textinput draws its placeholder in Width cells
// but text plus the cursor cell in Width+1, which shifted the centred dialog at the first key.
func inputView(in textinput.Model) string {
	return lipgloss.NewStyle().Width(in.Width + 1).Render(in.View())
}

func choiceField(label string, choices []string, value string) *field {
	f := &field{label: label, kind: kChoice, choices: choices}
	for i, c := range choices {
		if c == value {
			f.pick = i
		}
	}
	return f
}

func linesField(label string, lines []string, hint string) *field {
	a := textarea.New()
	a.SetValue(strings.Join(lines, "\n"))
	a.Placeholder = "" // the hint shows dimmed above it instead: a placeholder read as text
	a.ShowLineNumbers = false
	a.Prompt = ""
	a.SetWidth(40)
	a.SetHeight(4)
	a.FocusedStyle.CursorLine = lipgloss.NewStyle()
	a.FocusedStyle.Placeholder = sDim
	a.BlurredStyle.Placeholder = sDim
	a.FocusedStyle.Text = sText
	a.BlurredStyle.Text = sText
	return &field{label: label, kind: kLines, area: a, hint: hint}
}

func (f *form) visible() []int {
	var out []int
	for i, fd := range f.fields {
		if fd.shown == nil || fd.shown(f) {
			out = append(out, i)
		}
	}
	return out
}

func (f *form) get(label string) *field {
	for _, fd := range f.fields {
		if fd.label == label {
			return fd
		}
	}
	return nil
}

func (f *form) focusAt(i int) tea.Cmd {
	for j, fd := range f.fields {
		fd.input.Blur()
		fd.area.Blur()
		if j == i {
			switch fd.kind {
			case kText:
				return fd.input.Focus()
			case kLines:
				return fd.area.Focus()
			}
		}
	}
	return nil
}

func (f *form) move(d int) tea.Cmd {
	vis := f.visible()
	pos := 0
	for k, i := range vis {
		if i == f.focus {
			pos = k
		}
	}
	pos = (pos + d + len(vis)) % len(vis)
	f.focus = vis[pos]
	return f.focusAt(f.focus)
}

// update returns save true when the user saves, cancel true when they leave.
func (f *form) update(msg tea.Msg) (cmd tea.Cmd, save, cancel bool) {
	fd := f.fields[f.focus]
	if k, ok := msg.(tea.KeyMsg); ok {
		switch k.String() {
		case "esc":
			return nil, false, true
		case "ctrl+s":
			return nil, true, false
		case "tab", "down":
			if fd.kind != kLines || k.String() == "tab" {
				return f.move(1), false, false
			}
		case "shift+tab", "up":
			if fd.kind != kLines || k.String() == "shift+tab" {
				return f.move(-1), false, false
			}
		case "enter":
			if fd.kind != kLines {
				vis := f.visible()
				if f.focus == vis[len(vis)-1] {
					return nil, true, false
				}
				return f.move(1), false, false
			}
		case "left", "h":
			if fd.kind == kChoice {
				fd.pick = (fd.pick + len(fd.choices) - 1) % len(fd.choices)
				return nil, false, false
			}
		case "right", "l", " ":
			if fd.kind == kChoice {
				fd.pick = (fd.pick + 1) % len(fd.choices)
				return nil, false, false
			}
		}
	}
	switch fd.kind {
	case kText:
		fd.input, cmd = fd.input.Update(msg)
	case kLines:
		fd.area, cmd = fd.area.Update(msg)
	}
	return cmd, false, false
}

func (f *form) view() string {
	var b strings.Builder
	b.WriteString(sTitle.Render(f.title) + "\n\n")
	for _, i := range f.visible() {
		fd := f.fields[i]
		on := i == f.focus
		label := sLabel.Render(fd.label)
		if on {
			label = sLabelOn.Render(fd.label)
		}
		var v string
		switch fd.kind {
		case kText:
			v = " " + inputView(fd.input) + counter(fd.input) // in line with the choices' first letter
		case kChoice:
			var parts []string
			for j, c := range fd.choices {
				switch {
				case j == fd.pick && on:
					parts = append(parts, sRowOn.Render(" "+c+" "))
				case j == fd.pick:
					parts = append(parts, sText.Bold(true).Render(" "+c+" "))
				default:
					parts = append(parts, sDim.Render(" "+c+" "))
				}
			}
			v = strings.Join(parts, " ")
		case kLines:
			v = fd.area.View()
		}
		if fd.kind == kLines {
			b.WriteString(label + " " + sDim.Render(fd.hint) + "\n" + v + "\n")
			continue
		} else {
			b.WriteString(label + v + "\n")
		}
		if on && fd.hint != "" {
			b.WriteString(sLabel.Render("") + sDim.Render(fd.hint) + "\n")
		}
	}
	if f.err != "" {
		b.WriteString("\n" + sFieldErr.Render(f.err) + "\n")
	}
	b.WriteString("\n" + help([][2]string{{"tab", "next"}, {"←→", "choose"}, {"ctrl+s", "save"}, {"esc", "cancel"}}))
	return sDialog.Render(b.String())
}

// counter shows "used/limit" once a field is 80% full; its room is kept when hidden, so the
// dialog does not change width while typing. Typing stops at the limit (CharLimit).
func counter(in textinput.Model) string {
	lim := in.CharLimit
	if lim <= 0 {
		return ""
	}
	n := len([]rune(in.Value()))
	room := len(fmt.Sprintf("%d/%d", lim, lim))
	if n*5 < lim*4 {
		return " " + strings.Repeat(" ", room)
	}
	c := fmt.Sprintf("%*s", room, fmt.Sprintf("%d/%d", n, lim))
	if n >= lim {
		return " " + sWarn.Render(c)
	}
	return " " + sDim.Render(c)
}

// help renders "key action · key action".
func help(items [][2]string) string {
	var parts []string
	for _, it := range items {
		parts = append(parts, sKey.Render(it[0])+" "+sHelp.Render(it[1]))
	}
	return strings.Join(parts, sDim.Render("  ·  "))
}
