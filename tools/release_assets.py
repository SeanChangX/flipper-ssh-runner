"""Collect a release's files and notes from the builds, the same way in CI and locally.

    python tools/release_assets.py check X.Y.Z
    python tools/release_assets.py assets X.Y.Z OUT --fap-official F --fap-unleashed F \\
        --esp32 DIR --cli-dist DIR
    python tools/release_assets.py notes X.Y.Z > notes.md

check: the version is the same in CHANGELOG.md, the firmware and the Flipper app.

assets: writes to OUT the files a release carries, and nothing else:
  ssh_runner-official.fap, ssh_runner-unleashed.fap
  flipper-ssh-runner-esp32-vX.Y.Z.zip   the three firmware parts with flash_args, the licences
                                        and README-flash.txt
  sshrunner_X.Y.Z_*                     the PC tool's archives and packages (GoReleaser)
  SHA256SUMS                            the checksums of all of the above
--esp32 is a firmware build folder (bootloader/, partition_table/, flipper_eth_ssh.bin,
flash_args, sdkconfig) holding LICENSE.esp-idf, LICENSE.lwip, LICENSE.freertos and
LICENSE.mbedtls from the ESP-IDF it was built with. A build that would burn eFuses is refused.

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
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from tools.flash import efuse_burning_options  # noqa: E402

REPO = "https://github.com/SeanChangX/flipper-ssh-runner"
FW_PARTS = ["bootloader/bootloader.bin", "partition_table/partition-table.bin", "flipper_eth_ssh.bin", "flash_args"]
IDF_LICENSES = ["LICENSE.esp-idf", "LICENSE.lwip", "LICENSE.freertos", "LICENSE.mbedtls"]
CLI_KINDS = (".tar.gz", ".zip", ".deb", ".rpm")
ZIP_TIME = (1980, 1, 1, 0, 0, 0)  # every member dated the same: the zip depends on its content only

FLASH_README = """SSH Runner v{ver}: ESP32-S3 firmware (Waveshare ESP32-S3-ETH), development image

Flash the three parts at their own offsets (flash_args lists them):

    0x0      bootloader/bootloader.bin
    0x8000   partition_table/partition-table.bin
    0x20000  flipper_eth_ssh.bin

First install (a new board, or one that ran other firmware): erase the flash first.
Update (the board already runs SSH Runner): do not erase; flashing the three parts keeps the
board's WiFi network, remembered passwords and trusted host keys.

In the browser (desktop Chrome or Edge): https://esptool.spacehuhn.com/
Connect, Erase (first install only), then set the rows (offset after "0x", file with Select)
to 0, 8000 and 20000 with the three files: the page starts with 1000, 8000, E000, 10000, so
change 1000 to 0 and 10000 to 20000 and remove the E000 row. Program. Step by step:
docs/QUICKSTART.md in the repository. Espressif's https://espressif.github.io/esptool-js/
works the same way (Erase Flash on a first install only).

Or with esptool (pip install esptool), from this folder:

    esptool.py --chip esp32s3 -p <port> erase_flash              (first install only)
    esptool.py --chip esp32s3 -p <port> write_flash @flash_args

(<port>: COM5 on Windows, /dev/ttyACM0 on Linux, /dev/cu.usbmodem* on macOS; the board's
own USB-C port.)

If the board is wired for portable use (the Flipper's GPIO pin 9 to the board's 3V3), remove
that wire before you plug in the board's USB-C.

Do NOT merge the parts into one image written at offset 0: that also overwrites the nvs
partition between them and erases the board's WiFi credentials and trusted host keys.

This image burns no eFuses. Settings > Security > Protect Secrets on the Flipper is the only
step that does, and only when you confirm it there.

Licenses of the parts linked into the firmware: LICENSE.* and COPYING.libssh in this archive.
"""

INSTALL = """## Install

**Flipper app.** Download `ssh_runner-official.fap`: it runs on the official firmware and on Unleashed. Copy it to `SD Card/apps/GPIO/` with qFlipper or a card reader and open it from **Apps > GPIO > SSH Runner**. `ssh_runner-unleashed.fap` is optional, for Unleashed only. If the Flipper reports an API mismatch or an outdated app after a firmware update, download the latest release.

**ESP32-S3 board** (Waveshare ESP32-S3-ETH, wired as in [Hardware and Wiring]({doc}docs/HARDWARE.md)): unzip `flipper-ssh-runner-esp32-v{ver}.zip` and flash its three parts at their offsets, in the browser with Spacehuhn's [ESPTool](https://esptool.spacehuhn.com/) (step by step in the [Quick Start]({doc}docs/QUICKSTART.md#in-the-browser-recommended)) or with `esptool.py --chip esp32s3 -p <port> write_flash @flash_args`. Erase the flash first on a first install only; on an update, no Erase and never one merged image at offset 0. Remove the 3V3 wire before connecting the board's USB-C.

**sshrunner, the PC tool** (optional): the archive for your system (`sshrunner_{ver}_<os>_<arch>`), or the `.deb` / `.rpm` on Linux. It edits the hosts, commands, keys and scripts on the Flipper over USB, with a backup first: [PC Tool]({doc}cli/README.md).

Checksums of every file: `SHA256SUMS` (`sha256sum -c SHA256SUMS`). Each file also carries a build provenance attestation (`gh attestation verify <file> --repo SeanChangX/flipper-ssh-runner`).
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
    sec = re.sub(r"^## \[[0-9.]+\](?: - (\S+))?", lambda m: f"Released {m.group(1)}." if m.group(1) else "", sec)
    sec = re.sub(r"\]\(((?:docs|cli|flipper-app|firmware-esp32)/[^)]+)\)", lambda m: f"]({doc}{m.group(1)})", sec)
    return unwrap(sec).strip() + "\n\n" + INSTALL.format(ver=ver, doc=doc)


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


def assets(ver: str, out: Path, a: argparse.Namespace) -> None:
    check(ver)
    esp = Path(a.esp32)
    burning = efuse_burning_options(esp)
    if burning:
        die(f"{esp} enables {' '.join(burning)}: not the development firmware")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    shutil.copyfile(a.fap_official, out / "ssh_runner-official.fap")
    shutil.copyfile(a.fap_unleashed, out / "ssh_runner-unleashed.fap")
    base = f"flipper-ssh-runner-esp32-v{ver}/"
    with zipfile.ZipFile(out / f"flipper-ssh-runner-esp32-v{ver}.zip", "w") as z:
        for part in FW_PARTS + IDF_LICENSES:
            if not (esp / part).is_file():
                die(f"{esp / part} is missing")
            zip_add(z, base + part, (esp / part).read_bytes())
        zip_add(z, base + "COPYING.libssh", (ROOT / "firmware-esp32/components/libssh/COPYING").read_bytes())
        zip_add(z, base + "LICENSE", (ROOT / "LICENSE").read_bytes())
        zip_add(z, base + "THIRD_PARTY.md", (ROOT / "THIRD_PARTY.md").read_bytes())
        zip_add(z, base + "README-flash.txt", FLASH_README.format(ver=ver).encode())
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
    for opt in ("--fap-official", "--fap-unleashed", "--esp32", "--cli-dist"):
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
