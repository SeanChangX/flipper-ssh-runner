// Package conf is the editable view of SSH Runner's data folder: hosts, global commands,
// keys and scripts. Load reads a store, Files gives the folder the edits describe, Plan the
// changes between the two, and Apply writes them. Only the paths the tool manages are
// planned (hosts/, keys/, scripts/, commands.conf); the app's own files (settings, recent
// list, saved output) are never touched.
//
// SPDX-License-Identifier: GPL-3.0-only
package conf

import (
	"bytes"
	"errors"
	"fmt"
	"path"
	"sort"
	"strings"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/appdata"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/store"
)

const (
	HostsDir     = "hosts"
	KeysDir      = "keys"
	ScriptsDir   = "scripts"
	CommandsFile = "commands.conf"
)

// Key is a file in keys/ and its .pub, if any. A key with only its .pub (Private nil) has its
// private file somewhere else: a folder pulled without --include-keys keeps it on the Flipper.
// It counts as a key, hosts may use it, and a push never writes or deletes its private file.
type Key struct {
	Name    string
	Private []byte
	Pub     []byte
	Info    appdata.KeyInfo
}

// PublicOnly: only the .pub is here; the private file is not in this folder.
func (k Key) PublicOnly() bool { return k.Private == nil }

// Fingerprint from the .pub, else from the OpenSSH file's public section ("" if neither).
func (k Key) Fingerprint() string {
	if b := appdata.PublicLine(string(k.Pub)); b != nil {
		return appdata.Fingerprint(b)
	}
	if b := appdata.OpenSSHPublic(k.Private); b != nil {
		return appdata.Fingerprint(b)
	}
	return ""
}

// Script is a file in scripts/.
type Script struct {
	Name string
	Data []byte
}

// Config is the data folder as the tool edits it.
type Config struct {
	Hosts    []appdata.Host
	Commands []string
	Keys     []Key
	Scripts  []Script
	// Unreadable is what Load could not use: managed path -> why (kept as it is on push).
	Unreadable map[string]string
}

// Load reads the managed files.
func Load(s store.Store) (*Config, map[string][]byte, error) {
	cur, err := Snapshot(s)
	if err != nil {
		return nil, nil, err
	}
	c := &Config{Unreadable: map[string]string{}}
	var names []string
	for p := range cur {
		names = append(names, p)
	}
	sort.Strings(names)
	for _, p := range names {
		data := cur[p]
		dir, base := path.Split(p)
		switch {
		case p == CommandsFile:
			cmds, err := appdata.ParseCommands(string(data))
			if err != nil {
				c.Unreadable[p] = err.Error()
				continue
			}
			c.Commands = cmds
		case dir == HostsDir+"/" && strings.HasSuffix(base, ".conf"):
			alias := strings.TrimSuffix(base, ".conf")
			h, err := appdata.ParseHost(string(data), alias)
			if err != nil || !appdata.AliasValid(alias) {
				if err == nil {
					err = errors.New("the file name is not a valid alias")
				}
				c.Unreadable[p] = err.Error()
				continue
			}
			c.Hosts = append(c.Hosts, h)
		case dir == KeysDir+"/" && !strings.HasSuffix(base, ".pub"):
			if !appdata.KeyNameValid(base) {
				c.Unreadable[p] = "not a valid key name"
				continue
			}
			c.Keys = append(c.Keys, Key{Name: base, Private: data, Pub: cur[p+".pub"], Info: appdata.InspectKey(data)})
		case dir == KeysDir+"/":
			// a .pub goes with its key; one without it is a key whose private file is elsewhere
			name := strings.TrimSuffix(base, ".pub")
			if _, ok := cur[KeysDir+"/"+name]; ok {
				continue
			}
			if !appdata.KeyNameValid(name) {
				c.Unreadable[p] = "not a valid key name"
				continue
			}
			c.Keys = append(c.Keys, Key{Name: name, Pub: data, Info: appdata.InspectPublic(data)})
		case dir == ScriptsDir+"/" && appdata.ScriptNameValid(base):
			c.Scripts = append(c.Scripts, Script{Name: base, Data: data})
		default:
			// anything else in the managed folders (a file the app ignores) stays untouched
			c.Unreadable[p] = "not a file the app reads"
		}
	}
	return c, cur, nil
}

// Managed tells the paths the tool plans for.
func Managed(p string) bool {
	return p == CommandsFile || strings.HasPrefix(p, HostsDir+"/") || strings.HasPrefix(p, KeysDir+"/") ||
		strings.HasPrefix(p, ScriptsDir+"/")
}

// Snapshot reads every managed file of a store.
func Snapshot(s store.Store) (map[string][]byte, error) {
	out := map[string][]byte{}
	if b, err := s.Read(CommandsFile); err == nil {
		out[CommandsFile] = b
	} else if !errors.Is(err, store.ErrNotExist) {
		return nil, err
	}
	for _, d := range []string{HostsDir, KeysDir, ScriptsDir} {
		list, err := s.List(d)
		if err != nil {
			return nil, err
		}
		for _, e := range list {
			if e.IsDir {
				continue
			}
			p := d + "/" + e.Name
			b, err := s.Read(p)
			if err != nil {
				return nil, fmt.Errorf("%s: %w", p, err)
			}
			out[p] = b
		}
	}
	return out, nil
}

// Validate is everything the app would refuse, one line each.
func (c *Config) Validate() error {
	var errs []error
	seen := map[string]bool{}
	keys := map[string]bool{}
	for _, k := range c.Keys {
		keys[k.Name] = true
	}
	if len(c.Hosts) > appdata.HostsMax {
		errs = append(errs, fmt.Errorf("at most %d hosts", appdata.HostsMax))
	}
	for _, h := range c.Hosts {
		if seen[strings.ToLower(h.Alias)] { // FAT file names ignore case
			errs = append(errs, fmt.Errorf("%s: alias used twice", h.Alias))
		}
		seen[strings.ToLower(h.Alias)] = true
		if err := h.Validate(); err != nil {
			errs = append(errs, fmt.Errorf("%s: %w", h.Alias, err))
		}
		if h.Auth == "key" && !keys[h.Key] {
			errs = append(errs, fmt.Errorf("%s: key %q is not in keys/", h.Alias, h.Key))
		}
	}
	if len(c.Commands) > appdata.GlobalCmdsMax {
		errs = append(errs, fmt.Errorf("at most %d global commands", appdata.GlobalCmdsMax))
	}
	if len(c.Keys) > appdata.KeysMax {
		errs = append(errs, fmt.Errorf("at most %d keys", appdata.KeysMax))
	}
	for _, k := range c.Keys {
		if !appdata.KeyNameValid(k.Name) {
			errs = append(errs, fmt.Errorf("key %q: not a valid file name", k.Name))
		}
	}
	if len(c.Scripts) > appdata.ScriptsMax {
		errs = append(errs, fmt.Errorf("at most %d scripts", appdata.ScriptsMax))
	}
	return errors.Join(errs...)
}

// Files is the managed part of the folder as the config describes it. Unreadable files are
// kept as they were (from `cur`).
func (c *Config) Files(cur map[string][]byte) (map[string][]byte, error) {
	if err := c.Validate(); err != nil {
		return nil, err
	}
	out := map[string][]byte{}
	for p := range c.Unreadable {
		if b, ok := cur[p]; ok {
			out[p] = b
		}
	}
	for _, h := range c.Hosts {
		t, err := h.Text()
		if err != nil {
			return nil, err
		}
		out[HostsDir+"/"+h.Alias+".conf"] = []byte(t)
	}
	if len(c.Commands) > 0 || cur[CommandsFile] != nil {
		t, err := appdata.CommandsText(c.Commands)
		if err != nil {
			return nil, err
		}
		out[CommandsFile] = []byte(t)
	}
	for _, k := range c.Keys {
		priv := KeysDir + "/" + k.Name
		switch {
		case !k.PublicOnly():
			out[priv] = k.Private
		case cur[priv] != nil:
			out[priv] = cur[priv] // a private file only the target has: never touched
		}
		if k.Pub != nil {
			out[priv+".pub"] = k.Pub
		}
	}
	for _, s := range c.Scripts {
		out[ScriptsDir+"/"+s.Name] = s.Data
	}
	return out, nil
}

// Change kinds.
const (
	Add    = "add"
	Modify = "modify"
	Delete = "delete"
)

// Change is one file to write or delete.
type Change struct {
	Path, Kind string
	Data       []byte
}

// Plan is what turns `cur` into `want`, sorted by path.
func Plan(cur, want map[string][]byte) []Change {
	var out []Change
	for p, b := range want {
		old, ok := cur[p]
		switch {
		case !ok:
			out = append(out, Change{p, Add, b})
		case !bytes.Equal(old, b):
			out = append(out, Change{p, Modify, b})
		}
	}
	for p := range cur {
		if _, ok := want[p]; !ok && Managed(p) {
			out = append(out, Change{p, Delete, nil})
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Path < out[j].Path })
	return out
}

// Apply writes the changes (each write is checked by the store: md5 on a Flipper).
func Apply(s store.Store, plan []Change) error { return ApplyProgress(s, plan, nil) }

// ApplyProgress is Apply calling progress(i, n, path) before each of the n changes.
func ApplyProgress(s store.Store, plan []Change, progress func(i, n int, path string)) error {
	busy, err := s.Busy()
	if err != nil {
		return err
	}
	if busy {
		return errors.New("an app is running on the Flipper: close it (Back to the desktop), then push again")
	}
	for i, ch := range plan {
		if progress != nil {
			progress(i+1, len(plan), ch.Path)
		}
		var err error
		if ch.Kind == Delete {
			err = s.Delete(ch.Path)
		} else {
			err = s.Write(ch.Path, ch.Data)
		}
		if err != nil {
			return fmt.Errorf("%s %s: %w", ch.Kind, ch.Path, err)
		}
	}
	return nil
}
