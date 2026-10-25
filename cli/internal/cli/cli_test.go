// SPDX-License-Identifier: GPL-3.0-only
package cli

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/appdata"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/conf"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/store"
)

// env isolates a test from the user's config and backups.
func env(t *testing.T) string {
	t.Helper()
	d := t.TempDir()
	t.Setenv("SSHRUNNER_CONFIG", filepath.Join(d, "config.toml"))
	t.Setenv("SSHRUNNER_BACKUP_DIR", "")
	t.Setenv("SSHRUNNER_KEEP_BACKUPS", "")
	return d
}

// card: a data folder with one host and two global commands.
func card(t *testing.T) string {
	t.Helper()
	d := t.TempDir()
	h := appdata.NewHost()
	h.Alias, h.User, h.Host, h.Cmds = "nas", "admin", "192.168.1.10", []string{"uptime"}
	text, _ := h.Text()
	os.MkdirAll(filepath.Join(d, "hosts"), 0o700)
	os.WriteFile(filepath.Join(d, "hosts", "nas.conf"), []byte(text), 0o600)
	cmds, _ := appdata.CommandsText([]string{"uptime", "df -h"})
	os.WriteFile(filepath.Join(d, "commands.conf"), []byte(cmds), 0o600)
	return d
}

func runCLI(t *testing.T, args ...string) (string, error) {
	t.Helper()
	var out, errb bytes.Buffer
	o := &options{out: &out, err: &errb, in: strings.NewReader("")}
	err := run(o, args)
	return out.String(), err
}

func keys(m map[string]any) []string {
	var out []string
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

func TestJSONStatusAndHosts(t *testing.T) {
	env(t)
	d := card(t)
	out, err := runCLI(t, "--dir", d, "--json", "status")
	if err != nil {
		t.Fatal(err)
	}
	var st map[string]any
	if err := json.Unmarshal([]byte(out), &st); err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	if want := []string{"commands", "hosts", "keys", "keys_public_only", "scripts", "source", "unreadable"}; !reflect.DeepEqual(keys(st), want) {
		t.Fatalf("status fields %v", keys(st))
	}
	if st["hosts"].(float64) != 1 || st["commands"].(float64) != 2 {
		t.Fatalf("%v", st)
	}
	out, err = runCLI(t, "--dir", d, "hosts", "--json")
	if err != nil {
		t.Fatal(err)
	}
	var hs []map[string]any
	if err := json.Unmarshal([]byte(out), &hs); err != nil || len(hs) != 1 {
		t.Fatalf("%v\n%s", err, out)
	}
	want := []string{"alias", "auth", "commands", "host", "host_key_policy", "key", "port", "save_password", "transport", "user"}
	if !reflect.DeepEqual(keys(hs[0]), want) {
		t.Fatalf("host fields %v", keys(hs[0]))
	}
	if hs[0]["alias"] != "nas" || hs[0]["port"].(float64) != 22 {
		t.Fatalf("%v", hs[0])
	}
}

func TestJSONBackupsList(t *testing.T) {
	env(t)
	d, bdir := card(t), t.TempDir()
	out, _ := runCLI(t, "--backup-dir", bdir, "backups", "list", "--json")
	if strings.TrimSpace(out) != `{
  "dir": "`+strings.ReplaceAll(bdir, `\`, `\\`)+`",
  "backups": []
}` {
		t.Fatalf("empty list:\n%s", out)
	}
	if _, err := runCLI(t, "--dir", d, "--backup-dir", bdir, "backup"); err != nil {
		t.Fatal(err)
	}
	out, _ = runCLI(t, "--backup-dir", bdir, "backups", "list", "--json")
	var bl struct {
		Dir     string           `json:"dir"`
		Backups []map[string]any `json:"backups"`
	}
	if err := json.Unmarshal([]byte(out), &bl); err != nil || len(bl.Backups) != 1 {
		t.Fatalf("%v\n%s", err, out)
	}
	if want := []string{"created", "files", "hosts", "id", "keys", "size", "source"}; !reflect.DeepEqual(keys(bl.Backups[0]), want) {
		t.Fatalf("backup fields %v", keys(bl.Backups[0]))
	}
	ids, _ := runCLI(t, "--backup-dir", bdir, "backups", "ids")
	if strings.TrimSpace(ids) != bl.Backups[0]["id"] {
		t.Fatalf("ids %q", ids)
	}
}

func TestConfigPrecedence(t *testing.T) {
	d := env(t)
	fromFile, fromEnv, fromFlag := filepath.Join(d, "file"), filepath.Join(d, "env"), filepath.Join(d, "flag")
	os.WriteFile(filepath.Join(d, "config.toml"), []byte("# defaults\nbackup_dir = \""+filepath.ToSlash(fromFile)+"\"\nkeep_backups = 3 # three\nmouse = false\nport = 'COM9'\n"), 0o600)
	c, err := readConfig(filepath.Join(d, "config.toml"))
	if err != nil {
		t.Fatal(err)
	}
	def, _ := resolveDefaults(c)
	if def.backupDir != filepath.ToSlash(fromFile) || def.keepBackups != 3 || def.mouse || def.port != "COM9" {
		t.Fatalf("file: %+v", def)
	}
	t.Setenv("SSHRUNNER_BACKUP_DIR", fromEnv)
	t.Setenv("SSHRUNNER_KEEP_BACKUPS", "5")
	def, _ = resolveDefaults(c)
	if def.backupDir != fromEnv || def.keepBackups != 5 {
		t.Fatalf("env: %+v", def)
	}
	out, _ := runCLI(t, "--backup-dir", fromFlag, "backups", "list", "--json")
	if !strings.Contains(out, `"dir": "`+strings.ReplaceAll(fromFlag, `\`, `\\`)+`"`) {
		t.Fatalf("flag did not win:\n%s", out)
	}
	t.Setenv("SSHRUNNER_BACKUP_DIR", "")
	t.Setenv("SSHRUNNER_KEEP_BACKUPS", "")
	if def, _ := resolveDefaults(fileConfig{}); def.keepBackups != conf.DefaultKeepBackups || !def.mouse {
		t.Fatalf("built-in: %+v", def)
	}
}

func TestConfigRefusesTypos(t *testing.T) {
	d := t.TempDir()
	for text, want := range map[string]string{
		"keep_backup = 3\n":     `unknown key "keep_backup"`,
		"keep_backups = -1\n":   "keep_backups",
		"mouse = yes\n":         "mouse: true or false",
		"backup_dir = /tmp/x\n": "quotes",
		"just words\n":          "key = value",
	} {
		p := filepath.Join(d, "c.toml")
		os.WriteFile(p, []byte(text), 0o600)
		if _, err := readConfig(p); err == nil || !strings.Contains(err.Error(), want) || !strings.Contains(err.Error(), "line 1") {
			t.Errorf("%q: %v", text, err)
		}
	}
	if c, err := readConfig(filepath.Join(d, "missing.toml")); err != nil || c.KeepBackups != nil {
		t.Fatal("a missing file is not an empty config")
	}
}

func TestCompletionScripts(t *testing.T) {
	env(t)
	for _, sh := range []string{"bash", "zsh", "fish", "powershell"} {
		out, err := runCLI(t, "completion", sh)
		if err != nil {
			t.Fatal(err)
		}
		for _, want := range []string{"backups ids", "restore", "keep-backups", "export"} {
			if !strings.Contains(out, want) {
				t.Errorf("%s: no %q", sh, want)
			}
		}
		if strings.Contains(out, "%!") {
			t.Errorf("%s: formatting error in the script", sh)
		}
	}
	if _, err := runCLI(t, "completion", "tcsh"); err == nil {
		t.Fatal("unknown shell accepted")
	}
}

func TestPushRefusesOverLongCommand(t *testing.T) {
	env(t)
	d, src := card(t), card(t)
	long := strings.Repeat("x", appdata.CmdMax+1)
	text, _ := os.ReadFile(filepath.Join(src, "hosts", "nas.conf"))
	os.WriteFile(filepath.Join(src, "hosts", "nas.conf"), append(text, []byte("Cmd: "+long+"\n")...), 0o600)
	_, err := runCLI(t, "--dir", d, "--backup-dir", t.TempDir(), "--yes", "push", src)
	if err == nil || !strings.Contains(err.Error(), "hosts/nas.conf") || !strings.Contains(err.Error(), "128 characters, at most 127") {
		t.Fatalf("%v", err)
	}
	if b, _ := os.ReadFile(filepath.Join(d, "hosts", "nas.conf")); strings.Contains(string(b), long) {
		t.Fatal("written anyway")
	}
}

func TestPruneAfterPush(t *testing.T) {
	env(t)
	d, src, bdir := card(t), card(t), t.TempDir()
	for i := 0; i < 4; i++ {
		os.WriteFile(filepath.Join(src, "commands.conf"), []byte("Filetype: SSH Runner Commands\nVersion: 1\nCmd: c"+strings.Repeat("x", i)+"\n"), 0o600)
		if _, err := runCLI(t, "--dir", d, "--backup-dir", bdir, "--keep-backups", "2", "--yes", "push", src); err != nil {
			t.Fatal(err)
		}
	}
	if list, _ := conf.ListBackups(bdir); len(list) != 2 {
		t.Fatalf("%d backups kept", len(list))
	}
	s, _ := store.OpenDir(d)
	if c, _, _ := conf.Load(s); len(c.Commands) != 1 || c.Commands[0] != "cxxx" {
		t.Fatalf("%v", c.Commands)
	}
}

func TestProgressQuietWhenNotATerminal(t *testing.T) {
	var b bytes.Buffer
	o := &options{out: &b, err: &b}
	p := o.progress("Writing")
	p.step(1, 2, "hosts/a.conf")
	p.done()
	if b.Len() != 0 {
		t.Fatalf("%q", b.String())
	}
	p = &progress{w: &b, what: "Writing", on: true}
	p.step(2, 2, "hosts/a.conf")
	if !strings.Contains(b.String(), "Writing [2/2] hosts/a.conf") {
		t.Fatalf("%q", b.String())
	}
}
