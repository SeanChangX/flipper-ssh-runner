// SPDX-License-Identifier: GPL-3.0-only
package appdata

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"strings"
)

// KeyInfo is what a key file is, as the app's Keys list shows it (proto/src/keyfmt.c).
type KeyInfo struct {
	Format    string // "OpenSSH", "PEM", "PKCS#8"
	Type      string // "Ed25519", "ECDSA", "RSA", or "" when the file does not say
	Encrypted bool
	Problem   string // why the app cannot use it, or ""
}

// Fingerprint is "SHA256:<base64>" of a public key blob, as ssh-keygen -l prints it.
func Fingerprint(blob []byte) string {
	sum := sha256.Sum256(blob)
	return "SHA256:" + base64.RawStdEncoding.EncodeToString(sum[:])
}

func keyTypeName(t string) string {
	switch {
	case t == "ssh-ed25519":
		return "Ed25519"
	case strings.HasPrefix(t, "ecdsa-sha2-"):
		return "ECDSA"
	case t == "ssh-rsa":
		return "RSA"
	case t == "ssh-dss":
		return "DSA"
	}
	return ""
}

// sshString reads one uint32-length string of the SSH wire format.
func sshString(b []byte) (s, rest []byte, ok bool) {
	if len(b) < 4 {
		return nil, nil, false
	}
	n := binary.BigEndian.Uint32(b)
	if uint64(len(b)-4) < uint64(n) {
		return nil, nil, false
	}
	return b[4 : 4+n], b[4+n:], true
}

func pemBody(text, label string) []byte {
	begin, end := "-----BEGIN "+label+"-----", "-----END "+label+"-----"
	i := strings.Index(text, begin)
	j := strings.Index(text, end)
	if i < 0 || j < i {
		return nil
	}
	var b strings.Builder
	for _, l := range strings.Split(text[i+len(begin):j], "\n") {
		if l = strings.TrimSpace(l); l != "" && !strings.Contains(l, ":") {
			b.WriteString(l)
		}
	}
	out, err := base64.StdEncoding.DecodeString(b.String())
	if err != nil {
		return nil
	}
	return out
}

// OpenSSHPublic is the public key blob inside an OpenSSH private key file (readable without
// the passphrase), or nil.
func OpenSSHPublic(data []byte) []byte {
	body := pemBody(string(data), "OPENSSH PRIVATE KEY")
	magic := []byte("openssh-key-v1\x00")
	if !bytes.HasPrefix(body, magic) {
		return nil
	}
	b := body[len(magic):]
	var ok bool
	for i := 0; i < 3; i++ { // cipher, kdf, kdf options
		if _, b, ok = sshString(b); !ok {
			return nil
		}
	}
	if len(b) < 4 || binary.BigEndian.Uint32(b) < 1 {
		return nil
	}
	pub, _, ok := sshString(b[4:])
	if !ok {
		return nil
	}
	return pub
}

// InspectKey tells what a private key file is and whether the app can use it.
func InspectKey(data []byte) KeyInfo {
	text := string(data)
	switch {
	case len(data) == 0:
		return KeyInfo{Problem: "Empty file"}
	case len(data) > KeyFileMax:
		return KeyInfo{Problem: "Too big for a key file"}
	case strings.Contains(text, "PuTTY-User-Key-File"):
		return KeyInfo{Problem: "PuTTY key: export it as OpenSSH first"}
	case strings.Contains(text, "-----BEGIN OPENSSH PRIVATE KEY-----"):
		info := KeyInfo{Format: "OpenSSH"}
		body := pemBody(text, "OPENSSH PRIVATE KEY")
		magic := []byte("openssh-key-v1\x00")
		if !bytes.HasPrefix(body, magic) {
			info.Problem = "Damaged OpenSSH key file"
			return info
		}
		if cipher, _, ok := sshString(body[len(magic):]); ok {
			info.Encrypted = string(cipher) != "none"
		}
		if pub := OpenSSHPublic(data); pub != nil {
			if t, _, ok := sshString(pub); ok {
				info.Type = keyTypeName(string(t))
			}
		}
		switch info.Type {
		case "":
			info.Problem = "Damaged OpenSSH key file"
		case "DSA":
			info.Problem = "DSA keys are not supported"
		}
		return info
	case strings.Contains(text, "-----BEGIN RSA PRIVATE KEY-----"):
		return KeyInfo{Format: "PEM", Type: "RSA", Encrypted: strings.Contains(text, "ENCRYPTED")}
	case strings.Contains(text, "-----BEGIN EC PRIVATE KEY-----"):
		return KeyInfo{Format: "PEM", Type: "ECDSA", Encrypted: strings.Contains(text, "ENCRYPTED")}
	case strings.Contains(text, "-----BEGIN DSA PRIVATE KEY-----"):
		return KeyInfo{Format: "PEM", Problem: "DSA keys are not supported"}
	case strings.Contains(text, "-----BEGIN ENCRYPTED PRIVATE KEY-----"):
		return KeyInfo{Format: "PKCS#8", Encrypted: true}
	case strings.Contains(text, "-----BEGIN PRIVATE KEY-----"):
		return KeyInfo{Format: "PKCS#8"}
	case strings.HasPrefix(strings.TrimSpace(text), "ssh-") || strings.HasPrefix(strings.TrimSpace(text), "ecdsa-") ||
		strings.Contains(text, "PUBLIC KEY"):
		return KeyInfo{Problem: "This is a public key; copy the private key file too"}
	}
	return KeyInfo{Problem: "Not a private key file"}
}

// InspectPublic tells what a .pub file is, for a key whose private file is not at hand.
func InspectPublic(data []byte) KeyInfo {
	f := strings.Fields(string(data))
	if len(f) < 2 || PublicLine(string(data)) == nil {
		return KeyInfo{Format: "public key", Problem: "Not a public key line"}
	}
	info := KeyInfo{Format: "public key", Type: keyTypeName(f[0])}
	if info.Type == "DSA" {
		info.Problem = "DSA keys are not supported"
	}
	return info
}

// PublicLine parses an OpenSSH public key line ("type base64 [comment]") into its blob.
func PublicLine(line string) []byte {
	f := strings.Fields(line)
	if len(f) < 2 {
		return nil
	}
	b, err := base64.StdEncoding.DecodeString(f[1])
	if err != nil {
		return nil
	}
	return b
}
