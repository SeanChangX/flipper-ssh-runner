// SPDX-License-Identifier: GPL-3.0-only
package tui

import (
	"crypto/ed25519"
	"crypto/rand"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/charmbracelet/x/ansi"
	"golang.org/x/crypto/ssh"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/appdata"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/store"
)

// examplePub: a real public key line, as pull writes keys/<name>.pub.
func examplePub(t *testing.T) []byte {
	t.Helper()
	pub, _, _ := ed25519.GenerateKey(rand.Reader)
	sp, err := ssh.NewPublicKey(pub)
	if err != nil {
		t.Fatal(err)
	}
	return ssh.MarshalAuthorizedKey(sp)
}

// A folder pulled without --include-keys: hosts and .pub files, the private keys on the Flipper.
func pulledFolder(t *testing.T) string {
	t.Helper()
	d := t.TempDir()
	os.MkdirAll(filepath.Join(d, "hosts"), 0o700)
	os.MkdirAll(filepath.Join(d, "keys"), 0o700)
	os.WriteFile(filepath.Join(d, "keys", "work.pub"), examplePub(t), 0o644)
	h := appdata.NewHost()
	h.Alias, h.User, h.Host, h.Auth, h.Key = "pi", "pi", "192.168.1.20", "key", "work"
	text, _ := h.Text()
	os.WriteFile(filepath.Join(d, "hosts", "pi.conf"), []byte(text), 0o600)
	return d
}

func TestPulledFolderIsValid(t *testing.T) {
	d := pulledFolder(t)
	ds, err := store.OpenDir(d)
	if err != nil {
		t.Fatal(err)
	}
	m, _ := setup(t, func(o *Options) { o.Store = ds })
	v := ansi.Strip(m.View())
	if strings.Contains(v, "invalid edits") || strings.Contains(v, "cannot read") || strings.Contains(v, "unpushed") {
		t.Fatalf("a pulled folder looks broken:\n%s", v)
	}
	k := ansi.Strip(keys(t, m, "3").View())
	if !strings.Contains(k, "work") || !strings.Contains(k, "Ed25519") || !strings.Contains(k, "SHA256:") ||
		strings.Contains(k, "! ") || !strings.Contains(k, "public key") || !strings.Contains(k, "pull copies public keys only") {
		t.Fatalf("keys tab:\n%s", k)
	}
}
