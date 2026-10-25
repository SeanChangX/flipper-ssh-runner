// SPDX-License-Identifier: GPL-3.0-only
package conf

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/store"
)

// ManifestFile describes a backup folder.
const ManifestFile = "sshrunner-backup.json"

// Now is the clock a new backup is stamped with (its folder name and creation time); tests set
// a fixed one.
var Now = time.Now

// Manifest is what a backup holds.
type Manifest struct {
	Created     string            `json:"created"`
	Source      string            `json:"source"`
	IncludeKeys bool              `json:"include_keys"`
	Files       map[string]string `json:"files"` // path -> sha256
}

// PrivateKeys: how many private key files the backup holds.
func (m *Manifest) PrivateKeys() int {
	n := 0
	for p := range m.Files {
		if IsPrivateKey(p) {
			n++
		}
	}
	return n
}

// IsPrivateKey: a file in keys/ that is not a .pub.
func IsPrivateKey(p string) bool {
	return IsKeyPath(p) && !strings.HasSuffix(p, ".pub")
}

// IsKeyPath: a file in keys/.
func IsKeyPath(p string) bool { return strings.HasPrefix(p, KeysDir+"/") }

// walk lists every file under rel, recursively.
func walk(s store.Store, rel string) ([]string, error) {
	list, err := s.List(rel)
	if err != nil {
		return nil, err
	}
	var out []string
	for _, e := range list {
		p := path.Join(rel, e.Name)
		if e.IsDir {
			sub, err := walk(s, p)
			if err != nil {
				return nil, err
			}
			out = append(out, sub...)
		} else {
			out = append(out, p)
		}
	}
	return out, nil
}

// Backup copies the whole data folder into a new folder under `parent`, owner-only (files
// 0600, folders 0700). The private keys come too unless includeKeys is false: a key made on the
// Flipper exists nowhere else, so a backup without it cannot bring it back.
func Backup(s store.Store, parent string, includeKeys bool) (string, *Manifest, error) {
	return BackupProgress(s, parent, includeKeys, nil)
}

// BackupProgress is Backup calling progress(i, n, path) before reading each file.
func BackupProgress(s store.Store, parent string, includeKeys bool, progress func(i, n int, path string)) (string, *Manifest, error) {
	files, err := walk(s, ".")
	if err != nil {
		return "", nil, err
	}
	now := Now()
	dir, err := newBackupDir(parent, now)
	if err != nil {
		return "", nil, err
	}
	m := &Manifest{Created: now.Format(time.RFC3339), Source: s.Describe(), IncludeKeys: includeKeys,
		Files: map[string]string{}}
	for i, p := range files {
		p = strings.TrimPrefix(p, "./")
		if progress != nil {
			progress(i+1, len(files), p)
		}
		if IsPrivateKey(p) && !includeKeys {
			continue
		}
		data, err := s.Read(p)
		if err != nil {
			return "", nil, fmt.Errorf("%s: %w", p, err)
		}
		dst := filepath.Join(dir, filepath.FromSlash(p))
		if err := os.MkdirAll(filepath.Dir(dst), 0o700); err != nil {
			return "", nil, err
		}
		if err := os.WriteFile(dst, data, 0o600); err != nil {
			return "", nil, err
		}
		sum := sha256.Sum256(data)
		m.Files[p] = hex.EncodeToString(sum[:])
	}
	j, _ := json.MarshalIndent(m, "", "  ")
	if err := os.WriteFile(filepath.Join(dir, ManifestFile), append(j, '\n'), 0o600); err != nil {
		return "", nil, err
	}
	return dir, m, nil
}

// ReadBackup loads a backup folder and checks every file against its manifest.
func ReadBackup(dir string) (*Manifest, map[string][]byte, error) {
	j, err := os.ReadFile(filepath.Join(dir, ManifestFile))
	if err != nil {
		return nil, nil, fmt.Errorf("not an SSH Runner backup (no %s): %w", ManifestFile, err)
	}
	var m Manifest
	if err := json.Unmarshal(j, &m); err != nil {
		return nil, nil, err
	}
	files := map[string][]byte{}
	for p, want := range m.Files {
		if strings.Contains(p, "..") || path.IsAbs(p) {
			return nil, nil, fmt.Errorf("bad path in the manifest: %s", p)
		}
		data, err := os.ReadFile(filepath.Join(dir, filepath.FromSlash(p)))
		if err != nil {
			return nil, nil, err
		}
		sum := sha256.Sum256(data)
		if hex.EncodeToString(sum[:]) != want {
			return nil, nil, fmt.Errorf("%s changed since the backup", p)
		}
		files[p] = data
	}
	return &m, files, nil
}

// RestorePlan puts the backup's files back: the managed folders become what the backup has
// (keys too when it holds them; without, the current keys stay), and the app's other files
// in the backup (settings, recent list) are written back; nothing else is deleted.
func RestorePlan(s store.Store, m *Manifest, files map[string][]byte) ([]Change, error) {
	cur, err := Snapshot(s)
	if err != nil {
		return nil, err
	}
	want := map[string][]byte{}
	for p, b := range files {
		if Managed(p) {
			want[p] = b
		}
	}
	if !m.IncludeKeys {
		for p, b := range cur {
			if IsPrivateKey(p) {
				want[p] = b
			}
		}
	}
	plan := Plan(cur, want)
	var other []string
	for p := range files {
		if !Managed(p) {
			other = append(other, p)
		}
	}
	sort.Strings(other)
	for _, p := range other {
		old, err := s.Read(p)
		switch {
		case errors.Is(err, store.ErrNotExist):
			plan = append(plan, Change{p, Add, files[p]})
		case err != nil:
			return nil, err
		case string(old) != string(files[p]):
			plan = append(plan, Change{p, Modify, files[p]})
		}
	}
	return plan, nil
}
