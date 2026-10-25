// Package appdata reads and writes SSH Runner's files on the Flipper's SD card
// (apps_data/ssh_runner): hosts, global commands, keys and scripts, with the same rules as
// the app (flipper-app/core/profile.c, ffkv.c). The canonical text is byte for byte what
// the app writes; tests check both against the same golden files.
//
// SPDX-License-Identifier: GPL-3.0-only
package appdata

import (
	"fmt"
	"strconv"
	"strings"
)

// FileVersion is the "Version:" every file carries (FILE_VERSION).
const FileVersion = 1

// Pair is one "Key: value" line.
type Pair struct{ Key, Value string }

// kv is a parsed file: pairs in order, as ffkv_parse keeps them.
type kv []Pair

// parseKV follows ffkv_parse: lines split on '\n', spaces, tabs and a trailing '\r' trimmed,
// '#' lines and lines without ':' skipped, the key up to the first ':'.
func parseKV(text string) kv {
	var out kv
	for _, line := range strings.Split(text, "\n") {
		l := strings.Trim(line, " \t\r")
		if l == "" || l[0] == '#' {
			continue
		}
		i := strings.IndexByte(l, ':')
		if i < 0 {
			continue
		}
		key := strings.Trim(l[:i], " \t")
		if key == "" {
			continue
		}
		out = append(out, Pair{key, strings.Trim(l[i+1:], " \t\r")})
	}
	return out
}

func (k kv) get(key string) (string, bool) {
	for _, p := range k {
		if p.Key == key {
			return p.Value, true
		}
	}
	return "", false
}

func (k kv) all(key string) []string {
	var out []string
	for _, p := range k {
		if p.Key == key {
			out = append(out, p.Value)
		}
	}
	return out
}

// checkHeader follows ffkv_check_header: the Filetype matches and 1 <= Version <= FileVersion.
func (k kv) checkHeader(filetype string) error {
	ft, ok1 := k.get("Filetype")
	ver, ok2 := k.get("Version")
	if !ok1 || !ok2 || ft != filetype {
		return fmt.Errorf("not a %q file", filetype)
	}
	v, err := strconv.ParseUint(ver, 10, 32)
	if err != nil || v < 1 || v > FileVersion {
		return fmt.Errorf("unsupported version %q", ver)
	}
	return nil
}

// writer builds the canonical text (ffkv_write): "Key: value\n".
type writer struct {
	b   strings.Builder
	err error
}

func (w *writer) header(filetype string) {
	w.put("Filetype", filetype)
	w.put("Version", strconv.Itoa(FileVersion))
}

func (w *writer) put(key, value string) {
	if strings.ContainsAny(value, "\r\n") || strings.ContainsAny(key, ":\r\n") {
		if w.err == nil {
			w.err = fmt.Errorf("%s: line breaks are not allowed", key)
		}
		return
	}
	w.b.WriteString(key)
	w.b.WriteString(": ")
	w.b.WriteString(value)
	w.b.WriteByte('\n')
}

func (w *writer) comment(text string) {
	w.b.WriteString("# ")
	w.b.WriteString(text)
	w.b.WriteByte('\n')
}
