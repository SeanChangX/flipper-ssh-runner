// SPDX-License-Identifier: GPL-3.0-only
package cli

import (
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"github.com/mattn/go-isatty"
)

// progress shows "Writing [12/30] hosts/nas.conf" on one line of stderr while a long step
// runs, only when stderr is a terminal (scripts and --json see nothing), then clears it:
// the command's own summary line follows.
type progress struct {
	w     io.Writer
	what  string
	on    bool
	last  time.Time
	shown bool
}

var spin = []string{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}

func (o *options) progress(what string) *progress {
	on := false
	if f, ok := o.err.(*os.File); ok && !o.asJSON {
		on = isatty.IsTerminal(f.Fd()) || isatty.IsCygwinTerminal(f.Fd())
	}
	return &progress{w: o.err, what: what, on: on}
}

// step is called before each file; it redraws at most every 50 ms, and always for the last.
func (p *progress) step(i, n int, path string) {
	if !p.on || (i < n && time.Since(p.last) < 50*time.Millisecond) {
		return
	}
	p.last = time.Now()
	line := fmt.Sprintf("%s %s [%d/%d] %s", spin[i%len(spin)], p.what, i, n, path)
	if len(line) > 78 {
		line = line[:77] + "…"
	}
	fmt.Fprintf(p.w, "\r%-79s", line)
	p.shown = true
}

func (p *progress) done() {
	if p.shown {
		fmt.Fprintf(p.w, "\r%s\r", strings.Repeat(" ", 79))
		p.shown = false
	}
}
