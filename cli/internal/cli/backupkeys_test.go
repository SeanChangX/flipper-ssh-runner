// SPDX-License-Identifier: GPL-3.0-only
package cli

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/conf"
)

func TestBackupIncludesPrivateKeysByDefault(t *testing.T) {
	env(t)
	src, bdir := keyCard(t), t.TempDir()
	out, err := runCLI(t, "--dir", src, "--backup-dir", bdir, "backup")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out, "Includes 1 private key; stored only on this computer, readable by you only.") {
		t.Fatalf("%s", out)
	}
	list, _ := conf.ListBackups(bdir)
	if len(list) != 1 || !list[0].Keys {
		t.Fatalf("%+v", list)
	}
	key := filepath.Join(list[0].Dir, "keys", "work")
	st, err := os.Stat(key)
	if err != nil {
		t.Fatalf("the private key is not in the backup: %v", err)
	}
	if runtime.GOOS != "windows" && st.Mode().Perm() != 0o600 {
		t.Fatalf("%s: %v, want owner-only", key, st.Mode().Perm())
	}

	// --no-keys leaves them out and says what that means; --include-keys is still accepted
	out, err = runCLI(t, "--dir", src, "--backup-dir", bdir, "--no-keys", "backup")
	if err != nil || !strings.Contains(out, "Private keys left out (--no-keys)") {
		t.Fatalf("%v\n%s", err, out)
	}
	if _, err := runCLI(t, "--dir", src, "--backup-dir", bdir, "--include-keys", "backup"); err != nil {
		t.Fatal(err)
	}
	list, _ = conf.ListBackups(bdir)
	keys := 0
	for _, b := range list {
		if b.Keys {
			keys++
		}
	}
	if len(list) != 3 || keys != 2 {
		t.Fatalf("%d backups, %d with keys", len(list), keys)
	}
}

func TestBackupBeforePushHoldsTheKeys(t *testing.T) {
	env(t)
	target, bdir := keyCard(t), t.TempDir()
	src := card(t) // changes only the commands: the backup before still holds the keys
	os.WriteFile(filepath.Join(src, "commands.conf"), []byte("Filetype: SSH Runner Commands\nVersion: 1\nCmd: whoami\n"), 0o600)
	if _, err := runCLI(t, "--dir", target, "--backup-dir", bdir, "--yes", "push", src); err != nil {
		t.Fatal(err)
	}
	list, _ := conf.ListBackups(bdir)
	if len(list) != 1 || !list[0].Keys {
		t.Fatalf("%+v", list)
	}
	if _, err := os.Stat(filepath.Join(list[0].Dir, "keys", "work")); err != nil {
		t.Fatal("the backup before the push cannot bring the key back")
	}
}

func TestPullPointsToBackup(t *testing.T) {
	env(t)
	out, err := runCLI(t, "--dir", keyCard(t), "pull", filepath.Join(t.TempDir(), "edit"))
	if err != nil || !strings.Contains(out, "To back up everything, use `sshrunner backup`.") {
		t.Fatalf("%v\n%s", err, out)
	}
}

func TestPullFromAFolderSaysNotCopied(t *testing.T) {
	env(t)
	out, err := runCLI(t, "--dir", keyCard(t), "pull", filepath.Join(t.TempDir(), "edit"))
	if err != nil || !strings.Contains(out, "1 private key not copied (add --include-keys") || strings.Contains(out, "Flipper") {
		t.Fatalf("%v\n%s", err, out)
	}
}

func TestPathsShownCleaned(t *testing.T) {
	env(t)
	bdir := t.TempDir()
	messy := bdir + string(filepath.Separator) + "." + string(filepath.Separator)
	out, err := runCLI(t, "--dir", keyCard(t), "--backup-dir", messy, "backups", "list")
	if err != nil || !strings.HasPrefix(strings.TrimSpace(out), bdir) || strings.Contains(out, messy) {
		t.Fatalf("%v\n%s", err, out)
	}
}
