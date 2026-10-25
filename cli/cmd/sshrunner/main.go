// Command sshrunner manages SSH Runner's hosts, keys and scripts on a Flipper Zero.
//
// SPDX-License-Identifier: GPL-3.0-only
package main

import (
	"os"

	"github.com/SeanChangX/flipper-ssh-runner/cli/internal/cli"
)

func main() { os.Exit(cli.Main(os.Args[1:])) }
