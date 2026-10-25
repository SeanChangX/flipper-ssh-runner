// SPDX-License-Identifier: GPL-3.0-only
package appdata

import (
	"crypto/ecdsa"
	"crypto/ed25519"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"encoding/pem"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"golang.org/x/crypto/ssh"
)

// The same files flipper-app/tests/test_profile.c reads and writes back byte for byte.
const golden = "../../../flipper-app/tests/golden"

func readGolden(t *testing.T, name string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(golden, name))
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func TestGoldenHostsRoundTrip(t *testing.T) {
	for _, name := range []string{"host_key.conf", "host_password.conf", "host_default.conf"} {
		text := readGolden(t, name)
		h, err := ParseHost(text, "")
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		out, err := h.Text()
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if out != text {
			t.Errorf("%s: written back differs:\n%s\nwant:\n%s", name, out, text)
		}
	}
	h, _ := ParseHost(readGolden(t, "host_key.conf"), "")
	want := Host{Alias: "nas", User: "admin", Host: "192.168.1.10", Port: 2222, Auth: "key", Key: "id_ed25519",
		Transport: "lan", Policy: "strict", Cmds: []string{"uptime", "df -h", "sudo systemctl restart nginx"}}
	if h.Alias != want.Alias || h.Port != want.Port || h.Key != want.Key || h.Policy != want.Policy ||
		len(h.Cmds) != 3 || h.Cmds[2] != want.Cmds[2] || h.Remember {
		t.Errorf("parsed %+v", h)
	}
}

func TestGoldenCommandsRoundTrip(t *testing.T) {
	text := readGolden(t, "commands.conf")
	cmds, err := ParseCommands(text)
	if err != nil || len(cmds) != 3 {
		t.Fatalf("%v %v", cmds, err)
	}
	out, err := CommandsText(cmds)
	if err != nil || out != text {
		t.Errorf("written back differs:\n%s\n%v", out, err)
	}
}

func TestParseHostAsTheApp(t *testing.T) {
	base := "Filetype: SSH Runner Host\nVersion: 1\nAlias: a\nUser: u\nHost: h\n"
	h, err := ParseHost(base, "")
	if err != nil || h.Port != 22 || h.Auth != "password" || h.Transport != "auto" || h.Policy != "prompt" {
		t.Fatalf("defaults: %+v %v", h, err)
	}
	// unknown enum values fall back to the defaults; an old "Auth: key" without Key is "default"
	h, _ = ParseHost(base+"Auth: key\nTransport: usb\nHostkey_policy: maybe\n", "")
	if h.Auth != "default" || h.Transport != "auto" || h.Policy != "prompt" {
		t.Errorf("fallbacks: %+v", h)
	}
	// a bad port rejects the file, as the app does
	if _, err := ParseHost(base+"Port: 70000\n", ""); err == nil {
		t.Error("port 70000 accepted")
	}
	if _, err := ParseHost(strings.Replace(base, "Version: 1", "Version: 2", 1), ""); err == nil {
		t.Error("version 2 accepted")
	}
	// the file name is the alias; spaces and \r around values are trimmed; comments skipped
	h, err = ParseHost("# c\r\nFiletype:  SSH Runner Host \r\nVersion: 1\r\nAlias: x\r\nUser: u\r\nHost: h\r\n", "nas")
	if err != nil || h.Alias != "nas" {
		t.Errorf("CRLF/alias: %+v %v", h, err)
	}
	// an empty Cmd line holds nothing; too many or too long commands are refused, never cut
	h, err = ParseHost(base+"Cmd: c\nCmd: \n", "")
	if err != nil || len(h.Cmds) != 1 {
		t.Errorf("%v %v", h.Cmds, err)
	}
	text := base
	for i := 0; i < 10; i++ {
		text += "Cmd: c\n"
	}
	if _, err := ParseHost(text, ""); err == nil || !strings.Contains(err.Error(), "10 commands, at most 8") {
		t.Errorf("%v", err)
	}
	long := strings.Repeat("x", CmdMax+1)
	if _, err := ParseHost(base+"Cmd: ok\nCmd: "+long+"\n", ""); err == nil ||
		!strings.Contains(err.Error(), "command 2: 128 characters, at most 127") {
		t.Errorf("%v", err)
	}
	if _, err := ParseHost(base+"Cmd: "+strings.Repeat("x", CmdMax)+"\n", ""); err != nil {
		t.Errorf("127 characters refused: %v", err)
	}
	if _, err := ParseCommands("Filetype: SSH Runner Commands\nVersion: 1\nCmd: " + long + "\n"); err == nil {
		t.Error("an over-long global command was accepted")
	}
}

func TestValidators(t *testing.T) {
	cases := []struct {
		f    func(string) bool
		s    string
		want bool
	}{
		{AliasValid, "nas", true}, {AliasValid, "web-1.lab", true}, {AliasValid, "-x", false},
		{AliasValid, "x.", false}, {AliasValid, strings.Repeat("a", 21), false}, {AliasValid, "a b", false},
		{UserValid, "root", true}, {UserValid, "svc$", true}, {UserValid, "-u", false}, {UserValid, "a@b", false},
		{HostValid, "192.168.1.10", true}, {HostValid, "fe80::1%eth0", true}, {HostValid, "-h", false},
		{HostValid, ".h", false}, {HostValid, "a b", false}, {HostValid, strings.Repeat("h", 64), false},
		{KeyNameValid, "id_ed25519", true}, {KeyNameValid, "k.pub", false}, {KeyNameValid, ".k", false},
		{CmdValid, "ls -la", true}, {CmdValid, "", false}, {CmdValid, "a\nb", false},
		{ScriptNameValid, "backup.sh", true}, {ScriptNameValid, "a/b.sh", false}, {ScriptNameValid, "x.txt", false},
	}
	for _, c := range cases {
		if got := c.f(c.s); got != c.want {
			t.Errorf("%q: %v, want %v", c.s, got, c.want)
		}
	}
	bad := Host{Alias: "nas", User: "u", Host: "h", Port: 0, Auth: "key", Transport: "auto", Policy: "prompt"}
	if err := bad.Validate(); err == nil || !strings.Contains(err.Error(), "port") || !strings.Contains(err.Error(), "key") {
		t.Errorf("validate: %v", err)
	}
}

func opensshKey(t *testing.T, key any, passphrase string) []byte {
	t.Helper()
	var blk *pem.Block
	var err error
	if passphrase != "" {
		blk, err = ssh.MarshalPrivateKeyWithPassphrase(key, "test", []byte(passphrase))
	} else {
		blk, err = ssh.MarshalPrivateKey(key, "test")
	}
	if err != nil {
		t.Fatal(err)
	}
	return pem.EncodeToMemory(blk)
}

func TestInspectKey(t *testing.T) {
	_, ed, _ := ed25519.GenerateKey(rand.Reader)
	ec, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	rs, _ := rsa.GenerateKey(rand.Reader, 2048)
	cases := []struct {
		name string
		data []byte
		want KeyInfo
	}{
		{"ed25519", opensshKey(t, ed, ""), KeyInfo{Format: "OpenSSH", Type: "Ed25519"}},
		{"ed25519 encrypted", opensshKey(t, ed, "pw"), KeyInfo{Format: "OpenSSH", Type: "Ed25519", Encrypted: true}},
		{"ecdsa", opensshKey(t, ec, ""), KeyInfo{Format: "OpenSSH", Type: "ECDSA"}},
		{"rsa openssh", opensshKey(t, rs, ""), KeyInfo{Format: "OpenSSH", Type: "RSA"}},
		{"rsa pem", pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: []byte{1}}), KeyInfo{Format: "PEM", Type: "RSA"}},
		{"public", []byte("ssh-ed25519 AAAA test\n"), KeyInfo{Problem: "This is a public key; copy the private key file too"}},
		{"putty", []byte("PuTTY-User-Key-File-3: ssh-ed25519\n"), KeyInfo{Problem: "PuTTY key: export it as OpenSSH first"}},
		{"junk", []byte("hello"), KeyInfo{Problem: "Not a private key file"}},
		{"empty", nil, KeyInfo{Problem: "Empty file"}},
	}
	for _, c := range cases {
		if got := InspectKey(c.data); got != c.want {
			t.Errorf("%s: %+v, want %+v", c.name, got, c.want)
		}
	}
	// the fingerprint from the private file's public section matches ssh's
	signer, _ := ssh.NewSignerFromKey(ed)
	want := ssh.FingerprintSHA256(signer.PublicKey())
	if got := Fingerprint(OpenSSHPublic(opensshKey(t, ed, "pw"))); got != want {
		t.Errorf("fingerprint %s, want %s", got, want)
	}
	line := string(ssh.MarshalAuthorizedKey(signer.PublicKey()))
	if got := Fingerprint(PublicLine(line)); got != want {
		t.Errorf(".pub fingerprint %s, want %s", got, want)
	}
}
