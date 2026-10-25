// Package store is where SSH Runner's data folder lives: on a Flipper over USB, or in a
// local folder (an SD card in a card reader, a backup). Paths are relative to the data folder
// (apps_data/ssh_runner), with '/' separators.
//
// SPDX-License-Identifier: GPL-3.0-only
package store

import (
	"errors"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/flipper"
)

// AppDataDir is the app's folder on the Flipper's SD card.
const AppDataDir = "/ext/apps_data/ssh_runner"

// Entry is a file or directory in the data folder.
type Entry struct {
	Name  string // base name
	IsDir bool
	Size  uint64
}

// Store is the data folder.
type Store interface {
	Describe() string
	// List a directory; a missing one lists as empty.
	List(rel string) ([]Entry, error)
	Read(rel string) ([]byte, error)
	// Write a file, creating its directories.
	Write(rel string, data []byte) error
	// Delete a file; a missing one is fine.
	Delete(rel string) error
	// Busy: the app is running and could write the same files (Flipper only).
	Busy() (bool, error)
	Close() error
}

// ErrNotExist is returned by Read for a missing file.
var ErrNotExist = errors.New("file does not exist")

// ---------------- Flipper ----------------

type flipperStore struct {
	c    *flipper.Client
	port string
	made map[string]bool
}

// OpenFlipper connects over USB (port "" = the only Flipper found).
func OpenFlipper(port string) (Store, error) {
	c, p, err := flipper.Open(port)
	if err != nil {
		return nil, err
	}
	return &flipperStore{c: c, port: p, made: map[string]bool{}}, nil
}

// NewFlipper wraps an open client (tests use the fake Flipper).
func NewFlipper(c *flipper.Client, port string) Store {
	return &flipperStore{c: c, port: port, made: map[string]bool{}}
}

func (s *flipperStore) abs(rel string) string { return path.Join(AppDataDir, rel) }

func (s *flipperStore) Describe() string { return "Flipper on " + s.port }

func (s *flipperStore) List(rel string) ([]Entry, error) {
	l, err := s.c.List(s.abs(rel))
	if flipper.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	out := make([]Entry, 0, len(l))
	for _, e := range l {
		out = append(out, Entry{Name: path.Base(e.Path), IsDir: e.IsDir, Size: e.Size})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

func (s *flipperStore) Read(rel string) ([]byte, error) {
	b, err := s.c.Read(s.abs(rel))
	if flipper.IsNotExist(err) {
		return nil, ErrNotExist
	}
	return b, err
}

func (s *flipperStore) mkdirs(dir string) error {
	// /ext/apps_data/ssh_runner and the folders below it, one at a time (the RPC has no -p)
	parts := strings.Split(strings.TrimPrefix(dir, "/"), "/")
	cur := ""
	for _, p := range parts {
		cur += "/" + p
		if cur == "/ext" || s.made[cur] {
			continue
		}
		if err := s.c.Mkdir(cur); err != nil {
			return err
		}
		s.made[cur] = true
	}
	return nil
}

func (s *flipperStore) Write(rel string, data []byte) error {
	p := s.abs(rel)
	if err := s.mkdirs(path.Dir(p)); err != nil {
		return err
	}
	return s.c.Write(p, data)
}

func (s *flipperStore) Delete(rel string) error { return s.c.Delete(s.abs(rel), false) }

func (s *flipperStore) Busy() (bool, error) { return s.c.AppRunning() }

func (s *flipperStore) Close() error { return s.c.Close() }

// ---------------- local folder ----------------

type dirStore struct{ root string }

// OpenDir is a data folder on this computer: the SD card's apps_data/ssh_runner (or the
// card's root, which is detected) or a backup.
func OpenDir(root string) (Store, error) {
	root = filepath.Clean(root) // shown in messages: the system's own separators
	if st, err := os.Stat(filepath.Join(root, "apps_data", "ssh_runner")); err == nil && st.IsDir() {
		root = filepath.Join(root, "apps_data", "ssh_runner") // the card itself was given
	}
	st, err := os.Stat(root)
	if err != nil {
		return nil, err
	}
	if !st.IsDir() {
		return nil, errors.New(root + " is not a folder")
	}
	return &dirStore{root: root}, nil
}

// IsFlipper: the store is a Flipper over USB (not a folder).
func IsFlipper(s Store) bool {
	_, ok := s.(*flipperStore)
	return ok
}

func (s *dirStore) abs(rel string) string { return filepath.Join(s.root, filepath.FromSlash(rel)) }

func (s *dirStore) Describe() string { return "folder " + s.root }

func (s *dirStore) List(rel string) ([]Entry, error) {
	l, err := os.ReadDir(s.abs(rel))
	if errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var out []Entry
	for _, e := range l {
		info, err := e.Info()
		if err != nil {
			return nil, err
		}
		out = append(out, Entry{Name: e.Name(), IsDir: e.IsDir(), Size: uint64(info.Size())})
	}
	return out, nil
}

func (s *dirStore) Read(rel string) ([]byte, error) {
	b, err := os.ReadFile(s.abs(rel))
	if errors.Is(err, fs.ErrNotExist) {
		return nil, ErrNotExist
	}
	return b, err
}

func (s *dirStore) Write(rel string, data []byte) error {
	p := s.abs(rel)
	if err := os.MkdirAll(filepath.Dir(p), 0o700); err != nil {
		return err
	}
	// private keys: owner-only, as ssh expects of ~/.ssh
	return os.WriteFile(p, data, 0o600)
}

func (s *dirStore) Delete(rel string) error {
	err := os.Remove(s.abs(rel))
	if errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	return err
}

func (s *dirStore) Busy() (bool, error) { return false, nil }

func (s *dirStore) Close() error { return nil }
