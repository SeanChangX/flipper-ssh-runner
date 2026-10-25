// SPDX-License-Identifier: GPL-3.0-only
package cli

import (
	"strings"
	"testing"
)

func TestSuggest(t *testing.T) {
	cases := map[string]string{
		"host":    "hosts", // singular for plural
		"statsu":  "status",
		"stauts":  "status",
		"psuh":    "push",
		"pul":     "pull",
		"restor":  "restore",
		"backpus": "backups",
		"Hosts":   "hosts",
		"x":       "",     // too short to guess
		"deploy":  "",     // nothing close
		"pus":     "push", // one edit from push, two from pull
	}
	for in, want := range cases {
		if got := suggest(in, commands); got != want {
			t.Errorf("%q: %q, want %q", in, got, want)
		}
	}
	if got := suggest("lst", backupsSubcommands); got != "list" {
		t.Errorf("lst: %q", got)
	}
}

func TestUnknownCommandHints(t *testing.T) {
	env(t)
	cases := map[string][]string{
		`unknown command "host"; did you mean "hosts"?`:               {"host"},
		`backup takes no arguments; did you mean "backups list"?`:     {"backup", "list"},
		`unknown backups command "lst"; did you mean "backups list"?`: {"backups", "lst"},
		`unknown command "deploy" (see sshrunner --help)`:             {"deploy"},
	}
	for want, args := range cases {
		_, err := runCLI(t, args...)
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%v: %v, want %q", args, err, want)
		}
	}
}
