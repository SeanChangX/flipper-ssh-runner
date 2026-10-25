// SPDX-License-Identifier: GPL-3.0-only
package appdata

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
)

// Limits and names as in flipper-app/core/profile.h and proto_msg.h.
const (
	AliasMax      = 20
	UserMax       = 32
	HostMax       = 63
	CmdMax        = 127
	HostCmdsMax   = 8
	GlobalCmdsMax = 16
	HostsMax      = 32
	KeyNameMax    = 32
	KeysMax       = 12
	KeyFileMax    = 16 * 1024
	ScriptsMax    = 32
	DefaultPort   = 22

	HostFiletype     = "SSH Runner Host"
	CommandsFiletype = "SSH Runner Commands"
)

// Auth, Transport and Policy values as the files spell them.
var (
	Auths      = []string{"password", "default", "key"}
	Transports = []string{"auto", "lan", "wifi"}
	Policies   = []string{"prompt", "strict", "off"}
)

// Host is one hosts/<alias>.conf. Auth "key" names a file in keys/ (Key); "default" uses
// the key set as default on the Flipper.
type Host struct {
	Alias, User, Host string
	Port              int
	Auth, Key         string
	Remember          bool // the password is kept on the ESP32, never in this file
	Transport, Policy string
	Cmds              []string
}

// NewHost is a host with the app's defaults (profile_init).
func NewHost() Host {
	return Host{Port: DefaultPort, Auth: "password", Transport: "auto", Policy: "prompt"}
}

func isAlnum(c byte) bool {
	return c >= '0' && c <= '9' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z'
}

// AliasValid: 1..20 of [A-Za-z0-9._-], starting alphanumeric, not ending in '.'.
func AliasValid(s string) bool {
	if len(s) == 0 || len(s) > AliasMax || !isAlnum(s[0]) || s[len(s)-1] == '.' {
		return false
	}
	for i := 0; i < len(s); i++ {
		if c := s[i]; !isAlnum(c) && c != '.' && c != '_' && c != '-' {
			return false
		}
	}
	return true
}

// UserValid: 1..32 of [A-Za-z0-9_.$-], not starting with '-'.
func UserValid(s string) bool {
	if len(s) == 0 || len(s) > UserMax || s[0] == '-' {
		return false
	}
	for i := 0; i < len(s); i++ {
		if c := s[i]; !isAlnum(c) && c != '_' && c != '-' && c != '.' && c != '$' {
			return false
		}
	}
	return true
}

// HostValid: a name or IPv4 address of [A-Za-z0-9._-], or an IPv6 literal (with ':', an
// optional %zone), 1..63 characters, not starting with '-' or '.'.
func HostValid(s string) bool {
	if len(s) == 0 || len(s) > HostMax || s[0] == '-' || s[0] == '.' {
		return false
	}
	v6 := strings.ContainsRune(s, ':')
	for i := 0; i < len(s); i++ {
		c := s[i]
		if v6 {
			if !isAlnum(c) && c != ':' && c != '.' && c != '%' {
				return false
			}
		} else if !isAlnum(c) && c != '-' && c != '.' && c != '_' {
			return false
		}
	}
	return true
}

// KeyNameValid: a file name in keys/: 1..32 of [A-Za-z0-9._-], not starting with '.', not
// ending in ".pub".
func KeyNameValid(s string) bool {
	if len(s) == 0 || len(s) > KeyNameMax || s[0] == '.' || strings.HasSuffix(s, ".pub") {
		return false
	}
	for i := 0; i < len(s); i++ {
		if c := s[i]; !isAlnum(c) && c != '.' && c != '_' && c != '-' {
			return false
		}
	}
	return true
}

// CmdValid: 1..127 characters on one line.
func CmdValid(s string) bool {
	return len(s) > 0 && len(s) <= CmdMax && !strings.ContainsAny(s, "\r\n")
}

// ScriptNameValid: a .sh file name of 4..64 characters, no path, not hidden.
func ScriptNameValid(s string) bool {
	return len(s) >= 4 && len(s) <= 64 && !strings.ContainsAny(s, "/\\:") && s[0] != '.' &&
		strings.HasSuffix(s, ".sh")
}

func oneOf(s string, names []string) bool {
	for _, n := range names {
		if s == n {
			return true
		}
	}
	return false
}

// Validate lists what the app would refuse or drop, in the order of the fields.
func (h Host) Validate() error {
	var errs []error
	if !AliasValid(h.Alias) {
		errs = append(errs, fmt.Errorf("alias: 1-%d letters, digits, . _ - (starting with a letter or digit)", AliasMax))
	}
	if !UserValid(h.User) {
		errs = append(errs, fmt.Errorf("user: 1-%d letters, digits, . _ - $", UserMax))
	}
	if !HostValid(h.Host) {
		errs = append(errs, fmt.Errorf("host: an address or name of up to %d characters", HostMax))
	}
	if h.Port < 1 || h.Port > 65535 {
		errs = append(errs, errors.New("port: 1-65535"))
	}
	if !oneOf(h.Auth, Auths) {
		errs = append(errs, fmt.Errorf("auth: one of %s", strings.Join(Auths, ", ")))
	}
	if h.Auth == "key" && !KeyNameValid(h.Key) {
		errs = append(errs, errors.New("key: the name of a file in keys/"))
	}
	if !oneOf(h.Transport, Transports) {
		errs = append(errs, fmt.Errorf("transport: one of %s", strings.Join(Transports, ", ")))
	}
	if !oneOf(h.Policy, Policies) {
		errs = append(errs, fmt.Errorf("host key policy: one of %s", strings.Join(Policies, ", ")))
	}
	if len(h.Cmds) > HostCmdsMax {
		errs = append(errs, fmt.Errorf("commands: at most %d", HostCmdsMax))
	}
	for _, c := range h.Cmds {
		if !CmdValid(c) {
			errs = append(errs, fmt.Errorf("command %q: 1-%d characters on one line", c, CmdMax))
		}
	}
	return errors.Join(errs...)
}

// Text is the file as the app writes it (profile_to_text).
func (h Host) Text() (string, error) {
	if err := h.Validate(); err != nil {
		return "", err
	}
	var w writer
	w.header(HostFiletype)
	w.put("Alias", h.Alias)
	w.put("User", h.User)
	w.put("Host", h.Host)
	w.put("Port", strconv.Itoa(h.Port))
	named := h.Auth == "key" && h.Key != ""
	auth := h.Auth
	if h.Auth == "key" && !named {
		auth = "default"
	}
	w.put("Auth", auth)
	if named {
		w.put("Key", h.Key)
	}
	w.put("Remember_password", map[bool]string{true: "yes", false: "no"}[h.Remember])
	w.put("Transport", h.Transport)
	w.put("Hostkey_policy", h.Policy)
	for _, c := range h.Cmds {
		w.put("Cmd", c)
	}
	return w.b.String(), w.err
}

// ParseHost reads a host file as the app does (profile_from_text): Alias, User and Host must
// be valid and a Port, when given, 1-65535; other values out of range fall back to the
// defaults, invalid commands are dropped. The app takes the alias from the file name.
func ParseHost(text, alias string) (Host, error) {
	k := parseKV(text)
	if err := k.checkHeader(HostFiletype); err != nil {
		return Host{}, err
	}
	h := NewHost()
	a, _ := k.get("Alias")
	u, _ := k.get("User")
	hh, _ := k.get("Host")
	if !AliasValid(a) || !UserValid(u) || !HostValid(hh) {
		return Host{}, errors.New("alias, user or host missing or invalid")
	}
	h.Alias, h.User, h.Host = a, u, hh
	if alias != "" {
		h.Alias = alias // the file name is authoritative
	}
	if p, ok := k.get("Port"); ok {
		v, err := strconv.ParseUint(p, 10, 32)
		if err != nil || v == 0 || v > 65535 {
			return Host{}, fmt.Errorf("port %q", p)
		}
		h.Port = int(v)
	}
	if v, _ := k.get("Auth"); oneOf(v, Auths) {
		h.Auth = v
	}
	if h.Auth == "key" {
		// an old "Auth: key" with no Key (the board's one key) is the default key now
		if key, ok := k.get("Key"); ok && KeyNameValid(key) {
			h.Key = key
		} else {
			h.Auth = "default"
		}
	}
	if v, _ := k.get("Transport"); oneOf(v, Transports) {
		h.Transport = v
	}
	if v, _ := k.get("Hostkey_policy"); oneOf(v, Policies) {
		h.Policy = v
	}
	r, _ := k.get("Remember_password")
	h.Remember = r == "yes"
	cmds, err := checkCmds(k.all("Cmd"), HostCmdsMax)
	if err != nil {
		return Host{}, err
	}
	h.Cmds = cmds
	return h, nil
}

// checkCmds refuses what the app could not run as written, instead of dropping or cutting it:
// a command over CmdMax characters, on several lines, or more than max commands. An empty
// "Cmd:" line holds nothing and is skipped.
func checkCmds(all []string, max int) ([]string, error) {
	var errs []error
	var cmds []string
	for _, c := range all {
		if c != "" {
			cmds = append(cmds, c)
		}
	}
	for i, c := range cmds {
		switch {
		case len(c) > CmdMax:
			errs = append(errs, fmt.Errorf("command %d: %d characters, at most %d", i+1, len(c), CmdMax))
		case !CmdValid(c):
			errs = append(errs, fmt.Errorf("command %d: empty or on several lines", i+1))
		}
	}
	if len(cmds) > max {
		errs = append(errs, fmt.Errorf("%d commands, at most %d", len(cmds), max))
	}
	if len(errs) > 0 {
		return nil, errors.Join(errs...)
	}
	return cmds, nil
}

// CommandsText is commands.conf as the app writes it (commands_to_text).
func CommandsText(cmds []string) (string, error) {
	if len(cmds) > GlobalCmdsMax {
		return "", fmt.Errorf("at most %d global commands", GlobalCmdsMax)
	}
	var w writer
	w.header(CommandsFiletype)
	w.comment("Global saved commands, shown for every host")
	for _, c := range cmds {
		if !CmdValid(c) {
			return "", fmt.Errorf("command %q: 1-%d characters on one line", c, CmdMax)
		}
		w.put("Cmd", c)
	}
	return w.b.String(), w.err
}

// ParseCommands reads commands.conf (commands_from_text).
func ParseCommands(text string) ([]string, error) {
	k := parseKV(text)
	if err := k.checkHeader(CommandsFiletype); err != nil {
		return nil, err
	}
	return checkCmds(k.all("Cmd"), GlobalCmdsMax)
}
