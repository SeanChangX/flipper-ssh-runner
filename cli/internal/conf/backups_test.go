// SPDX-License-Identifier: GPL-3.0-only
package conf

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"compress/gzip"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/store"
)

func dirStore(t *testing.T) store.Store {
	t.Helper()
	for _, s := range stores(t, seed(t)) {
		if strings.HasPrefix(s.Describe(), "folder") {
			return s
		}
	}
	t.Fatal("no folder store")
	return nil
}

// fakeBackup makes a tool-named backup folder with a manifest, as of `ts`.
func fakeBackup(t *testing.T, parent, id string) {
	t.Helper()
	os.MkdirAll(filepath.Join(parent, id), 0o700)
	os.WriteFile(filepath.Join(parent, id, ManifestFile), []byte(`{"created":"","source":"x","files":{}}`), 0o600)
}

func ids(t *testing.T, parent string) []string {
	t.Helper()
	list, err := ListBackups(parent)
	if err != nil {
		t.Fatal(err)
	}
	var out []string
	for _, b := range list {
		out = append(out, b.ID)
	}
	return out
}

func TestBackupNamesNeverCollide(t *testing.T) {
	parent := t.TempDir()
	now := time.Date(2026, 10, 24, 9, 0, 0, 0, time.Local)
	a, _ := newBackupDir(parent, now)
	b, _ := newBackupDir(parent, now)
	if a == b || filepath.Base(b) != "sshrunner-20261024-090000-2" {
		t.Fatalf("%s %s", a, b)
	}
	os.RemoveAll(a) // pruned: its name is not reused, the next one still sorts newest
	if c, _ := newBackupDir(parent, now); filepath.Base(c) != "sshrunner-20261024-090000-3" {
		t.Fatalf("%s", c)
	}
}

func TestListNewestFirst(t *testing.T) {
	parent := t.TempDir()
	for _, id := range []string{"sshrunner-20261020-100000", "sshrunner-20261024-090000",
		"sshrunner-20261024-090000-2", "sshrunner-20261024-090000-10", "sshrunner-20261022-120000"} {
		fakeBackup(t, parent, id)
	}
	os.MkdirAll(filepath.Join(parent, "sshrunner-notes"), 0o700) // not a backup name
	os.WriteFile(filepath.Join(parent, "readme.txt"), nil, 0o600)
	os.MkdirAll(filepath.Join(parent, "sshrunner-20261025-000000"), 0o700) // no manifest
	want := []string{"sshrunner-20261024-090000-10", "sshrunner-20261024-090000-2", "sshrunner-20261024-090000",
		"sshrunner-20261022-120000", "sshrunner-20261020-100000"}
	if got := ids(t, parent); !reflect.DeepEqual(got, want) {
		t.Fatalf("%v", got)
	}
	if got := ids(t, filepath.Join(parent, "missing")); len(got) != 0 {
		t.Fatalf("missing folder: %v", got)
	}
}

func TestListDescribesABackup(t *testing.T) {
	s, parent := dirStore(t), t.TempDir()
	dir, m, err := Backup(s, parent, true)
	if err != nil {
		t.Fatal(err)
	}
	list, _ := ListBackups(parent)
	if len(list) != 1 {
		t.Fatalf("%v", list)
	}
	b := list[0]
	hosts := 0
	for p := range m.Files {
		if strings.HasPrefix(p, "hosts/") {
			hosts++
		}
	}
	if b.Dir != dir || hosts == 0 || b.Hosts != hosts || !b.Keys || b.Files != len(m.Files) || b.Size == 0 || b.Source != s.Describe() {
		t.Fatalf("%+v", b)
	}
}

func TestPrune(t *testing.T) {
	parent := t.TempDir()
	var all []string
	for d := 1; d <= 14; d++ {
		id := "sshrunner-202610" + []string{"01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11", "12", "13", "14"}[d-1] + "-120000"
		fakeBackup(t, parent, id)
		all = append(all, id)
	}
	foreign := filepath.Join(parent, "my-notes.txt")
	os.WriteFile(foreign, []byte("mine"), 0o600)
	os.MkdirAll(filepath.Join(parent, "old-stuff"), 0o700)

	// keep 0: nothing goes
	if gone, _ := PruneBackups(parent, 0, ""); len(gone) != 0 {
		t.Fatalf("keep 0 deleted %v", gone)
	}
	// the oldest backup is protected (as if just made): it stays, and 10 stay besides it
	gone, err := PruneBackups(parent, 10, filepath.Join(parent, all[0]))
	if err != nil {
		t.Fatal(err)
	}
	if want := all[1:4]; !reflect.DeepEqual(gone, []string{all[3], all[2], all[1]}) {
		t.Fatalf("deleted %v, want %v (oldest first after the protected one)", gone, want)
	}
	left := ids(t, parent)
	if len(left) != 11 || left[len(left)-1] != all[0] || left[0] != all[13] {
		t.Fatalf("left %v", left)
	}
	if _, err := os.Stat(foreign); err != nil {
		t.Fatal("a foreign file was deleted")
	}
	if _, err := os.Stat(filepath.Join(parent, "old-stuff")); err != nil {
		t.Fatal("a foreign folder was deleted")
	}
	// the default, unprotected: the 10 newest stay
	PruneBackups(parent, DefaultKeepBackups, "")
	if left := ids(t, parent); len(left) != 10 || left[9] != all[4] {
		t.Fatalf("left %v", left)
	}
}

func TestDelete(t *testing.T) {
	parent := t.TempDir()
	fakeBackup(t, parent, "sshrunner-20261024-090000")
	os.MkdirAll(filepath.Join(parent, "other"), 0o700)
	if err := DeleteBackup(parent, "other"); err == nil {
		t.Fatal("deleted a folder that is not a backup")
	}
	if err := DeleteBackup(parent, "../"+filepath.Base(parent)); err == nil {
		t.Fatal("deleted outside the backup folder")
	}
	if err := DeleteBackup(parent, "sshrunner-20261024-090000"); err != nil {
		t.Fatal(err)
	}
	if len(ids(t, parent)) != 0 {
		t.Fatal("still listed")
	}
	if _, err := os.Stat(filepath.Join(parent, "other")); err != nil {
		t.Fatal("the other folder went too")
	}
}

func readTree(t *testing.T, dir string) map[string]string {
	t.Helper()
	files, err := backupFiles(dir)
	if err != nil {
		t.Fatal(err)
	}
	out := map[string]string{}
	for _, p := range files {
		b, _ := os.ReadFile(filepath.Join(dir, filepath.FromSlash(p)))
		out[p] = string(b)
	}
	return out
}

func TestExportImportRoundTrip(t *testing.T) {
	for _, ext := range []string{".zip", ".tar.gz"} {
		t.Run(ext, func(t *testing.T) {
			s, home, other := dirStore(t), t.TempDir(), t.TempDir()
			dir, _, err := Backup(s, home, true)
			if err != nil {
				t.Fatal(err)
			}
			file := filepath.Join(t.TempDir(), "export"+ext)
			if err := ExportBackup(dir, file); err != nil {
				t.Fatal(err)
			}
			if err := ExportBackup(dir, file); err == nil {
				t.Fatal("overwrote an existing file")
			}
			got, err := ImportBackup(file, other)
			if err != nil {
				t.Fatal(err)
			}
			if filepath.Base(got) != filepath.Base(dir) {
				t.Fatalf("imported as %s", got)
			}
			if a, b := readTree(t, dir), readTree(t, got); !reflect.DeepEqual(a, b) {
				t.Fatal("imported backup differs from the original")
			}
			if len(ids(t, other)) != 1 {
				t.Fatal("import not listed")
			}
			if _, err := ImportBackup(file, other); err == nil || !strings.Contains(err.Error(), "already") {
				t.Fatalf("second import: %v", err)
			}
			if leftovers, _ := filepath.Glob(filepath.Join(other, ".import-*")); len(leftovers) != 0 {
				t.Fatalf("temporary folders left: %v", leftovers)
			}
		})
	}
	if err := ExportBackup(t.TempDir(), filepath.Join(t.TempDir(), "x.rar")); err == nil {
		t.Fatal("exported to an unknown archive type")
	}
}

type member struct {
	name, body string
	link       bool
}

func writeZip(t *testing.T, ms []member) string {
	t.Helper()
	file := filepath.Join(t.TempDir(), "in.zip")
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for _, m := range ms {
		h := &zip.FileHeader{Name: m.name, Method: zip.Store}
		if m.link {
			h.SetMode(os.ModeSymlink | 0o777)
		} else {
			h.SetMode(0o600)
		}
		w, _ := zw.CreateHeader(h)
		w.Write([]byte(m.body))
	}
	zw.Close()
	os.WriteFile(file, buf.Bytes(), 0o600)
	return file
}

func writeTarGz(t *testing.T, ms []member) string {
	t.Helper()
	file := filepath.Join(t.TempDir(), "in.tar.gz")
	var buf bytes.Buffer
	gz := gzip.NewWriter(&buf)
	tw := tar.NewWriter(gz)
	for _, m := range ms {
		h := &tar.Header{Name: m.name, Mode: 0o600, Size: int64(len(m.body)), Typeflag: tar.TypeReg}
		if m.link {
			h = &tar.Header{Name: m.name, Linkname: "/etc/passwd", Typeflag: tar.TypeSymlink}
		}
		tw.WriteHeader(h)
		if !m.link {
			tw.Write([]byte(m.body))
		}
	}
	tw.Close()
	gz.Close()
	os.WriteFile(file, buf.Bytes(), 0o600)
	return file
}

func TestImportRefuses(t *testing.T) {
	const id = "sshrunner-20261024-090000"
	good := `{"created":"2026-10-24T09:00:00Z","source":"x","files":{}}`
	cases := map[string][]member{
		"bad manifest":    {{name: id + "/" + ManifestFile, body: "{not json"}},
		"no manifest":     {{name: id + "/hosts/a.conf", body: "x"}},
		"not in manifest": {{name: id + "/" + ManifestFile, body: good}, {name: id + "/hosts/extra.conf", body: "x"}},
		"traversal":       {{name: id + "/" + ManifestFile, body: good}, {name: id + "/../evil", body: "x"}},
		"absolute":        {{name: "/" + id + "/" + ManifestFile, body: good}},
		"two folders":     {{name: id + "/" + ManifestFile, body: good}, {name: "sshrunner-20261024-090001/x", body: "x"}},
		"foreign folder":  {{name: "photos/" + ManifestFile, body: good}},
		"file at the top": {{name: ManifestFile, body: good}},
		"symlink":         {{name: id + "/" + ManifestFile, body: good}, {name: id + "/hosts/a.conf", link: true}},
		"backslash path":  {{name: id + "\\..\\evil", body: "x"}},
	}
	for name, ms := range cases {
		for kind, write := range map[string]func(*testing.T, []member) string{"zip": writeZip, "tar.gz": writeTarGz} {
			t.Run(name+" "+kind, func(t *testing.T) {
				parent := t.TempDir()
				if got, err := ImportBackup(write(t, ms), parent); err == nil {
					t.Fatalf("accepted as %s", got)
				}
				if left, _ := os.ReadDir(parent); len(left) != 0 {
					t.Fatalf("left behind: %v", left)
				}
				if _, err := os.Stat(filepath.Join(filepath.Dir(parent), "evil")); err == nil {
					t.Fatal("wrote outside the backup folder")
				}
			})
		}
	}
	// a valid one for contrast
	if _, err := ImportBackup(writeZip(t, []member{{name: id + "/" + ManifestFile, body: good}}), t.TempDir()); err != nil {
		t.Fatal(err)
	}
}
