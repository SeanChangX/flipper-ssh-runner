// SPDX-License-Identifier: GPL-3.0-only
package cli

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/json"
	"encoding/pem"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"golang.org/x/crypto/ssh"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/appdata"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/conf"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/store"
)

// keyCard: a data folder (standing in for the Flipper) with a key and a host that uses it.
func keyCard(t *testing.T) string {
	t.Helper()
	d := card(t)
	pub, priv, _ := ed25519.GenerateKey(rand.Reader)
	blk, err := ssh.MarshalPrivateKey(priv, "example")
	if err != nil {
		t.Fatal(err)
	}
	sp, _ := ssh.NewPublicKey(pub)
	os.MkdirAll(filepath.Join(d, "keys"), 0o700)
	os.WriteFile(filepath.Join(d, "keys", "work"), pem.EncodeToMemory(blk), 0o600)
	os.WriteFile(filepath.Join(d, "keys", "work.pub"), ssh.MarshalAuthorizedKey(sp), 0o644)
	h := appdata.NewHost()
	h.Alias, h.User, h.Host, h.Auth, h.Key = "pi", "pi", "192.168.1.20", "key", "work"
	text, _ := h.Text()
	os.WriteFile(filepath.Join(d, "hosts", "pi.conf"), []byte(text), 0o600)
	return d
}

func TestPullWithoutKeysRoundTrips(t *testing.T) {
	env(t)
	src := keyCard(t)
	dst := filepath.Join(t.TempDir(), "pulled")
	if _, err := runCLI(t, "--dir", src, "pull", dst); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dst, "keys", "work")); !os.IsNotExist(err) {
		t.Fatalf("the private key was pulled: %v", err)
	}

	// status of the pulled folder: no warnings, the key counted as public only
	out, err := runCLI(t, "--dir", dst, "status")
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(out, "!") || !strings.Contains(out, "keys      1") || !strings.Contains(out, "1 public only") {
		t.Fatalf("status:\n%s", out)
	}
	js, _ := runCLI(t, "--dir", dst, "--json", "status")
	var st map[string]any
	json.Unmarshal([]byte(js), &st)
	if st["keys"].(float64) != 1 || st["keys_public_only"].(float64) != 1 || len(st["unreadable"].(map[string]any)) != 0 {
		t.Fatalf("%v", st)
	}

	// the editor's view: valid, nothing to push
	s, _ := store.OpenDir(dst)
	c, cur, _ := conf.Load(s)
	if len(c.Unreadable) != 0 || len(c.Keys) != 1 || !c.Keys[0].PublicOnly() || c.Keys[0].Fingerprint() == "" {
		t.Fatalf("%+v %v", c.Keys, c.Unreadable)
	}
	want, err := c.Files(cur)
	if err != nil {
		t.Fatalf("the pulled folder is not valid: %v", err)
	}
	if plan := conf.Plan(cur, want); len(plan) != 0 {
		t.Fatalf("plan %v", plan)
	}

	// pushing it back changes nothing, with --delete too; the private key stays
	for _, args := range [][]string{{"push", dst}, {"--delete", "push", dst}} {
		out, err := runCLI(t, append([]string{"--dir", src, "--backup-dir", t.TempDir(), "--yes"}, args...)...)
		if err != nil || !strings.Contains(out, "Nothing to change.") {
			t.Fatalf("%v: %v\n%s", args, err, out)
		}
	}
	if _, err := os.Stat(filepath.Join(src, "keys", "work")); err != nil {
		t.Fatalf("the private key is gone: %v", err)
	}
}

func TestPushRefusesAHostWhoseKeyIsNowhere(t *testing.T) {
	env(t)
	target := card(t) // no keys at all
	src := filepath.Join(t.TempDir(), "edit")
	os.MkdirAll(filepath.Join(src, "hosts"), 0o700)
	os.MkdirAll(filepath.Join(src, "keys"), 0o700)
	pub, _, _ := ed25519.GenerateKey(rand.Reader)
	sp, _ := ssh.NewPublicKey(pub)
	os.WriteFile(filepath.Join(src, "keys", "work.pub"), ssh.MarshalAuthorizedKey(sp), 0o644)
	h := appdata.NewHost()
	h.Alias, h.User, h.Host, h.Auth, h.Key = "pi", "pi", "192.168.1.20", "key", "work"
	text, _ := h.Text()
	os.WriteFile(filepath.Join(src, "hosts", "pi.conf"), []byte(text), 0o600)
	_, err := runCLI(t, "--dir", target, "--backup-dir", t.TempDir(), "--yes", "push", src)
	if err == nil || !strings.Contains(err.Error(), `key "work" is neither in the folder nor on the target`) {
		t.Fatalf("%v", err)
	}
}
