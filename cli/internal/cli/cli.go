// Package cli is sshrunner's command line: the TUI by default, or one command for scripts.
//
// SPDX-License-Identifier: GPL-3.0-only
package cli

import (
	"bufio"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/charmbracelet/lipgloss"
	"github.com/mattn/go-isatty"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/appdata"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/conf"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/plural"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/store"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/tui"
	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/version"
)

const usage = `sshrunner - manage SSH Runner's hosts, keys and scripts on a Flipper Zero

Usage:
  sshrunner [flags]                     open the interactive editor
  sshrunner [flags] <command> [args]

Commands:
  status                    what is on the Flipper (or folder)
  hosts                     list the hosts
  pull <dir>                copy hosts, commands, scripts and public keys to <dir> to edit
                            (--include-keys: private keys too)
  push <dir>                write <dir>'s files to the Flipper, showing the changes first
                            (--delete: also remove hosts and scripts <dir> does not have)
  import-key <file>         add a private key (and its .pub next to it, if any)
                            (--name: the name on the Flipper; default the file's name)
  backup                    save the whole data folder, private keys included
                            (--no-keys: leave the private keys out)
  restore <backup>          put a backup back (its ID or folder), showing the changes first
  backups list              the backups, newest first
  backups export <id> <file>  write one backup as a .zip or .tar.gz
  backups import <file>     bring an exported backup in (checked before it is listed)
  backups delete <id>       delete one backup
  completion <shell>        print a completion script: bash, zsh, fish or powershell
  version                   print the version

Flags:
  --port <device>           the Flipper's serial port (default: the only one connected)
  --dir <folder>            work on a folder instead: the SD card in a card reader, or a copy
  --backup-dir <folder>     where backups go (default: %s)
  --keep-backups <n>        backups kept after a change, older ones deleted (default %d;
                            0 keeps all; or SSHRUNNER_KEEP_BACKUPS)
  --yes                     apply without asking (scripts)
  --json                    status, hosts and backups list as JSON (scripts)
  --no-mouse                the editor ignores the mouse (Shift+drag selects text either way
                            in most terminals)

Config file (optional): %s
  port = "COM3"   backup_dir = "~/backups"   keep_backups = 10   mouse = true
Flags win over the environment (SSHRUNNER_BACKUP_DIR, SSHRUNNER_KEEP_BACKUPS), which wins
over the file.

Before every push, restore or key import, the data folder is backed up first; after the
change only the newest backups are kept (--keep-backups).
Close qFlipper and the SSH Runner app while sshrunner is connected.
`

type options struct {
	port, dir, backupDir string
	keepBackups          int
	yes, includeKeys     bool
	noKeys               bool
	asJSON, noMouse      bool
	mouse                bool
	deleteMissing        bool
	name                 string
	out, err             io.Writer
	in                   io.Reader
}

// Main runs the command line and returns the exit code.
func Main(args []string) int {
	o := &options{out: os.Stdout, err: os.Stderr, in: os.Stdin}
	if err := run(o, args); err != nil {
		fmt.Fprintln(os.Stderr, styleErr.Render("error:"), err)
		return 1
	}
	return 0
}

// platformBackupRoot is where backups go by default: the platform's per-user data folder
// (XDG on Linux, Application Support on macOS, LocalAppData on Windows).
func platformBackupRoot() string {
	home, _ := os.UserHomeDir()
	switch runtime.GOOS {
	case "windows":
		if d := os.Getenv("LOCALAPPDATA"); d != "" {
			return filepath.Join(d, "sshrunner", "backups")
		}
	case "darwin":
		return filepath.Join(home, "Library", "Application Support", "sshrunner", "backups")
	}
	if d := os.Getenv("XDG_DATA_HOME"); d != "" {
		return filepath.Join(d, "sshrunner", "backups")
	}
	return filepath.Join(home, ".local", "share", "sshrunner", "backups")
}

// DefaultKeep is the built-in --keep-backups.
func DefaultKeep() int { return conf.DefaultKeepBackups }

func run(o *options, args []string) error {
	cfgPath := ConfigPath()
	cfg, err := readConfig(cfgPath)
	if err != nil {
		return err
	}
	def, err := resolveDefaults(cfg)
	if err != nil {
		return err
	}
	o.mouse = def.mouse
	fs := flag.NewFlagSet("sshrunner", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	fs.StringVar(&o.port, "port", def.port, "")
	fs.StringVar(&o.dir, "dir", "", "")
	fs.StringVar(&o.backupDir, "backup-dir", def.backupDir, "")
	fs.IntVar(&o.keepBackups, "keep-backups", def.keepBackups, "")
	fs.BoolVar(&o.asJSON, "json", false, "")
	fs.BoolVar(&o.noMouse, "no-mouse", false, "")
	fs.BoolVar(&o.yes, "yes", false, "")
	// --include-keys: pull's private keys; for backup (keys by default now) it is accepted and
	// changes nothing, so older scripts keep working
	fs.BoolVar(&o.includeKeys, "include-keys", false, "")
	fs.BoolVar(&o.noKeys, "no-keys", false, "")
	fs.BoolVar(&o.deleteMissing, "delete", false, "")
	fs.StringVar(&o.name, "name", "", "")
	showVersion := fs.Bool("version", false, "")
	help := fs.Bool("help", false, "")
	fs.BoolVar(help, "h", false, "")
	// flags may come before or after the command
	var rest []string
	for len(args) > 0 {
		if err := fs.Parse(args); err != nil {
			return fmt.Errorf("%w (see sshrunner --help)", err)
		}
		args = fs.Args()
		if len(args) > 0 {
			rest = append(rest, args[0])
			args = args[1:]
		}
	}
	if *help {
		fmt.Fprintf(o.out, usage, def.backupDir, conf.DefaultKeepBackups, cfgPath)
		return nil
	}
	// paths are shown back in messages: in the system's own spelling (C:\..., not C:/...)
	if o.backupDir != "" {
		o.backupDir = filepath.Clean(o.backupDir)
	}
	if o.dir != "" {
		o.dir = filepath.Clean(o.dir)
	}
	if o.noMouse {
		o.mouse = false
	}
	if o.keepBackups < 0 {
		return errors.New("--keep-backups takes 0 (keep all) or more")
	}
	if *showVersion || (len(rest) > 0 && rest[0] == "version") {
		fmt.Fprintln(o.out, "sshrunner", version.String())
		return nil
	}
	if len(rest) == 0 {
		return tui.Run(tui.Options{Port: o.port, Dir: o.dir, BackupDir: o.backupDir, KeepBackups: o.keepBackups, Mouse: o.mouse})
	}
	cmd, params := rest[0], rest[1:]
	switch cmd {
	case "backups":
		return backups(o, params) // local files only: no Flipper needed
	case "completion":
		if len(params) != 1 {
			return errors.New("completion takes the shell: bash, zsh, fish or powershell")
		}
		return completion(o.out, params[0])
	}
	need := map[string]int{"status": 0, "hosts": 0, "pull": 1, "push": 1, "import-key": 1, "backup": 0, "restore": 1}
	n, ok := need[cmd]
	if !ok {
		return unknownCommand(cmd)
	}
	if cmd == "backup" && len(params) > 0 && slices.Contains(backupsSubcommands, params[0]) {
		return fmt.Errorf("backup takes no arguments; did you mean %q? (see sshrunner --help)", "backups "+params[0])
	}
	if len(params) != n {
		return fmt.Errorf("%s takes %s (see sshrunner --help)", cmd, plural.N(n, "argument"))
	}
	s, err := open(o)
	if err != nil {
		return err
	}
	defer s.Close()
	switch cmd {
	case "status":
		return status(o, s)
	case "hosts":
		return hosts(o, s)
	case "pull":
		return pull(o, s, params[0])
	case "push":
		return push(o, s, params[0])
	case "import-key":
		return importKey(o, s, params[0])
	case "backup":
		pr := o.progress("Backing up")
		dir, m, err := conf.BackupProgress(s, o.backupDir, !o.noKeys, pr.step)
		pr.done()
		if err != nil {
			return err
		}
		fmt.Fprintf(o.out, "%s %s to %s\n", styleOK.Render("Backed up"), plural.N(len(m.Files), "file"), dir)
		switch n := m.PrivateKeys(); {
		case o.noKeys:
			fmt.Fprintln(o.out, styleDim.Render("Private keys left out (--no-keys): this backup cannot bring them back."))
		case n > 0:
			fmt.Fprintln(o.out, styleDim.Render(fmt.Sprintf("Includes %s; stored only on this computer, readable by you only.", plural.N(n, "private key"))))
		}
		prune(o, dir)
		return nil
	case "restore":
		return restore(o, s, params[0])
	}
	return nil
}

func open(o *options) (store.Store, error) {
	if o.dir != "" {
		return store.OpenDir(o.dir)
	}
	return store.OpenFlipper(o.port)
}

var (
	styleOK   = lipgloss.NewStyle().Foreground(lipgloss.AdaptiveColor{Light: "28", Dark: "114"}).Bold(true)
	styleErr  = lipgloss.NewStyle().Foreground(lipgloss.AdaptiveColor{Light: "160", Dark: "203"}).Bold(true)
	styleWarn = lipgloss.NewStyle().Foreground(lipgloss.AdaptiveColor{Light: "130", Dark: "179"})
	styleDim  = lipgloss.NewStyle().Foreground(lipgloss.AdaptiveColor{Light: "244", Dark: "244"})
	styleHead = lipgloss.NewStyle().Bold(true)
)

func status(o *options, s store.Store) error {
	c, _, err := conf.Load(s)
	if err != nil {
		return err
	}
	pubOnly := 0
	for _, k := range c.Keys {
		if k.PublicOnly() {
			pubOnly++
		}
	}
	if o.asJSON {
		return writeJSON(o, statusJSON{Source: s.Describe(), Hosts: len(c.Hosts), Commands: len(c.Commands),
			Keys: len(c.Keys), PublicOnly: pubOnly, Scripts: len(c.Scripts), Unreadable: c.Unreadable})
	}
	fmt.Fprintln(o.out, styleHead.Render(s.Describe()))
	keys := strconv.Itoa(len(c.Keys))
	if pubOnly > 0 {
		keys += styleDim.Render(fmt.Sprintf("  (%d public only: the private key is not in this folder)", pubOnly))
	}
	fmt.Fprintf(o.out, "  hosts     %d\n  commands  %d\n  keys      %s\n  scripts   %d\n",
		len(c.Hosts), len(c.Commands), keys, len(c.Scripts))
	printUnreadable(o, c)
	return nil
}

func printUnreadable(o *options, c *conf.Config) {
	var ps []string
	for p := range c.Unreadable {
		ps = append(ps, p)
	}
	sort.Strings(ps)
	for _, p := range ps {
		fmt.Fprintf(o.out, "  %s %s: %s (left as it is)\n", styleWarn.Render("!"), p, c.Unreadable[p])
	}
}

func hosts(o *options, s store.Store) error {
	c, _, err := conf.Load(s)
	if err != nil {
		return err
	}
	if o.asJSON {
		out := []hostJSON{}
		for _, h := range c.Hosts {
			cmds := h.Cmds
			if cmds == nil {
				cmds = []string{}
			}
			out = append(out, hostJSON{Alias: h.Alias, User: h.User, Host: h.Host, Port: h.Port, Auth: h.Auth,
				Key: h.Key, SavePassword: h.Remember, Transport: h.Transport, HostKey: h.Policy, Commands: cmds})
		}
		return writeJSON(o, out)
	}
	rows := [][]string{{"ALIAS", "TARGET", "AUTH", "TRANSPORT", "HOST KEY"}}
	for _, h := range c.Hosts {
		auth := h.Auth
		if h.Auth == "key" {
			auth = "key: " + h.Key
		}
		rows = append(rows, []string{h.Alias, fmt.Sprintf("%s@%s:%d", h.User, h.Host, h.Port), auth, h.Transport, h.Policy})
	}
	printTable(o, rows)
	return nil
}

func pull(o *options, s store.Store, dst string) error {
	cur, err := conf.Snapshot(s)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dst, 0o700); err != nil {
		return err
	}
	out, err := store.OpenDir(dst)
	if err != nil {
		return err
	}
	n, skipped := 0, 0
	var paths []string
	for p := range cur {
		paths = append(paths, p)
	}
	sort.Strings(paths)
	pr := o.progress("Pulling")
	for i, p := range paths {
		pr.step(i+1, len(paths), p)
		if conf.IsPrivateKey(p) && !o.includeKeys {
			skipped++
			continue
		}
		if err := out.Write(p, cur[p]); err != nil {
			pr.done()
			return fmt.Errorf("%s: %w", p, err)
		}
		n++
	}
	pr.done()
	fmt.Fprintf(o.out, "%s %s to %s\n", styleOK.Render("Pulled"), plural.N(n, "file"), filepath.Clean(dst))
	if skipped > 0 {
		where := "not copied"
		if store.IsFlipper(s) {
			where = "left on the Flipper"
		}
		fmt.Fprintf(o.out, "%s\n", styleDim.Render(fmt.Sprintf("%s %s (add --include-keys to copy private keys too).", plural.N(skipped, "private key"), where)))
	}
	fmt.Fprintln(o.out, styleDim.Render("To back up everything, use `sshrunner backup`."))
	return nil
}

// push: the folder's managed files over the Flipper's; with --delete, hosts, scripts and
// commands the folder does not have go too. Keys are never deleted from a folder push.
func push(o *options, s store.Store, src string) error {
	in, err := store.OpenDir(src)
	if err != nil {
		return err
	}
	local, err := conf.Snapshot(in)
	if err != nil {
		return err
	}
	if err := checkFiles(local); err != nil {
		return err
	}
	cur, err := conf.Snapshot(s)
	if err != nil {
		return err
	}
	want := map[string][]byte{}
	for p, b := range cur {
		if !o.deleteMissing || conf.IsKeyPath(p) {
			want[p] = b
		}
	}
	for p, b := range local {
		want[p] = b
	}
	if err := checkKeysPresent(local, want); err != nil {
		return err
	}
	return applyPlan(o, s, conf.Plan(cur, want))
}

// checkKeysPresent refuses a pushed host that uses a key whose private file is on neither
// side (a folder may hold only the .pub: the private file then has to be on the target).
func checkKeysPresent(pushed, result map[string][]byte) error {
	var errs []error
	var paths []string
	for p := range pushed {
		paths = append(paths, p)
	}
	sort.Strings(paths)
	for _, p := range paths {
		dir, base := filepath.Split(filepath.FromSlash(p))
		if filepath.ToSlash(dir) != conf.HostsDir+"/" || !strings.HasSuffix(base, ".conf") {
			continue
		}
		h, err := appdata.ParseHost(string(pushed[p]), strings.TrimSuffix(base, ".conf"))
		if err != nil || h.Auth != "key" {
			continue
		}
		if _, ok := result[conf.KeysDir+"/"+h.Key]; !ok {
			errs = append(errs, fmt.Errorf("%s: key %q is neither in the folder nor on the target (copy its private file into keys/)", p, h.Key))
		}
	}
	return errors.Join(errs...)
}

// checkFiles refuses a folder the app could not read.
func checkFiles(files map[string][]byte) error {
	var errs []error
	for p, b := range files {
		dir, base := filepath.Split(filepath.FromSlash(p))
		dir = filepath.ToSlash(dir)
		switch {
		case p == conf.CommandsFile:
			if _, err := appdata.ParseCommands(string(b)); err != nil {
				errs = append(errs, fmt.Errorf("%s: %w", p, err))
			}
		case dir == conf.HostsDir+"/" && strings.HasSuffix(base, ".conf"):
			alias := strings.TrimSuffix(base, ".conf")
			h, err := appdata.ParseHost(string(b), alias)
			if err == nil {
				err = h.Validate()
			}
			if err != nil {
				errs = append(errs, fmt.Errorf("%s: %w", p, err))
			}
		case conf.IsPrivateKey(p):
			if info := appdata.InspectKey(b); info.Problem != "" {
				errs = append(errs, fmt.Errorf("%s: %s", p, info.Problem))
			}
		}
	}
	return errors.Join(errs...)
}

func importKey(o *options, s store.Store, file string) error {
	data, err := os.ReadFile(file)
	if err != nil {
		return err
	}
	info := appdata.InspectKey(data)
	if info.Problem != "" {
		return fmt.Errorf("%s: %s", file, info.Problem)
	}
	name := o.name
	if name == "" {
		name = filepath.Base(file)
	}
	if !appdata.KeyNameValid(name) {
		return fmt.Errorf("%q is not a valid key name (letters, digits, . _ -, up to %d; choose one with --name)", name, appdata.KeyNameMax)
	}
	cur, err := conf.Snapshot(s)
	if err != nil {
		return err
	}
	want := map[string][]byte{}
	for p, b := range cur {
		want[p] = b
	}
	want[conf.KeysDir+"/"+name] = data
	if pub, err := os.ReadFile(file + ".pub"); err == nil {
		want[conf.KeysDir+"/"+name+".pub"] = pub
	}
	enc := "not encrypted"
	if info.Encrypted {
		enc = "encrypted: the Flipper asks for the passphrase once"
	}
	fmt.Fprintf(o.out, "%s %s key, %s\n", styleHead.Render(info.Type), info.Format, enc)
	return applyPlan(o, s, conf.Plan(cur, want))
}

func restore(o *options, s store.Store, arg string) error {
	dir, err := conf.ResolveBackup(o.backupDir, arg)
	if err != nil {
		return err
	}
	m, files, err := conf.ReadBackup(dir)
	if err != nil {
		return err
	}
	fmt.Fprintf(o.out, "Backup of %s from %s\n", m.Source, m.Created)
	plan, err := conf.RestorePlan(s, m, files)
	if err != nil {
		return err
	}
	return applyPlan(o, s, plan)
}

// applyPlan shows the changes, asks, backs up, applies.
func applyPlan(o *options, s store.Store, plan []conf.Change) error {
	if len(plan) == 0 {
		fmt.Fprintln(o.out, styleOK.Render("Nothing to change."))
		return nil
	}
	fmt.Fprintln(o.out, styleHead.Render(fmt.Sprintf("%s on %s:", plural.N(len(plan), "change"), s.Describe())))
	for _, ch := range plan {
		fmt.Fprintln(o.out, "  "+changeLine(ch))
	}
	if !o.yes {
		if f, ok := o.in.(*os.File); !ok || !isatty.IsTerminal(f.Fd()) {
			return errors.New("not applied: add --yes to apply without asking")
		}
		fmt.Fprint(o.out, "Apply? [y/N] ")
		line, _ := bufio.NewReader(o.in).ReadString('\n')
		if a := strings.ToLower(strings.TrimSpace(line)); a != "y" && a != "yes" {
			fmt.Fprintln(o.out, "Not applied.")
			return nil
		}
	}
	// the whole state, private keys included: an undo must bring back exactly what was there
	pr := o.progress("Backing up")
	dir, _, err := conf.BackupProgress(s, o.backupDir, true, pr.step)
	pr.done()
	if err != nil {
		return fmt.Errorf("backup before the change failed, nothing applied: %w", err)
	}
	fmt.Fprintln(o.out, styleDim.Render("Backed up first to "+dir))
	pr = o.progress("Writing")
	err = conf.ApplyProgress(s, plan, pr.step)
	pr.done()
	if err != nil {
		return err
	}
	fmt.Fprintln(o.out, styleOK.Render(fmt.Sprintf("Applied %s.", plural.N(len(plan), "change"))))
	prune(o, dir)
	return nil
}

// prune keeps the newest --keep-backups backups; the one just made always stays.
func prune(o *options, made string) {
	gone, err := conf.PruneBackups(o.backupDir, o.keepBackups, made)
	if err != nil {
		fmt.Fprintln(o.out, styleWarn.Render("Old backups not pruned: "+err.Error()))
		return
	}
	if len(gone) > 0 {
		fmt.Fprintln(o.out, styleDim.Render(fmt.Sprintf("Deleted %s; %d kept (--keep-backups).", plural.N(len(gone), "old backup"), o.keepBackups)))
	}
}

func humanSize(n int64) string {
	switch {
	case n >= 1<<20:
		return fmt.Sprintf("%.1f MB", float64(n)/(1<<20))
	case n >= 1<<10:
		return fmt.Sprintf("%.1f KB", float64(n)/(1<<10))
	}
	return fmt.Sprintf("%d B", n)
}

// backups: list, export, import and delete the local backups.
func backups(o *options, params []string) error {
	need := map[string]int{"list": 0, "ids": 0, "export": 2, "import": 1, "delete": 1}
	if len(params) == 0 {
		params = []string{"list"}
	}
	n, ok := need[params[0]]
	if !ok {
		if s := suggest(params[0], backupsSubcommands); s != "" {
			return fmt.Errorf("unknown backups command %q; did you mean %q? (see sshrunner --help)", params[0], "backups "+s)
		}
	}
	if !ok || len(params)-1 != n {
		return errors.New("usage: backups list | export <id> <file> | import <file> | delete <id> (see sshrunner --help)")
	}
	switch params[0] {
	case "ids": // for shell completion: one ID per line, newest first
		list, err := conf.ListBackups(o.backupDir)
		if err != nil {
			return err
		}
		for _, b := range list {
			fmt.Fprintln(o.out, b.ID)
		}
	case "list":
		list, err := conf.ListBackups(o.backupDir)
		if err != nil {
			return err
		}
		if o.asJSON {
			out := backupsJSON{Dir: o.backupDir, Backups: []backupJSON{}}
			for _, b := range list {
				out.Backups = append(out.Backups, backupJSON{ID: b.ID, Created: b.Created.Format(time.RFC3339),
					Source: b.Source, Hosts: b.Hosts, Keys: b.Keys, Files: b.Files, Size: b.Size})
			}
			return writeJSON(o, out)
		}
		fmt.Fprintln(o.out, styleDim.Render(o.backupDir))
		if len(list) == 0 {
			fmt.Fprintln(o.out, "No backups yet.")
			return nil
		}
		rows := [][]string{{"ID", "CREATED", "SOURCE", "HOSTS", "KEYS", "SIZE"}}
		for _, b := range list {
			keys := "no"
			if b.Keys {
				keys = "yes"
			}
			rows = append(rows, []string{b.ID, b.Created.Local().Format(time.DateTime), b.Source,
				strconv.Itoa(b.Hosts), keys, humanSize(b.Size)})
		}
		printTable(o, rows)
	case "export":
		dir, err := conf.ResolveBackup(o.backupDir, params[1])
		if err != nil {
			return err
		}
		m, _, err := conf.ReadBackup(dir)
		if err != nil {
			return err
		}
		if err := conf.ExportBackup(dir, params[2]); err != nil {
			return err
		}
		fmt.Fprintf(o.out, "%s %s to %s\n", styleOK.Render("Exported"), filepath.Base(dir), params[2])
		if m.IncludeKeys {
			fmt.Fprintln(o.out, styleWarn.Render("It holds private keys: keep the file safe."))
		}
	case "import":
		dir, err := conf.ImportBackup(params[1], o.backupDir)
		if err != nil {
			return err
		}
		fmt.Fprintf(o.out, "%s %s (restore it with: sshrunner restore %s)\n", styleOK.Render("Imported"), dir, filepath.Base(dir))
	case "delete":
		if err := conf.DeleteBackup(o.backupDir, params[1]); err != nil {
			return err
		}
		fmt.Fprintf(o.out, "%s %s\n", styleOK.Render("Deleted"), params[1])
	}
	return nil
}

// JSON output: stable field names (snake_case), documented in the README.
type statusJSON struct {
	Source     string            `json:"source"`
	Hosts      int               `json:"hosts"`
	Commands   int               `json:"commands"`
	Keys       int               `json:"keys"`
	PublicOnly int               `json:"keys_public_only"`
	Scripts    int               `json:"scripts"`
	Unreadable map[string]string `json:"unreadable"`
}

type hostJSON struct {
	Alias        string   `json:"alias"`
	User         string   `json:"user"`
	Host         string   `json:"host"`
	Port         int      `json:"port"`
	Auth         string   `json:"auth"`
	Key          string   `json:"key"`
	SavePassword bool     `json:"save_password"`
	Transport    string   `json:"transport"`
	HostKey      string   `json:"host_key_policy"`
	Commands     []string `json:"commands"`
}

type backupJSON struct {
	ID      string `json:"id"`
	Created string `json:"created"`
	Source  string `json:"source"`
	Hosts   int    `json:"hosts"`
	Keys    bool   `json:"keys"`
	Files   int    `json:"files"`
	Size    int64  `json:"size"`
}

type backupsJSON struct {
	Dir     string       `json:"dir"`
	Backups []backupJSON `json:"backups"`
}

func writeJSON(o *options, v any) error {
	e := json.NewEncoder(o.out)
	e.SetIndent("", "  ")
	return e.Encode(v)
}

func printTable(o *options, rows [][]string) {
	w := make([]int, len(rows[0]))
	for _, r := range rows {
		for i, c := range r {
			w[i] = max(w[i], len(c))
		}
	}
	for n, r := range rows {
		var b strings.Builder
		for i, c := range r {
			if i < len(r)-1 {
				c = fmt.Sprintf("%-*s  ", w[i], c)
			}
			b.WriteString(c)
		}
		line := b.String()
		if n == 0 {
			line = styleHead.Render(line)
		}
		fmt.Fprintln(o.out, line)
	}
}

func changeLine(ch conf.Change) string {
	switch ch.Kind {
	case conf.Add:
		return styleOK.Render("+ ") + ch.Path
	case conf.Delete:
		return styleErr.Render("- ") + ch.Path
	}
	return styleWarn.Render("~ ") + ch.Path
}
