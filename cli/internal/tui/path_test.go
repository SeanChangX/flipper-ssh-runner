// SPDX-License-Identifier: GPL-3.0-only
package tui

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

// a folder like a user's: keys, their .pub, scripts, archives, a folder with a space
func tree(t *testing.T) string {
	t.Helper()
	d := t.TempDir()
	for _, f := range []string{"id_ed25519", "id_ed25519.pub", "id_rsa", "work.pem", "notes.txt", "deploy.sh",
		"backup.zip", "old.tar.gz", ".hidden.pem"} {
		os.WriteFile(filepath.Join(d, f), nil, 0o600)
	}
	for _, sub := range []string{"keys", "My Docs", "kits"} {
		os.MkdirAll(filepath.Join(d, sub), 0o700)
	}
	os.WriteFile(filepath.Join(d, "My Docs", "id_home"), nil, 0o600)
	return d
}

func TestCompleteUniquePrefix(t *testing.T) {
	d := tree(t)
	r := pathRules{home: d}
	got, matches := r.complete(d+"/wo", pathKey)
	if got != d+"/work.pem" || len(matches) != 1 {
		t.Fatalf("%q %v", got, matches)
	}
	got, _ = r.complete(d+"/My", pathKey) // a folder: the separator is added
	if got != d+"/My Docs/" {
		t.Fatalf("%q", got)
	}
}

func TestCompleteSeveralMatches(t *testing.T) {
	d := tree(t)
	r := pathRules{home: d}
	got, matches := r.complete(d+"/id", pathKey)
	if got != d+"/id_" {
		t.Fatalf("longest common prefix: %q", got)
	}
	if want := []string{"id_ed25519", "id_rsa"}; !reflect.DeepEqual(matches, want) { // no .pub
		t.Fatalf("%v", matches)
	}
	_, matches = r.complete(d+"/k", pathKey)
	if want := []string{"keys/", "kits/"}; !reflect.DeepEqual(matches, want) {
		t.Fatalf("%v", matches)
	}
}

func TestCompleteHome(t *testing.T) {
	d := tree(t)
	r := pathRules{home: d}
	got, _ := r.complete("~/de", pathScript)
	if got != "~/deploy.sh" {
		t.Fatalf("%q", got)
	}
	if p := r.cleanPath("~/deploy.sh"); p != filepath.Join(d, "deploy.sh") {
		t.Fatalf("%q", p)
	}
	if _, matches := r.complete("~", pathScript); len(matches) == 0 {
		t.Fatal("~ alone lists the home folder")
	}
}

func TestCompleteWindowsSeparators(t *testing.T) {
	d := tree(t)
	r := pathRules{home: d, windows: true}
	got, _ := r.complete(d+`\WO`, pathKey) // backslash and any case
	if got != d+`\work.pem` {
		t.Fatalf("%q", got)
	}
	got, _ = r.complete(`~\my`, pathKey)
	if got != `~\My Docs\` {
		t.Fatalf("%q", got)
	}
	if p := r.cleanPath(`~\My Docs\id_home`); p != filepath.Join(d, "My Docs", "id_home") {
		t.Fatalf("%q", p)
	}
	unix := pathRules{home: d}
	if got, matches := unix.complete(d+`\wo`, pathKey); got != d+`\wo` || matches != nil {
		t.Fatalf("a backslash is a name character off Windows: %q %v", got, matches)
	}
}

func TestCompleteSpacesAndQuotes(t *testing.T) {
	d := tree(t)
	r := pathRules{home: d}
	got, _ := r.complete(`"`+d+"/My Docs/id", pathKey)
	if got != `"`+d+`/My Docs/id_home"` {
		t.Fatalf("%q", got)
	}
	if p := r.cleanPath(got); p != d+"/My Docs/id_home" {
		t.Fatalf("%q", p)
	}
	got, _ = r.complete(d+"/My Docs/id", pathKey) // unquoted works too: the field is one value
	if got != d+"/My Docs/id_home" {
		t.Fatalf("%q", got)
	}
}

func TestCompleteNoMatch(t *testing.T) {
	d := tree(t)
	r := pathRules{home: d}
	for _, v := range []string{d + "/zzz", d + "/missing/x", d + "/notes"} { // notes.txt is no key
		if got, matches := r.complete(v, pathKey); got != v || matches != nil {
			t.Fatalf("%q -> %q %v", v, got, matches)
		}
	}
	if _, matches := r.complete(d+"/.hid", pathKey); len(matches) != 1 {
		t.Fatal("a hidden name shows when asked for with a dot")
	}
}

func names(p *picker) []string {
	var out []string
	for _, r := range p.rows {
		out = append(out, r.name)
	}
	return out
}

func TestPickerFilters(t *testing.T) {
	d := tree(t)
	cases := map[pathKind][]string{
		pathKey:     {"..", "My Docs", "keys", "kits", "id_ed25519", "id_rsa", "work.pem"}, // dotfiles hidden
		pathScript:  {"..", "My Docs", "keys", "kits", "deploy.sh"},
		pathArchive: {"..", "My Docs", "keys", "kits", "backup.zip", "old.tar.gz"},
		pathExport:  {"[ save here ]", "..", "My Docs", "keys", "kits"},
	}
	for kind, want := range cases {
		p := newPicker(kind, d)
		got := names(p)
		// folders first, then files; ReadDir's order is by name
		if strings.Join(got, ",") != strings.Join(sortedLike(want, got), ",") || len(got) != len(want) {
			t.Fatalf("kind %d: %v, want %v", kind, got, want)
		}
	}
}

// sortedLike: want, in the order got lists the same names (ReadDir sorts by byte order).
func sortedLike(want, got []string) []string {
	set := map[string]bool{}
	for _, w := range want {
		set[w] = true
	}
	var out []string
	for _, g := range got {
		if set[g] {
			out = append(out, g)
		}
	}
	return out
}

func TestPickerNavigatesAndFilters(t *testing.T) {
	d := tree(t)
	h := newPicker(pathKey, d)
	h.key(".") // a filter starting with a dot shows hidden entries
	if got := names(h); len(got) == 0 || got[0] != ".hidden.pem" {
		t.Fatalf("dot filter: %v", got)
	}
	p := newPicker(pathKey, d)
	for _, k := range []string{"r", "s", "a"} { // type to filter
		p.key(k)
	}
	if got := names(p); !reflect.DeepEqual(got, []string{"id_rsa"}) {
		t.Fatalf("%v", got)
	}
	chosen, done := p.key("enter")
	if !done || chosen != filepath.Join(d, "id_rsa") {
		t.Fatalf("%q %v", chosen, done)
	}
	p = newPicker(pathKey, d)
	p.key("M") // "My Docs"
	p.key("enter")
	if p.dir != filepath.Join(d, "My Docs") || !reflect.DeepEqual(names(p), []string{"..", "id_home"}) {
		t.Fatalf("%s %v", p.dir, names(p))
	}
	p.key("backspace") // up again
	if p.dir != d {
		t.Fatalf("%s", p.dir)
	}
	e := newPicker(pathExport, d)
	if chosen, done := e.key("enter"); !done || chosen != d { // "save here"
		t.Fatalf("%q %v", chosen, done)
	}
	if _, done := newPicker(pathKey, d).key("esc"); !done {
		t.Fatal("esc does not close the picker")
	}
}

func TestPickerNeverTraps(t *testing.T) {
	d := t.TempDir()
	keys := filepath.Join(d, "keys")
	os.MkdirAll(keys, 0o700)
	os.WriteFile(filepath.Join(keys, "work.pub"), nil, 0o644)
	p := newPicker(pathKey, keys)
	if !reflect.DeepEqual(names(p), []string{".."}) || !strings.Contains(p.note, "Only public keys here") {
		t.Fatalf("%v %q", names(p), p.note)
	}
	if _, done := p.key("enter"); done || p.dir != d { // ".." goes up
		t.Fatalf("%s", p.dir)
	}
	for kind, want := range map[pathKind]string{pathKey: "No private keys", pathScript: "No scripts", pathArchive: "No backup archives"} {
		if n := newPicker(kind, keys).note; kind != pathKey && !strings.HasPrefix(n, want) {
			t.Fatalf("kind %d: %q", kind, n)
		}
	}
	e := newPicker(pathScript, t.TempDir())
	if !strings.HasPrefix(e.note, "No scripts") {
		t.Fatalf("%q", e.note)
	}
	e.key("z")
	if e.note != "Nothing here matches the filter." || len(names(e)) != 0 {
		t.Fatalf("%v %q", names(e), e.note)
	}
	if newPicker(pathExport, keys).note != "" {
		t.Fatal("a folder picker needs no files")
	}
}

func TestPickerStart(t *testing.T) {
	home := t.TempDir()
	r := pathRules{home: home}
	if got := pickerStart(r, "", pathKey); got != home {
		t.Fatalf("no ~/.ssh: %q", got)
	}
	ssh := filepath.Join(home, ".ssh")
	os.MkdirAll(ssh, 0o700)
	if got := pickerStart(r, "", pathKey); got != ssh {
		t.Fatalf("keys: %q", got)
	}
	if got := pickerStart(r, "", pathScript); got != home {
		t.Fatalf("scripts: %q", got)
	}
	if got := pickerStart(r, "~/.ssh/id_ed25519", pathScript); got != ssh {
		t.Fatalf("typed: %q", got)
	}
}
