// SPDX-License-Identifier: GPL-3.0-only
package conf

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/appdata"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/flipper"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/store"
)

const keyText = "-----BEGIN OPENSSH PRIVATE KEY-----\nAAAA\n-----END OPENSSH PRIVATE KEY-----\n"

// stores: a local folder and the fake Flipper, seeded with the same files
func stores(t *testing.T, files map[string]string) map[string]store.Store {
	t.Helper()
	dir := t.TempDir()
	f := flipper.NewFake()
	for _, d := range []string{"/ext/apps_data", store.AppDataDir} {
		f.Dirs[d] = true
	}
	for p, b := range files {
		full := filepath.Join(dir, filepath.FromSlash(p))
		os.MkdirAll(filepath.Dir(full), 0o700)
		os.WriteFile(full, []byte(b), 0o600)
		fp := store.AppDataDir + "/" + p
		for d := filepath.Dir(fp); d != "/ext"; d = filepath.Dir(d) {
			f.Dirs[d] = true
		}
		f.Files[fp] = []byte(b)
	}
	ds, err := store.OpenDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	c, err := flipper.New(f)
	if err != nil {
		t.Fatal(err)
	}
	return map[string]store.Store{"folder": ds, "flipper": store.NewFlipper(c, "fake")}
}

func seed(t *testing.T) map[string]string {
	h := appdata.NewHost()
	h.Alias, h.User, h.Host = "nas", "admin", "192.168.1.10"
	text, err := h.Text()
	if err != nil {
		t.Fatal(err)
	}
	return map[string]string{
		"hosts/nas.conf":   text,
		"hosts/bad.conf":   "garbage",
		"keys/work":        keyText,
		"keys/lonely.pub":  "ssh-ed25519 AAAA x\n",
		"scripts/a.sh":     "#!/bin/sh\nuptime\n",
		"settings.conf":    "Filetype: SSH Runner Settings\nVersion: 1\n",
		"recent.conf":      "keep me",
		"logs/nas/x.log":   "output",
		"scripts/notes.md": "not a script",
	}
}

func TestEditPushOnBothStores(t *testing.T) {
	for name, s := range stores(t, seed(t)) {
		t.Run(name, func(t *testing.T) {
			c, cur, err := Load(s)
			if err != nil {
				t.Fatal(err)
			}
			if len(c.Hosts) != 1 || len(c.Keys) != 2 || len(c.Scripts) != 1 || len(c.Unreadable) != 2 {
				t.Fatalf("loaded %d hosts %d keys %d scripts, unreadable %v", len(c.Hosts), len(c.Keys), len(c.Scripts), c.Unreadable)
			}
			// nothing edited: nothing to do
			want, err := c.Files(cur)
			if err != nil {
				t.Fatal(err)
			}
			if plan := Plan(cur, want); len(plan) != 0 {
				t.Fatalf("no edits, plan %v", plan)
			}
			// edit: one host changed, one added, a command, the script removed
			c.Hosts[0].Port = 2222
			h := appdata.NewHost()
			h.Alias, h.User, h.Host, h.Auth, h.Key = "pi", "pi", "pi.local", "key", "work"
			c.Hosts = append(c.Hosts, h)
			c.Commands = []string{"uptime"}
			c.Scripts = nil
			want, err = c.Files(cur)
			if err != nil {
				t.Fatal(err)
			}
			plan := Plan(cur, want)
			kinds := map[string]string{}
			for _, ch := range plan {
				kinds[ch.Path] = ch.Kind
			}
			exp := map[string]string{"hosts/nas.conf": Modify, "hosts/pi.conf": Add, "commands.conf": Add, "scripts/a.sh": Delete}
			if len(kinds) != len(exp) {
				t.Fatalf("plan %v", kinds)
			}
			for p, k := range exp {
				if kinds[p] != k {
					t.Errorf("%s: %s, want %s", p, kinds[p], k)
				}
			}
			if err := Apply(s, plan); err != nil {
				t.Fatal(err)
			}
			// written as planned; the app's own files and the files it ignores untouched
			again, cur2, err := Load(s)
			if err != nil {
				t.Fatal(err)
			}
			if len(again.Hosts) != 2 || again.Hosts[0].Port != 2222 || len(again.Commands) != 1 {
				t.Fatalf("after push: %+v", again.Hosts)
			}
			for _, p := range []string{"hosts/bad.conf", "keys/lonely.pub", "scripts/notes.md"} {
				if _, ok := cur2[p]; !ok {
					t.Errorf("%s was removed", p)
				}
			}
			for _, p := range []string{"settings.conf", "recent.conf", "logs/nas/x.log"} {
				if b, err := s.Read(p); err != nil || len(b) == 0 {
					t.Errorf("%s: %v", p, err)
				}
			}
		})
	}
}

func TestValidateRefusesWhatTheAppWould(t *testing.T) {
	c := &Config{}
	h := appdata.NewHost()
	h.Alias, h.User, h.Host, h.Auth, h.Key = "nas", "u", "h", "key", "missing"
	c.Hosts = []appdata.Host{h, h}
	err := c.Validate()
	if err == nil || !strings.Contains(err.Error(), "twice") || !strings.Contains(err.Error(), "not in keys/") {
		t.Fatalf("%v", err)
	}
	if _, err := c.Files(nil); err == nil {
		t.Fatal("invalid config produced files")
	}
}

func TestBackupRestore(t *testing.T) {
	for name, s := range stores(t, seed(t)) {
		t.Run(name, func(t *testing.T) {
			parent := t.TempDir()
			dir, m, err := Backup(s, parent, false)
			if err != nil {
				t.Fatal(err)
			}
			if _, ok := m.Files["keys/work"]; ok {
				t.Fatal("private key in a backup without --include-keys")
			}
			if _, err := os.Stat(filepath.Join(dir, "keys", "work")); err == nil {
				t.Fatal("private key file written")
			}
			if _, ok := m.Files["settings.conf"]; !ok {
				t.Fatal("settings not backed up")
			}
			// change the store, then restore: hosts come back, the key stays (not in the backup)
			c, cur, _ := Load(s)
			c.Hosts = nil
			want, _ := c.Files(cur)
			if err := Apply(s, Plan(cur, want)); err != nil {
				t.Fatal(err)
			}
			s.Write("settings.conf", []byte("changed"))
			bm, files, err := ReadBackup(dir)
			if err != nil {
				t.Fatal(err)
			}
			plan, err := RestorePlan(s, bm, files)
			if err != nil {
				t.Fatal(err)
			}
			if err := Apply(s, plan); err != nil {
				t.Fatal(err)
			}
			after, _, _ := Load(s)
			if len(after.Hosts) != 1 || len(after.Keys) != 2 {
				t.Fatalf("after restore: %d hosts %d keys", len(after.Hosts), len(after.Keys))
			}
			if b, _ := s.Read("settings.conf"); !strings.HasPrefix(string(b), "Filetype") {
				t.Fatalf("settings not restored: %q", b)
			}
			// a tampered backup is refused
			os.WriteFile(filepath.Join(dir, "recent.conf"), []byte("tampered"), 0o600)
			if _, _, err := ReadBackup(dir); err == nil {
				t.Fatal("tampered backup accepted")
			}
		})
	}
}

func TestApplyRefusesWhileTheAppRuns(t *testing.T) {
	f := flipper.NewFake()
	f.AppBusy = true
	c, _ := flipper.New(f)
	s := store.NewFlipper(c, "fake")
	if err := Apply(s, []Change{{"hosts/x.conf", Add, []byte("x")}}); err == nil || !strings.Contains(err.Error(), "running") {
		t.Fatalf("%v", err)
	}
}
