"""Build the documentation's figures of sshrunner, the PC tool, from its real output.

    venv\\Scripts\\python tools\\cli_figures.py          (Windows)
    venv/bin/python tools/cli_figures.py                (Linux)
    ... --check                                         (fail if a figure no longer matches)

Everything runs in the pinned `golang` container (the version cli/go.mod names), nothing on
the host: it builds sshrunner, fills a demo data folder (the app's example commands and
script, the hosts of flipper-app/tests/golden/, a key made on the spot), and captures each
screen in a real terminal with its colours: the one-shot commands through `script` (a PTY),
the full-screen editor through tmux (`capture-pane -e`). Charm's freeze then draws each capture
as an SVG terminal window with real text into docs/media/cli/, its font cut down to the glyphs
the figure uses (fontTools, WOFF2). No Flipper, and no data but the demo folder's.

--check captures and draws everything again into .cache/cli-figures/ and compares it with
docs/media/cli/, the backup times and IDs aside (they are the time of the run). Every CLI
screen in the documentation comes from here: never edit an SVG by hand.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "media" / "cli"
WORK = ROOT / ".cache" / "cli-figures"
FREEZE = "github.com/charmbracelet/freeze@v0.2.2"  # MIT; fetched into .cache/go only
# fontTools' subsetter (MIT) and Brotli (MIT, for WOFF2), in a venv inside the container
SUBSET_PIP = ["fonttools==4.67.0", "brotli==1.2.1"]

# Runs in the container on each SVG freeze wrote: freeze embeds the whole JetBrains Mono
# (about 360 KB); keep only the glyphs the figure's text uses, as WOFF2.
SUBSET = r"""
import base64, io, re, sys
import xml.etree.ElementTree as ET
from fontTools import subset
from fontTools.ttLib import TTFont

for path in sys.argv[1:]:
    svg = open(path, encoding="utf-8").read()
    m = re.search(r"(url\(data:[^;]+;charset=utf-8;base64,)([A-Za-z0-9+/=]+)(\))", svg)
    root = ET.fromstring(svg.encode())
    text = "".join(t for el in root.iter() if el.tag.endswith("}text") for t in el.itertext())
    # recalcTimestamp off: the same figure gives the same bytes (--check)
    font = TTFont(io.BytesIO(base64.b64decode(m.group(2))), recalcTimestamp=False)
    opts = subset.Options()
    opts.flavor = "woff2"
    opts.layout_features = ["*"]
    sub = subset.Subsetter(opts)
    # every digit, "-" and ":" too: the backup times change between runs, the font must not
    sub.populate(unicodes=sorted({ord(c) for c in text + " 0123456789-:"}))
    sub.subset(font)
    buf = io.BytesIO()
    font.flavor = "woff2"
    font.save(buf)
    data = base64.b64encode(buf.getvalue()).decode()
    open(path, "w", encoding="utf-8", newline="").write(svg[: m.start(2)] + data + svg[m.end(2) :])
"""

# name: (kind, command line or keys, terminal columns, rows, typed answer)
#   cmd: a command line in the demo home; the answer is typed at its question
#   tui: the editor, after these tmux keys
FIGURES = {
    "status": ("cmd", "sshrunner --dir flipper status", 80, 0, ""),
    "hosts": ("cmd", "sshrunner --dir flipper hosts", 80, 0, ""),
    "push": ("cmd", "sshrunner --dir flipper push edited", 88, 0, "n"),
    "backups-list": ("cmd", "sshrunner --dir flipper backups list", 88, 0, ""),
    "editor-hosts": ("tui", "", 108, 20, ""),
    "editor-backups": ("tui", "5", 134, 20, ""),
}

# The window: freeze's (Charm's) own default colours, from its configurations/base.json:
# background #171717, border #515151, and the "charm" theme's text colour for text without a
# colour of its own; with the window bar, a 1 px border and 8 px corners as in its full.json.
# No drop shadow (it sits badly on GitHub's light page). The tool's own colours are captured as
# they are.
FREEZE_ARGS = [
    "--language", "ansi", "--window", "--border.radius", "8", "--border.width", "1",
    "--border.color", "#515151", "--background", "#171717",
    "--padding", "10,14", "--margin", "0", "--font.size", "14", "--line-height", "1.3",
]

# Runs in the container; $1 is the folder under .cache/cli-figures/ to draw into.
INNER = r"""
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get -qq update >/dev/null && apt-get -qq install -y tmux python3-venv >/dev/null
python3 -m venv /opt/subset
/opt/subset/bin/pip install -q --cache-dir /project/.cache/pip $SUBSET_PIP
GOBIN=/project/.cache/go/bin go install "$FREEZE"
freeze=/project/.cache/go/bin/freeze
ver=$(sed -nE 's/^## \[([0-9]+\.[0-9]+\.[0-9]+)\].*/\1/p' /project/CHANGELOG.md | head -1)
(cd /project/cli && go build -buildvcs=false -o /usr/local/bin/sshrunner \
    -ldflags "-X github.com/SeanChangX/flipper-ssh-runner/cli/internal/version.Version=$ver" ./cmd/sshrunner)

# the demo home: no user name in any path the tool prints. Inside tmux (and so with its TERM)
# the colour library asks the terminal nothing, so no query takes a typed answer.
export HOME=/home/user TERM=tmux-256color COLORTERM=truecolor TZ=UTC
mkdir -p "$HOME" && cd "$HOME"
mkdir -p flipper/hosts flipper/keys flipper/scripts
golden=/project/flipper-app/tests/golden examples=/project/flipper-app/files/examples
cp "$golden/host_key.conf" flipper/hosts/nas.conf
cp "$golden/host_password.conf" flipper/hosts/pi.conf
cp "$golden/host_default.conf" flipper/hosts/v6.conf
cp "$examples/commands.conf" flipper/commands.conf
cp "$examples/scripts/"*.sh flipper/scripts/
ssh-keygen -q -t ed25519 -N "" -C "" -f flipper/keys/id_ed25519
# a copy with changes, for the push figure: one host edited, one added, one gone
cp -r flipper edited
sed -i 's/^Port: 2222$/Port: 22/' edited/hosts/nas.conf
sed -e 's/^Alias: pi$/Alias: web/' -e 's/^Host: raspberrypi.local$/Host: web.example.com/' \
    flipper/hosts/pi.conf > edited/hosts/web.conf
rm edited/hosts/v6.conf
# two backups, a second apart (their IDs are the time)
sshrunner --dir flipper backup >/dev/null
sleep 1.2
sshrunner --dir flipper backup --no-keys >/dev/null

out=/project/.cache/cli-figures/$1
rm -rf "$out" && mkdir -p "$out"
# the PTY's carriage returns and the empty lines at the start and the end; and "default
# background" (SGR 49, which freeze 0.2.2 ignores, so a background ran on to the end of the
# line) spelled as the window's own background colour, which is what it shows
clean() {
    sed -e 's/\r$//' -e 's/\x1b\[49m/\x1b[48;2;23;23;23m/g' | sed -e '/[^[:space:]]/,$!d' |
        sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'
}
prompt=$(printf '\x1b[38;5;244m$\x1b[0m')
exec 3< /project/.cache/cli-figures/figures.txt
while IFS='|' read -r name kind run cols rows answer <&3; do
    if [ "$kind" = cmd ]; then
        # the answer reaches the PTY as keys: the terminal echoes it after the question
        { echo "$prompt $run"
          { sleep 2; [ -z "$answer" ] || echo "$answer"; } |
              script -qec "stty cols $cols rows 40; $run" /dev/null; } | clean > "$out/$name.ansi"
    else
        tmux new-session -d -s fig -x "$cols" -y "$rows" "sshrunner --dir flipper"
        sleep 2
        [ -z "$run" ] || { tmux send-keys -t fig "$run"; sleep 1; }
        tmux capture-pane -e -p -t fig | clean > "$out/$name.ansi"
        tmux kill-server
    fi
    "$freeze" $FREEZE_ARGS --output "$out/$name.svg" "$out/$name.ansi" > /dev/null
done
/opt/subset/bin/python -c "$SUBSET" "$out"/*.svg
"""

# Backup times and IDs are the time of the run: equal for --check whatever the time.
TIME = re.compile(rb"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d|sshrunner-\d{8}-\d{6}")


def capture(target: str) -> Path:
    if not shutil.which("docker"):
        sys.exit("docker is not available: the figures are made in the golang container")
    gover = re.search(r"^go (\S+)$", (ROOT / "cli" / "go.mod").read_text(), re.M).group(1)
    WORK.mkdir(parents=True, exist_ok=True)
    rows = "".join("|".join(map(str, (n, *f))) + "\n" for n, f in FIGURES.items())
    (WORK / "figures.txt").write_bytes(rows.encode())
    env = {"GOPATH": "/project/.cache/go", "GOMODCACHE": "/project/.cache/go/mod",
           "GOCACHE": "/project/.cache/go/build", "GOFLAGS": "-modcacherw", "GOTOOLCHAIN": "local",
           "CGO_ENABLED": "0", "FREEZE": FREEZE, "FREEZE_ARGS": " ".join(FREEZE_ARGS),
           "SUBSET": SUBSET, "SUBSET_PIP": " ".join(SUBSET_PIP)}
    cmd = ["docker", "run", "--rm", "-v", f"{ROOT}:/project", "-w", "/project"]
    for k, v in env.items():
        cmd += ["-e", f"{k}={v}"]
    cmd += [f"golang:{gover}", "bash", "-c", INNER, "inner", target]
    subprocess.run(cmd, check=True)
    return WORK / target


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Build the figures of sshrunner from its real output.")
    ap.add_argument("--check", action="store_true", help="fail if a figure no longer matches the CLI")
    args = ap.parse_args(argv)
    fresh = capture("check" if args.check else "out")
    stale = []
    for name in FIGURES:
        svg = fresh / f"{name}.svg"
        if args.check:
            old = OUT / f"{name}.svg"
            if not old.exists() or TIME.sub(b"T", old.read_bytes()) != TIME.sub(b"T", svg.read_bytes()):
                stale.append(name)
            continue
        OUT.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(svg, OUT / f"{name}.svg")
        print(f"{(OUT / f'{name}.svg').relative_to(ROOT)}: {svg.stat().st_size} B")
    if stale:
        print("out of date, run tools/cli_figures.py: " + ", ".join(stale), file=sys.stderr)
        return 1
    if args.check:
        print(f"{len(FIGURES)} figures match the CLI")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
