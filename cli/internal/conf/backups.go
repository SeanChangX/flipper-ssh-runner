// SPDX-License-Identifier: GPL-3.0-only
package conf

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

// DefaultKeepBackups: how many backups stay after a change; older ones are pruned.
const DefaultKeepBackups = 10

// importMax bounds what an imported archive may unpack to (an SD card's app data is small).
const importMax = 64 << 20

// backupName is the tool's own folder name: sshrunner-YYYYMMDD-HHMMSS, with -N when two
// backups fall in the same second. Nothing else in the backup folder is ever touched.
var backupName = regexp.MustCompile(`^sshrunner-(\d{8}-\d{6})(?:-(\d+))?$`)

// IsBackupName: a folder name this tool gives its backups.
func IsBackupName(name string) bool { return backupName.MatchString(name) }

// newBackupDir makes a fresh backup folder under parent, never reusing one.
func newBackupDir(parent string, now time.Time) (string, error) {
	if err := os.MkdirAll(parent, 0o700); err != nil {
		return "", err
	}
	base := "sshrunner-" + now.Format("20060102-150405")
	// after the newest of this second, so a pruned name is never reused out of order
	start := 1
	if list, err := os.ReadDir(parent); err == nil {
		for _, e := range list {
			if m := backupName.FindStringSubmatch(e.Name()); m != nil && "sshrunner-"+m[1] == base {
				n := 1
				if m[2] != "" {
					n, _ = strconv.Atoi(m[2])
				}
				start = max(start, n+1)
			}
		}
	}
	for n := start; n < start+1000; n++ {
		name := base
		if n > 1 {
			name = fmt.Sprintf("%s-%d", base, n)
		}
		dir := filepath.Join(parent, name)
		err := os.Mkdir(dir, 0o700)
		if err == nil {
			if err := ownerOnly(dir); err != nil {
				os.Remove(dir)
				return "", fmt.Errorf("making %s readable by you only: %w", dir, err)
			}
			return dir, nil
		}
		if !errors.Is(err, fs.ErrExist) {
			return "", err
		}
	}
	return "", fmt.Errorf("no free backup name for %s", base)
}

// BackupInfo is one backup in the list.
type BackupInfo struct {
	ID      string // the folder name
	Dir     string
	Created time.Time
	Source  string
	Hosts   int
	Keys    bool // private keys included
	Files   int
	Size    int64
}

// sortKey orders backups by time, then by the same-second counter.
func sortKey(id string) (string, int) {
	m := backupName.FindStringSubmatch(id)
	n := 1
	if m[2] != "" {
		n, _ = strconv.Atoi(m[2])
	}
	return m[1], n
}

func newerFirst(ids []string) {
	sort.Slice(ids, func(i, j int) bool {
		ti, ni := sortKey(ids[i])
		tj, nj := sortKey(ids[j])
		if ti != tj {
			return ti > tj
		}
		return ni > nj
	})
}

// backupIDs: the folders under parent that are this tool's backups (name and manifest),
// newest first. A missing parent is an empty list.
func backupIDs(parent string) ([]string, error) {
	list, err := os.ReadDir(parent)
	if errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var ids []string
	for _, e := range list {
		if !e.IsDir() || !IsBackupName(e.Name()) {
			continue
		}
		if st, err := os.Lstat(filepath.Join(parent, e.Name(), ManifestFile)); err != nil || !st.Mode().IsRegular() {
			continue
		}
		ids = append(ids, e.Name())
	}
	newerFirst(ids)
	return ids, nil
}

// ListBackups describes every backup under parent, newest first. A backup whose manifest
// cannot be read is still listed (by its folder name) so it can be deleted.
func ListBackups(parent string) ([]BackupInfo, error) {
	ids, err := backupIDs(parent)
	if err != nil {
		return nil, err
	}
	var out []BackupInfo
	for _, id := range ids {
		b := BackupInfo{ID: id, Dir: filepath.Join(parent, id)}
		ts, _ := sortKey(id)
		b.Created, _ = time.ParseInLocation("20060102-150405", ts, time.Local)
		if m, err := readManifest(b.Dir); err == nil {
			if t, err := time.Parse(time.RFC3339, m.Created); err == nil {
				b.Created = t
			}
			b.Source, b.Keys, b.Files = m.Source, m.IncludeKeys, len(m.Files)
			for p := range m.Files {
				if strings.HasPrefix(p, HostsDir+"/") && strings.HasSuffix(p, ".conf") {
					b.Hosts++
				}
			}
		} else {
			b.Source = "(unreadable manifest)"
		}
		_ = filepath.WalkDir(b.Dir, func(_ string, d fs.DirEntry, err error) error {
			if err == nil && d.Type().IsRegular() {
				if fi, err := d.Info(); err == nil {
					b.Size += fi.Size()
				}
			}
			return nil
		})
		out = append(out, b)
	}
	return out, nil
}

func readManifest(dir string) (*Manifest, error) {
	j, err := os.ReadFile(filepath.Join(dir, ManifestFile))
	if err != nil {
		return nil, err
	}
	var m Manifest
	if err := json.Unmarshal(j, &m); err != nil {
		return nil, err
	}
	return &m, nil
}

// PruneBackups keeps the newest `keep` backups under parent and deletes the older ones;
// keep <= 0 keeps all. `protect` (a backup folder, e.g. the one just made) is never deleted.
// Only this tool's backups are considered. Returns the deleted IDs.
func PruneBackups(parent string, keep int, protect string) ([]string, error) {
	if keep <= 0 {
		return nil, nil
	}
	ids, err := backupIDs(parent)
	if err != nil {
		return nil, err
	}
	protect = filepath.Base(protect)
	var gone []string
	kept := 0
	for _, id := range ids {
		if id == protect || kept < keep {
			kept++
			continue
		}
		if err := os.RemoveAll(filepath.Join(parent, id)); err != nil {
			return gone, err
		}
		gone = append(gone, id)
	}
	return gone, nil
}

// backupDir resolves an ID (or a path to a backup folder) under parent, refusing anything
// that is not one of this tool's backups.
func backupDir(parent, id string) (string, error) {
	id = filepath.Base(filepath.Clean(id))
	if !IsBackupName(id) {
		return "", fmt.Errorf("%q is not an sshrunner backup name", id)
	}
	dir := filepath.Join(parent, id)
	if st, err := os.Lstat(filepath.Join(dir, ManifestFile)); err != nil || !st.Mode().IsRegular() {
		return "", fmt.Errorf("no backup %s in %s", id, parent)
	}
	return dir, nil
}

// ResolveBackup: a backup ID under parent, or a path to any backup folder.
func ResolveBackup(parent, arg string) (string, error) {
	if dir, err := backupDir(parent, arg); err == nil {
		return dir, nil
	}
	if _, err := os.Stat(filepath.Join(arg, ManifestFile)); err == nil {
		return arg, nil
	}
	return backupDir(parent, arg)
}

// DeleteBackup removes one of this tool's backups under parent.
func DeleteBackup(parent, id string) error {
	dir, err := backupDir(parent, id)
	if err != nil {
		return err
	}
	return os.RemoveAll(dir)
}

// backupFiles: every file of a backup folder, as slash paths; a link or special file is an error.
func backupFiles(dir string) ([]string, error) {
	var files []string
	err := filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		if !d.Type().IsRegular() {
			return fmt.Errorf("%s: not a regular file", p)
		}
		rel, err := filepath.Rel(dir, p)
		if err != nil {
			return err
		}
		files = append(files, filepath.ToSlash(rel))
		return nil
	})
	sort.Strings(files)
	return files, err
}

// ArchiveKind: "zip" or "tar.gz" by the file name, "" for anything else.
func ArchiveKind(name string) string {
	l := strings.ToLower(name)
	switch {
	case strings.HasSuffix(l, ".zip"):
		return "zip"
	case strings.HasSuffix(l, ".tar.gz"), strings.HasSuffix(l, ".tgz"):
		return "tar.gz"
	}
	return ""
}

// ExportBackup writes a backup folder as one archive (.zip or .tar.gz by the file's name),
// its files under the backup's ID. It refuses to overwrite an existing file.
func ExportBackup(dir, file string) error {
	kind := ArchiveKind(file)
	if kind == "" {
		return fmt.Errorf("%s: name it .zip or .tar.gz", file)
	}
	if _, _, err := ReadBackup(dir); err != nil {
		return err
	}
	files, err := backupFiles(dir)
	if err != nil {
		return err
	}
	id := filepath.Base(filepath.Clean(dir))
	f, err := os.OpenFile(file, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return err
	}
	werr := writeArchive(f, kind, id, dir, files)
	cerr := f.Close()
	if werr != nil || cerr != nil {
		os.Remove(file)
		return errors.Join(werr, cerr)
	}
	return nil
}

func writeArchive(w io.Writer, kind, id, dir string, files []string) error {
	mtime := time.Now()
	if kind == "zip" {
		zw := zip.NewWriter(w)
		for _, p := range files {
			data, err := os.ReadFile(filepath.Join(dir, filepath.FromSlash(p)))
			if err != nil {
				return err
			}
			h := &zip.FileHeader{Name: id + "/" + p, Method: zip.Deflate, Modified: mtime}
			h.SetMode(0o600)
			fw, err := zw.CreateHeader(h)
			if err != nil {
				return err
			}
			if _, err := fw.Write(data); err != nil {
				return err
			}
		}
		return zw.Close()
	}
	gz := gzip.NewWriter(w)
	tw := tar.NewWriter(gz)
	for _, p := range files {
		data, err := os.ReadFile(filepath.Join(dir, filepath.FromSlash(p)))
		if err != nil {
			return err
		}
		h := &tar.Header{Name: id + "/" + p, Mode: 0o600, Size: int64(len(data)), ModTime: mtime, Typeflag: tar.TypeReg}
		if err := tw.WriteHeader(h); err != nil {
			return err
		}
		if _, err := tw.Write(data); err != nil {
			return err
		}
	}
	if err := tw.Close(); err != nil {
		return err
	}
	return gz.Close()
}

// archiveEntry is one regular file read from an archive.
type archiveEntry struct {
	name string
	data []byte
}

// safeEntry checks an archive member's name: relative, slash-separated, no "..", one
// backup folder at the top.
func safeEntry(name string) (top, rel string, err error) {
	if name == "" || strings.Contains(name, "\\") || strings.HasPrefix(name, "/") || path.IsAbs(name) ||
		(len(name) > 1 && name[1] == ':') || !filepath.IsLocal(name) {
		return "", "", fmt.Errorf("unsafe path in the archive: %q", name)
	}
	for _, part := range strings.Split(strings.TrimSuffix(name, "/"), "/") {
		if part == ".." || part == "." || part == "" {
			return "", "", fmt.Errorf("unsafe path in the archive: %q", name)
		}
	}
	top, rel, _ = strings.Cut(strings.TrimSuffix(name, "/"), "/")
	return top, rel, nil
}

func readArchive(file string) ([]archiveEntry, error) {
	kind := ArchiveKind(file)
	var out []archiveEntry
	total := int64(0)
	add := func(name string, r io.Reader, size int64) error {
		// a name must stay inside the folder it is unpacked into: not absolute, no "..", and
		// on Windows no reserved device name (NUL, COM1) or ":" (an alternate data stream)
		if !filepath.IsLocal(name) {
			return fmt.Errorf("unsafe path in the archive: %q", name)
		}
		total += size
		if size < 0 || total > importMax {
			return fmt.Errorf("the archive unpacks to more than %d MB", importMax>>20)
		}
		data, err := io.ReadAll(io.LimitReader(r, importMax+1))
		if err != nil {
			return err
		}
		if int64(len(data)) != size {
			return fmt.Errorf("%s: size does not match the archive's header", name)
		}
		out = append(out, archiveEntry{name, data})
		return nil
	}
	switch kind {
	case "zip":
		zr, err := zip.OpenReader(file)
		if err != nil {
			return nil, err
		}
		defer zr.Close()
		for _, f := range zr.File {
			mode := f.Mode()
			switch {
			case mode&fs.ModeSymlink != 0:
				return nil, fmt.Errorf("the archive holds a link (%s): refused", f.Name)
			case mode.IsDir() || strings.HasSuffix(f.Name, "/"):
				if _, _, err := safeEntry(f.Name); err != nil {
					return nil, err
				}
				continue
			case !mode.IsRegular():
				return nil, fmt.Errorf("the archive holds a special file (%s): refused", f.Name)
			}
			rc, err := f.Open()
			if err != nil {
				return nil, err
			}
			err = add(f.Name, rc, int64(f.UncompressedSize64))
			rc.Close()
			if err != nil {
				return nil, err
			}
		}
	case "tar.gz":
		f, err := os.Open(file)
		if err != nil {
			return nil, err
		}
		defer f.Close()
		gz, err := gzip.NewReader(f)
		if err != nil {
			return nil, err
		}
		tr := tar.NewReader(gz)
		for {
			h, err := tr.Next()
			if err == io.EOF {
				break
			}
			if err != nil {
				return nil, err
			}
			switch h.Typeflag {
			case tar.TypeDir:
				if _, _, err := safeEntry(h.Name); err != nil {
					return nil, err
				}
				continue
			case tar.TypeReg:
			case tar.TypeSymlink, tar.TypeLink:
				return nil, fmt.Errorf("the archive holds a link (%s): refused", h.Name)
			default:
				return nil, fmt.Errorf("the archive holds a special file (%s): refused", h.Name)
			}
			if err := add(h.Name, tr, h.Size); err != nil {
				return nil, err
			}
		}
	default:
		return nil, fmt.Errorf("%s: not a .zip or .tar.gz", file)
	}
	return out, nil
}

// ImportBackup unpacks an exported archive into parent as a backup, after checking it: one
// sshrunner backup folder, safe paths, regular files only, a manifest whose files are all
// there with their SHA-256 and nothing else beside them. Returns the new backup's folder.
func ImportBackup(file, parent string) (string, error) {
	entries, err := readArchive(file)
	if err != nil {
		return "", err
	}
	if len(entries) == 0 {
		return "", errors.New("the archive is empty")
	}
	id := ""
	for _, e := range entries {
		top, rel, err := safeEntry(e.name)
		if err != nil {
			return "", err
		}
		if rel == "" {
			return "", fmt.Errorf("unexpected file at the archive's top: %q", e.name)
		}
		if id == "" {
			id = top
		}
		if top != id {
			return "", errors.New("the archive holds more than one folder: not an sshrunner backup")
		}
	}
	if !IsBackupName(id) {
		return "", fmt.Errorf("%q is not an sshrunner backup folder", id)
	}
	if _, err := os.Lstat(filepath.Join(parent, id)); err == nil {
		return "", fmt.Errorf("backup %s is already in %s", id, parent)
	}
	if err := os.MkdirAll(parent, 0o700); err != nil {
		return "", err
	}
	tmp, err := os.MkdirTemp(parent, ".import-")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(tmp)
	dir := filepath.Join(tmp, id)
	for _, e := range entries {
		_, rel, _ := safeEntry(e.name)
		dst := filepath.Join(dir, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(dst), 0o700); err != nil {
			return "", err
		}
		if err := os.WriteFile(dst, e.data, 0o600); err != nil {
			return "", err
		}
	}
	m, _, err := ReadBackup(dir)
	if err != nil {
		return "", err
	}
	for _, e := range entries {
		_, rel, _ := safeEntry(e.name)
		if _, ok := m.Files[rel]; !ok && rel != ManifestFile {
			return "", fmt.Errorf("%s is not in the backup's manifest", rel)
		}
	}
	if err := ownerOnly(dir); err != nil {
		return "", fmt.Errorf("making the backup readable by you only: %w", err)
	}
	final := filepath.Join(parent, id)
	if err := os.Rename(dir, final); err != nil {
		return "", err
	}
	return final, nil
}
