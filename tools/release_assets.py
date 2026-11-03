"""Collect a release's files and notes from the builds, the same way in CI and locally.

    python tools/release_assets.py check X.Y.Z
    python tools/release_assets.py assets X.Y.Z OUT --fap-official F --fap-unleashed F \
        --esp32 DIR --esp32-addon DIR --cli-dist DIR
    python tools/release_assets.py notes X.Y.Z > notes.md

check: the version is the same in CHANGELOG.md, the firmware and the Flipper app.

assets: writes to OUT the files a release carries, and nothing else:
  ssh_runner-official.fap, ssh_runner-unleashed.fap
  flipper-ssh-runner-esp32-waveshare-vX.Y.Z.zip  the firmware for the Waveshare ESP32-S3-ETH
  flipper-ssh-runner-esp32-addon-vX.Y.Z.zip      the firmware for the SSH Runner add-on board
                                              each: README.txt, the factory image (from 0x0, for
                                              a first install), the app image (at 0x20000, for
                                              updates) and licenses/
  sshrunner_X.Y.Z_*                           the PC tool's archives and packages (GoReleaser)
  SHA256SUMS                                  the checksums of all of the above
--esp32 and --esp32-addon are firmware build folders, one per board: flipper_eth_ssh.bin,
flash_args and sdkconfig from the build, factory.bin (`esptool.py merge_bin @flash_args`), and
LICENSE.esp-idf, LICENSE.lwip, LICENSE.freertos and LICENSE.mbedtls from the ESP-IDF it was
built with. A build that would burn eFuses, or one for the other board, is refused.

notes: the version's CHANGELOG section as release notes: one line per paragraph or list item
(GitHub keeps hard line breaks), links to files pointing at the tag, and how to install.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import sys
import textwrap
import zipfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from tools.flash import efuse_burning_options, sdkconfig_values  # noqa: E402

REPO = "https://github.com/SeanChangX/flipper-ssh-runner"
APP = "flipper_eth_ssh.bin"
FACTORY = "factory.bin"
APP_OFFSET = 0x20000  # the factory app partition (firmware-esp32/partitions.csv)
BUILD_FILES = [APP, FACTORY, "flash_args", "sdkconfig"]
IDF_LICENSES = ["LICENSE.esp-idf", "LICENSE.lwip", "LICENSE.freertos", "LICENSE.mbedtls"]
REPO_LICENSES = {  # name in licenses/: file in the repository
    "LICENSE": "LICENSE",
    "COPYING.libssh": "firmware-esp32/components/libssh/COPYING",
    "BSD.libssh": "firmware-esp32/components/libssh/BSD",
}
NOTICES = "THIRD_PARTY_NOTICES.txt"
CLI_KINDS = (".tar.gz", ".zip", ".deb", ".rpm")
ZIP_TIME = (1980, 1, 1, 0, 0, 0)  # every member dated the same: the zip depends on its content only


@dataclass(frozen=True)
class Board:
    arg: str  # the argparse destination of its build folder
    tag: str  # in the file names
    name: str
    kconfig: str  # the board's option in the Kconfig choice FES_BOARD
    usb: str  # what to mind about its USB-C (README.txt)


BOARDS = (
    Board("esp32", "waveshare", "Waveshare ESP32-S3-ETH", "CONFIG_FES_BOARD_WAVESHARE_S3_ETH",
          "Remove the Flipper's 3V3 wire before you plug in the board's USB-C."),
    Board("esp32_addon", "addon", "SSH Runner add-on board, rev 0.1 and later", "CONFIG_FES_BOARD_SSH_RUNNER_ADDON",
          "The board's USB-C may stay plugged in while it sits on the Flipper."),
)

FLASH_README = """SSH Runner v{ver}: ESP32-S3 firmware for the {board}

First install (a new board, or one that ran other firmware): erase the flash, then write
    {factory}  at 0x0
The factory image also covers the settings (NVS) partition: it erases the board's saved WiFi
and trusted host keys, so it is for a first install only.
Update (the board already runs SSH Runner): do not erase, write only
    {app}  at 0x20000
This keeps the board's saved WiFi and trusted host keys.

In the browser (desktop Chrome or Edge): https://esptool.spacehuhn.com/ or
https://espressif.github.io/esptool-js/ : Connect, Erase (first install only), one file at
its offset as above ("Multiple files" off), Program.
With esptool (pip install esptool), from this folder; <port> is e.g. COM5, /dev/ttyACM0 or
/dev/cu.usbmodem*:
    first install:  esptool.py --chip esp32s3 -p <port> erase_flash
                    esptool.py --chip esp32s3 -p <port> write_flash 0x0 {factory}
    update:         esptool.py --chip esp32s3 -p <port> write_flash 0x20000 {app}

{usb} The image burns no eFuses.
Step by step: {doc}docs/QUICKSTART.md#2-flash-the-esp32
Hardware: {doc}docs/HARDWARE.md   Licenses: licenses/
"""

INSTALL = """## Install

**Flipper app.** Download `ssh_runner-official.fap`: it runs on the official firmware and on Unleashed. Copy it to `SD Card/apps/GPIO/` with qFlipper or a card reader and open it from **Apps > GPIO > SSH Runner**. `ssh_runner-unleashed.fap` is optional, for Unleashed only. If the Flipper reports an API mismatch or an outdated app after a firmware update, download the latest release.

**ESP32-S3 board.** Download the zip for your board: `flipper-ssh-runner-esp32-waveshare-v{ver}.zip` for the Waveshare ESP32-S3-ETH (wired as in [Hardware and Wiring]({doc}docs/HARDWARE.md)), `flipper-ssh-runner-esp32-addon-v{ver}.zip` for the SSH Runner add-on board, rev 0.1 and later. On a first install, erase the flash, then write the factory image (`-factory.bin`) at `0x0`. On an update, write only the app image (`-app.bin`) at `0x20000`, without erasing: that keeps the board's saved WiFi and trusted host keys, which the factory image would erase. In the browser with Spacehuhn's [ESPTool](https://esptool.spacehuhn.com/) (desktop Chrome or Edge; step by step in the [Quick Start]({doc}docs/QUICKSTART.md#in-the-browser-recommended)), or with esptool (the commands are in the zip's `README.txt`). On the Waveshare board, remove the 3V3 wire before connecting its USB-C.

**sshrunner, the PC tool** (optional): the archive for your system (`sshrunner_{ver}_<os>_<arch>`), or the `.deb` / `.rpm` on Linux. It edits the hosts, commands, keys and scripts on the Flipper over USB, with a backup first: [PC Tool]({doc}cli/README.md).

Setup from scratch: [Quick Start]({doc}docs/QUICKSTART.md). Checksums of every file: `SHA256SUMS` (`sha256sum -c SHA256SUMS`). Each file also carries a build provenance attestation (`gh attestation verify <file> --repo SeanChangX/flipper-ssh-runner`).
"""


def die(msg: str) -> None:
    sys.exit(f"release_assets: {msg}")


def check(ver: str) -> None:
    mm = ver.rsplit(".", 1)[0]
    rules = [
        ("CHANGELOG.md", f"## [{ver}]"),
        ("firmware-esp32/CMakeLists.txt", f'set(PROJECT_VER "{ver}")'),
        ("flipper-app/application.fam", f'fap_version="{mm}"'),
        ("flipper-app/ssh_runner_i.h", f'define SSH_RUNNER_VERSION "{mm}"'),
    ]
    for file, text in rules:
        if text not in (ROOT / file).read_text(encoding="utf-8"):
            die(f"{file} does not say {text}")


def section(ver: str) -> str:
    ch = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    start = ch.find(f"## [{ver}]")
    if start < 0:
        die(f"no '## [{ver}]' in CHANGELOG.md")
    body = ch[start:]
    nxt = body.find("\n## ", 5)
    body = body[: nxt if nxt > 0 else None]
    return re.split(r"\n\[[^\]]+\]: http", body)[0].strip()


def unwrap(md: str) -> str:
    """Joins the lines of each paragraph and list item into one line."""
    out: list[str] = []
    for line in md.splitlines():
        s = line.strip()
        starts = not s or s.startswith(("#", "- ", "* ", "|", "```")) or re.match(r"\d+\. ", s)
        if out and out[-1] and not out[-1].startswith(("#", "|")) and not starts:
            out[-1] += " " + s
        else:
            out.append(line.rstrip())
    return "\n".join(out)


def notes(ver: str) -> str:
    doc = f"{REPO}/blob/v{ver}/"
    sec = section(ver)
    # no heading or date (the release shows its own); the change kinds at the level of "## Install"
    sec = re.sub(r"^## \[[0-9.]+\].*", "", sec)
    sec = re.sub(r"^### ", "## ", sec, flags=re.M)
    out = unwrap(pin_links(sec, ver)).strip() + "\n\n" + INSTALL.format(ver=ver, doc=doc)
    check_links(out)
    return out


# Link targets in Markdown: inline links and images `](target)`, reference definitions
# `[x]: target`, and the HTML attributes href="target" / src="target".
LINK_TARGETS = (
    re.compile(r"\]\(([^)\s]+)\)"),
    re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(\S+)", re.M),
    re.compile(r"""\b(?:href|src)=["']([^"']+)["']"""),
)
SCHEME = re.compile(r"[a-zA-Z][a-zA-Z0-9+.-]*:")


def pin_links(md: str, ver: str) -> str:
    """Every relative link target as the file at the tag: the release page has no repository
    folder to resolve `docs/...` or `hardware/...` against. Paths are relative to the
    repository's root (CHANGELOG.md sits there); a missing file stops the release."""

    def pin(m: re.Match[str]) -> str:
        target = m.group(1)
        if SCHEME.match(target) or target.startswith("#"):
            return m.group(0)
        path = target.removeprefix("./")
        if not (ROOT / path.split("#", 1)[0]).exists():
            die(f"release notes link to {target}, which is not in the repository")
        start, end = m.span(1)
        whole = m.group(0)
        return whole[: start - m.start()] + f"{REPO}/blob/v{ver}/{path}" + whole[end - m.start():]

    for pattern in LINK_TARGETS:
        md = pattern.sub(pin, md)
    return md


def check_links(md: str) -> None:
    """Fail closed: a release page whose links do not all open somewhere absolute is not made."""
    for pattern in LINK_TARGETS:
        for target in pattern.findall(md):
            if not target.startswith(("https://", "mailto:")):
                die(f"release notes keep a link that does not resolve on the release page: {target}")


def plain(md: str, ver: str) -> str:
    """Markdown inline text as plain text: a link becomes `text <absolute URL>`."""

    def link(m: re.Match[str]) -> str:
        text, url = m.group(1).replace("`", ""), m.group(2)
        if not re.match(r"https?://", url):
            url = f"{REPO}/blob/v{ver}/{url}"  # relative to THIRD_PARTY.md, at the repository's root
        return url if text == url else f"{text} <{url}>"

    return re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, md).replace("`", "").replace("**", "")


def third_party_notices(ver: str) -> str:
    """THIRD_PARTY.md as plain text: no Markdown, every link an absolute URL pinned to the tag."""
    wrap = dict(width=79, break_long_words=False, break_on_hyphens=False)
    out = [f"SSH Runner v{ver}: third-party notices", "",
           f"From {REPO}/blob/v{ver}/THIRD_PARTY.md",
           "The license texts are in this folder.", ""]
    para: list[str] = []
    cols: list[str] = []

    def flush() -> None:
        if para:
            out.extend(textwrap.wrap(plain(" ".join(para), ver), **wrap))
            out.append("")
            para.clear()

    for line in (ROOT / "THIRD_PARTY.md").read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if s.startswith("[←"):
            continue
        if s.startswith("|"):
            flush()
            cells = [c.strip() for c in s.strip("|").split("|")]
            if not cols:
                cols = cells
            elif not all(set(c) <= set("-: ") for c in cells):
                item = textwrap.wrap(plain(cells[0], ver), initial_indent="* ", subsequent_indent="  ", **wrap)
                for col, cell in zip(cols[1:], cells[1:]):
                    if cell:
                        item += textwrap.wrap(plain(f"{col}: {cell}", ver), initial_indent="  ",
                                              subsequent_indent="    ", **wrap)
                out.extend(item)
            continue
        if cols:  # the end of a table
            cols = []
            out.append("")
        m = re.match(r"(#+) (.*)", s)
        if m:
            flush()
            title = plain(m.group(2), ver)
            out += [title, ("=" if len(m.group(1)) == 1 else "-") * len(title), ""]
        elif s:
            para.append(s)
        else:
            flush()
    flush()
    return re.sub(r"\n{3,}", "\n\n", "\n".join(out)).strip() + "\n"


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def zip_add(z: zipfile.ZipFile, name: str, data: bytes) -> None:
    info = zipfile.ZipInfo(name, date_time=ZIP_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    z.writestr(info, data)


def flash_offsets(flash_args: Path) -> dict[str, int]:
    """The files in an idf.py flash_args and their offsets (its first line holds the options)."""
    out = {}
    for line in flash_args.read_text(encoding="utf-8").splitlines()[1:]:
        off, _, file = line.strip().partition(" ")
        if file:
            out[file.strip()] = int(off, 0)
    return out


def check_build(esp: Path, board: Board) -> None:
    for name in BUILD_FILES + IDF_LICENSES:
        if not (esp / name).is_file():
            die(f"{esp / name} is missing")
    burning = efuse_burning_options(esp)
    if burning:
        die(f"{esp} enables {' '.join(burning)}: not the development firmware")
    if sdkconfig_values(esp).get(board.kconfig) != "y":
        die(f"{esp} is not a build for the {board.name} ({board.kconfig} is not set)")
    offsets = flash_offsets(esp / "flash_args")
    if offsets.get("bootloader/bootloader.bin") != 0 or offsets.get(APP) != APP_OFFSET:
        die(f"{esp / 'flash_args'} does not put the bootloader at 0x0 and the app at {APP_OFFSET:#x}")
    factory, app = (esp / FACTORY).read_bytes(), (esp / APP).read_bytes()
    if factory[:1] != b"\xe9" or len(factory) != APP_OFFSET + len(app) or factory[APP_OFFSET:] != app:
        die(f"{esp / FACTORY} is not the bootloader from 0x0 through this app at {APP_OFFSET:#x}")


def firmware_zip(ver: str, out: Path, esp: Path, board: Board) -> None:
    check_build(esp, board)
    stem = f"flipper-ssh-runner-{board.tag}-v{ver}"
    factory, app = f"{stem}-factory.bin", f"{stem}-app.bin"
    zip_name = f"flipper-ssh-runner-esp32-{board.tag}-v{ver}"
    base = f"{zip_name}/"
    readme = FLASH_README.format(
        ver=ver, board=board.name, usb=board.usb, factory=factory, app=app, doc=f"{REPO}/blob/v{ver}/")
    with zipfile.ZipFile(out / f"{zip_name}.zip", "w") as z:
        zip_add(z, base + "README.txt", readme.encode())
        zip_add(z, base + factory, (esp / FACTORY).read_bytes())
        zip_add(z, base + app, (esp / APP).read_bytes())
        for name, src in REPO_LICENSES.items():
            zip_add(z, base + "licenses/" + name, (ROOT / src).read_bytes())
        for name in IDF_LICENSES:
            zip_add(z, base + "licenses/" + name, (esp / name).read_bytes())
        zip_add(z, base + "licenses/" + NOTICES, third_party_notices(ver).encode())


def assets(ver: str, out: Path, a: argparse.Namespace) -> None:
    check(ver)
    for board in BOARDS:  # every build checked before anything is written
        check_build(Path(getattr(a, board.arg)), board)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    shutil.copyfile(a.fap_official, out / "ssh_runner-official.fap")
    shutil.copyfile(a.fap_unleashed, out / "ssh_runner-unleashed.fap")
    for board in BOARDS:
        firmware_zip(ver, out, Path(getattr(a, board.arg)), board)
    cli = sorted(p for p in Path(a.cli_dist).glob(f"sshrunner_{ver}_*") if p.name.endswith(CLI_KINDS))
    if not cli:
        die(f"no sshrunner_{ver}_* archives in {a.cli_dist}")
    for p in cli:
        shutil.copyfile(p, out / p.name)
    files = sorted(p for p in out.iterdir() if p.is_file())
    with open(out / "SHA256SUMS", "w", encoding="ascii", newline="\n") as f:
        for p in files:
            f.write(f"{sha256(p)}  {p.name}\n")
    for p in sorted(out.iterdir()):
        print(f"{p.stat().st_size:>9}  {p.name}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Collect a release's files and notes.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check").add_argument("version")
    n = sub.add_parser("notes")
    n.add_argument("version")
    s = sub.add_parser("assets")
    s.add_argument("version")
    s.add_argument("out")
    for opt in ("--fap-official", "--fap-unleashed", "--esp32", "--esp32-addon", "--cli-dist"):
        s.add_argument(opt, required=True)
    a = ap.parse_args(argv)
    if not re.fullmatch(r"\d+\.\d+\.\d+", a.version):
        die(f"{a.version!r} is not X.Y.Z")
    if a.cmd == "check":
        check(a.version)
    elif a.cmd == "notes":
        sys.stdout.write(notes(a.version))
    else:
        assets(a.version, Path(a.out), a)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
