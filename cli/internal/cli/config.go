// SPDX-License-Identifier: GPL-3.0-only
package cli

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// fileConfig is the optional config file: defaults for flags. Flags win, then the
// environment, then this file, then the built-in defaults.
type fileConfig struct {
	Port        string
	BackupDir   string
	KeepBackups *int
	Mouse       *bool
}

// ConfigPath: $SSHRUNNER_CONFIG, else config.toml in the platform's per-user config folder
// (~/.config/sshrunner on Linux, ~/Library/Application Support/sshrunner on macOS,
// %AppData%\sshrunner on Windows).
func ConfigPath() string {
	if p := os.Getenv("SSHRUNNER_CONFIG"); p != "" {
		return p
	}
	d, err := os.UserConfigDir()
	if err != nil {
		return ""
	}
	return filepath.Join(d, "sshrunner", "config.toml")
}

// readConfig reads `key = value` lines (a small TOML subset: "strings" or 'strings', numbers,
// true / false, # comments). A missing file is an empty config; anything else it cannot
// read is an error naming the line, so a typo is not silently ignored.
func readConfig(path string) (fileConfig, error) {
	var c fileConfig
	if path == "" {
		return c, nil
	}
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return c, nil
	}
	if err != nil {
		return c, err
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for n := 1; sc.Scan(); n++ {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		bad := func(why string) error { return fmt.Errorf("%s line %d: %s", path, n, why) }
		key, val, ok := strings.Cut(line, "=")
		if !ok {
			return c, bad("expected key = value")
		}
		key, val = strings.TrimSpace(key), strings.TrimSpace(val)
		if i := strings.Index(val, " #"); i >= 0 && !strings.HasPrefix(val, `"`) && !strings.HasPrefix(val, "'") {
			val = strings.TrimSpace(val[:i])
		}
		str := func() (string, error) {
			if len(val) >= 2 && (val[0] == '"' && val[len(val)-1] == '"') {
				return strconv.Unquote(val)
			}
			if len(val) >= 2 && val[0] == '\'' && val[len(val)-1] == '\'' {
				return val[1 : len(val)-1], nil
			}
			return "", errors.New("a text value goes in quotes")
		}
		switch key {
		case "port", "backup_dir":
			s, err := str()
			if err != nil {
				return c, bad(key + ": " + err.Error())
			}
			if key == "port" {
				c.Port = s
			} else {
				c.BackupDir = expandTilde(s)
			}
		case "keep_backups":
			v, err := strconv.Atoi(val)
			if err != nil || v < 0 {
				return c, bad("keep_backups: a number, 0 keeps all")
			}
			c.KeepBackups = &v
		case "mouse":
			v, err := strconv.ParseBool(val)
			if err != nil || (val != "true" && val != "false") {
				return c, bad("mouse: true or false")
			}
			c.Mouse = &v
		default:
			return c, bad(fmt.Sprintf("unknown key %q (port, backup_dir, keep_backups, mouse)", key))
		}
	}
	return c, sc.Err()
}

func expandTilde(p string) string {
	if p == "~" || strings.HasPrefix(p, "~/") || strings.HasPrefix(p, `~\`) {
		if home, err := os.UserHomeDir(); err == nil {
			return filepath.Join(home, p[1:])
		}
	}
	return p
}

// defaults: the environment over the file over the built-in values.
type defaults struct {
	port        string
	backupDir   string
	keepBackups int
	mouse       bool
}

func resolveDefaults(c fileConfig) (defaults, error) {
	d := defaults{port: c.Port, backupDir: platformBackupRoot(), keepBackups: DefaultKeep(), mouse: true}
	if c.BackupDir != "" {
		d.backupDir = c.BackupDir
	}
	if v := os.Getenv("SSHRUNNER_BACKUP_DIR"); v != "" {
		d.backupDir = v
	}
	if c.KeepBackups != nil {
		d.keepBackups = *c.KeepBackups
	}
	if v := os.Getenv("SSHRUNNER_KEEP_BACKUPS"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 0 {
			return d, errors.New("SSHRUNNER_KEEP_BACKUPS: a number, 0 keeps all")
		}
		d.keepBackups = n
	}
	if c.Mouse != nil {
		d.mouse = *c.Mouse
	}
	return d, nil
}
