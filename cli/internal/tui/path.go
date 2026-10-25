// SPDX-License-Identifier: GPL-3.0-only
package tui

import (
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/conf"
)

// What a path field is for: it picks the picker's filter and what Tab offers.
type pathKind int

const (
	pathKey     pathKind = iota // a private key: id_* or *.pem, never a .pub
	pathScript                  // a .sh file
	pathArchive                 // an exported backup: .zip, .tar.gz
	pathExport                  // where to write an export: any name; the picker picks a folder
)

// wants: does a file of this name belong in the picker / completion for kind k.
func (k pathKind) wants(name string) bool {
	l := strings.ToLower(name)
	switch k {
	case pathKey:
		return !strings.HasSuffix(l, ".pub") && (strings.HasPrefix(l, "id_") || strings.HasSuffix(l, ".pem"))
	case pathScript:
		return strings.HasSuffix(l, ".sh")
	case pathArchive:
		return conf.ArchiveKind(l) != ""
	case pathExport:
		return true
	}
	return false
}

// pathRules: how paths are spelled. Windows takes both separators and ignores case.
type pathRules struct {
	windows bool
	home    string
}

func hostRules() pathRules {
	home, _ := os.UserHomeDir()
	return pathRules{windows: runtime.GOOS == "windows", home: home}
}

func (r pathRules) isSep(c byte) bool { return c == '/' || (r.windows && c == '\\') }

// tildePath shows a path under the home folder as ~/..., as shells and file managers do.
func tildePath(p string) string {
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		return p
	}
	if rel, err := filepath.Rel(home, p); err == nil && !strings.HasPrefix(rel, "..") && !filepath.IsAbs(rel) {
		if rel == "." {
			return "~"
		}
		return "~" + string(filepath.Separator) + rel
	}
	return p
}

// cleanPath turns what was typed into a path: quotes off, ~ expanded.
func (r pathRules) cleanPath(v string) string {
	v = strings.TrimSpace(v)
	if len(v) >= 2 && v[0] == '"' && v[len(v)-1] == '"' {
		v = v[1 : len(v)-1]
	} else {
		v = strings.TrimPrefix(v, `"`)
	}
	if r.windows { // both separators, as Windows itself takes them
		v = strings.NewReplacer(`\`, string(filepath.Separator), "/", string(filepath.Separator)).Replace(v)
	}
	if v == "~" {
		return r.home
	}
	if len(v) >= 2 && v[0] == '~' && (r.isSep(v[1]) || v[1] == filepath.Separator) {
		return filepath.Join(r.home, v[2:])
	}
	return v
}

// complete is shell-like Tab completion of v: the longest prefix the matching entries share,
// plus every match (directories end with a separator) for a second Tab to show. The typed
// spelling (~, a quote, the separator used) is kept.
func (r pathRules) complete(v string, kind pathKind) (string, []string) {
	quoted := strings.HasPrefix(v, `"`)
	raw := strings.TrimPrefix(v, `"`)
	cut := -1
	for i := len(raw) - 1; i >= 0; i-- {
		if r.isSep(raw[i]) {
			cut = i
			break
		}
	}
	typedDir, base := "", raw
	if cut >= 0 {
		typedDir, base = raw[:cut+1], raw[cut+1:]
	}
	sep := "/"
	if cut >= 0 {
		sep = string(raw[cut])
	} else if r.windows {
		sep = `\`
	}
	dir := "."
	if typedDir != "" {
		dir = r.cleanPath(typedDir)
	} else if raw == "~" {
		return r.complete(v+sep, kind)
	}
	list, err := os.ReadDir(dir)
	if err != nil {
		return v, nil
	}
	norm := func(s string) string {
		if r.windows {
			return strings.ToLower(s)
		}
		return s
	}
	var matches []string
	for _, e := range list {
		name := e.Name()
		if !strings.HasPrefix(norm(name), norm(base)) {
			continue
		}
		if strings.HasPrefix(name, ".") && !strings.HasPrefix(base, ".") {
			continue // hidden unless asked for, as in a shell
		}
		isDir := e.IsDir()
		if !isDir && e.Type()&os.ModeSymlink != 0 {
			if st, err := os.Stat(filepath.Join(dir, name)); err == nil {
				isDir = st.IsDir()
			}
		}
		if isDir {
			matches = append(matches, name+sep)
		} else if kind.wants(name) {
			matches = append(matches, name)
		}
	}
	sort.Slice(matches, func(i, j int) bool { return norm(matches[i]) < norm(matches[j]) })
	if len(matches) == 0 {
		return v, nil
	}
	common := matches[0]
	for _, m := range matches[1:] {
		n := 0
		for n < len(common) && n < len(m) && norm(common[n:n+1]) == norm(m[n:n+1]) {
			n++
		}
		common = common[:n]
	}
	if len(common) < len(base) {
		common = base
	}
	out := typedDir + common
	if quoted {
		out = `"` + out
		if len(matches) == 1 && !strings.HasSuffix(common, sep) {
			out += `"`
		}
	}
	return out, matches
}

// ---------------- the file picker ----------------

// picker browses folders for one kind of file, with type-to-filter.
type picker struct {
	kind   pathKind
	dir    string
	filter string
	rows   []pickRow
	sel    int
	err    string
	note   string // why no file is listed here, and what to do
}

type pickRow struct {
	name  string
	isDir bool
	here  bool // "use this folder" (export)
	up    bool // "..": the parent folder
}

func newPicker(kind pathKind, start string) *picker {
	p := &picker{kind: kind}
	p.open(start)
	return p
}

func (p *picker) open(dir string) {
	if abs, err := filepath.Abs(dir); err == nil {
		dir = abs
	}
	p.dir, p.filter, p.sel, p.err = dir, "", 0, ""
	p.refresh()
	// start on the first entry, not on "save here" or ".."
	for p.sel < len(p.rows)-1 && p.rows[p.sel].up {
		p.sel++
	}
}

func (p *picker) refresh() {
	p.rows = nil
	if p.kind == pathExport {
		p.rows = append(p.rows, pickRow{name: "[ save here ]", here: true})
	}
	list, err := os.ReadDir(p.dir)
	if err != nil {
		p.err = err.Error()
	}
	if parent := filepath.Dir(p.dir); parent != p.dir && p.filter == "" {
		p.rows = append(p.rows, pickRow{name: "..", isDir: true, up: true})
	}
	f := strings.ToLower(p.filter)
	var dirs, files []pickRow
	pubs := 0
	for _, e := range list {
		name := e.Name()
		if f != "" && !strings.Contains(strings.ToLower(name), f) {
			continue
		}
		if strings.HasPrefix(name, ".") && !strings.HasPrefix(f, ".") {
			continue // hidden, unless the filter starts with a dot
		}
		isDir := e.IsDir()
		if !isDir && e.Type()&os.ModeSymlink != 0 {
			if st, err := os.Stat(filepath.Join(p.dir, name)); err == nil {
				isDir = st.IsDir()
			}
		}
		switch {
		case isDir:
			dirs = append(dirs, pickRow{name: name, isDir: true})
		case p.kind != pathExport && p.kind.wants(name):
			files = append(files, pickRow{name: name})
		case strings.HasSuffix(strings.ToLower(name), ".pub"):
			pubs++
		}
	}
	p.rows = append(append(p.rows, dirs...), files...)
	p.note = ""
	switch {
	case p.kind == pathExport || len(files) > 0 || p.err != "":
	case p.filter != "":
		p.note = "Nothing here matches the filter."
	case p.kind == pathKey && pubs > 0:
		p.note = "Only public keys here: choose the private key (the file without .pub)."
	default:
		p.note = map[pathKind]string{pathKey: "No private keys (id_*, *.pem) here.", pathScript: "No scripts (*.sh) here.",
			pathArchive: "No backup archives (.zip, .tar.gz) here."}[p.kind]
	}
	if p.sel >= len(p.rows) {
		p.sel = max(len(p.rows)-1, 0)
	}
}

// key handles one key; it returns the chosen path when the user picks one, and whether the
// picker is done (picked or cancelled).
func (p *picker) key(k string) (chosen string, done bool) {
	switch k {
	case "esc":
		return "", true
	case "up", "ctrl+p":
		if p.sel > 0 {
			p.sel--
		}
	case "down", "ctrl+n":
		if p.sel < len(p.rows)-1 {
			p.sel++
		}
	case "left":
		p.open(filepath.Dir(p.dir))
	case "backspace":
		if p.filter == "" {
			p.open(filepath.Dir(p.dir))
		} else {
			p.filter = p.filter[:len(p.filter)-1]
			p.refresh()
		}
	case "enter", "right":
		if len(p.rows) == 0 {
			return "", false
		}
		r := p.rows[p.sel]
		switch {
		case r.here:
			return p.dir, true
		case r.up:
			p.open(filepath.Dir(p.dir))
		case r.isDir:
			p.open(filepath.Join(p.dir, r.name))
		case k == "enter":
			return filepath.Join(p.dir, r.name), true
		}
	default:
		if len(k) == 1 && k[0] >= ' ' && k[0] < 0x7f {
			p.filter += k
			p.sel = 0
			p.refresh()
		}
	}
	return "", false
}

// pickerStart: the folder of what was typed, else ~/.ssh for keys (where keys usually are),
// else the home folder.
func pickerStart(r pathRules, typed string, kind pathKind) string {
	if v := r.cleanPath(typed); v != "" {
		if st, err := os.Stat(v); err == nil && st.IsDir() {
			return v
		}
		if st, err := os.Stat(filepath.Dir(v)); err == nil && st.IsDir() {
			return filepath.Dir(v)
		}
	}
	if kind == pathKey {
		if st, err := os.Stat(filepath.Join(r.home, ".ssh")); err == nil && st.IsDir() {
			return filepath.Join(r.home, ".ssh")
		}
	}
	if r.home != "" {
		return r.home
	}
	return "."
}
