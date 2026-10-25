""""Protect secrets" firmware test on the board, VIRTUAL eFuses only.

    venv\\Scripts\\python tools\\hil\\sec_matrix.py [--port COM8] [--entries 10]

Flashes the virtual-eFuse test build (tools/flash.py --efuse-test, which refuses anything
else), then over the ESP32 console (`sec`, `sectest`, only in that build):

  words     wrong or empty words never reach the burn function (burn calls stay 0, tier 0)
  fail      the key derivation fails after the burn: "Protection failed; your data is
            unchanged", plaintext keystore intact, readable and writable, tier 0
  plain     the opt-in without interruption: every seeded entry readable afterwards, tier 1
  point N   a power loss (restart) at migration point N = 1 (after the burn), 2 (encrypted
            partition cleared), 3 (half copied), 4 (marker written): the next boot resumes,
            every entry readable, tier 1, plaintext copy gone
  again     enabling twice is a no-op ("Already protected", no second burn)

Before each case the test partitions (fes_sec, nvs_test, efuse_em) are erased, so the
emulated eFuses start again as a copy of the real ones; the board's own nvs is never touched.
Run `espefuse summary` before and after (read-only) to show the real eFuses did not change.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[2]
PY = sys.executable
LOG = ROOT / ".cache" / "hil" / "sec_matrix.log"


class Console:
    def __init__(self, port: str):
        self.port = port
        self.s: serial.Serial | None = None

    def open(self, wait_boot: float = 0.0) -> None:
        deadline = time.monotonic() + 20
        while True:
            try:
                s = serial.Serial()
                s.port, s.baudrate, s.timeout = self.port, 115200, 0.2
                s.dtr = s.rts = False  # never reset the chip from here
                s.open()
                self.s = s
                break
            except serial.SerialException:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.5)
        time.sleep(wait_boot)
        self.s.reset_input_buffer()

    def close(self) -> None:
        if self.s:
            self.s.close()
            self.s = None

    def cmd(self, line: str, wait: float = 1.5) -> str:
        assert self.s
        self.s.write((line + "\r\n").encode())
        end, out = time.monotonic() + wait, b""
        while time.monotonic() < end:
            out += self.s.read(4096)
        text = out.decode("utf-8", "replace").replace("\r", "")
        log(f">>> {line}\n{text}")
        return text

    def after_restart(self) -> None:
        """The command restarted the ESP32: USB re-enumerates, then the boot resumes."""
        self.close()
        time.sleep(2.0)
        self.open(wait_boot=6.0)


def log(text: str) -> None:
    LOG.parent.mkdir(parents=True, exist_ok=True)
    with LOG.open("a", encoding="utf-8") as f:
        f.write(text + "\n")


def sec(c: Console) -> dict:
    out = c.cmd("sec")
    m = re.search(r"sec: tier (\d+), key block (?:none )?(\d+), (\d+) free key blocks, (\d+) keystore entries "
                  r"\((\d+) plaintext\), burn calls (\d+)( \(VIRTUAL eFuses\))?", out)
    if not m:
        raise SystemExit(f"no `sec` answer:\n{out}")
    t, blk, free, n, plain, burns, virt = m.groups()
    return {"tier": int(t), "block": int(blk), "free": int(free), "entries": int(n), "plain": int(plain),
            "burns": int(burns), "virtual": bool(virt)}


def erase_test_parts(c: Console, port: str) -> None:
    c.close()
    rc = subprocess.call([PY, str(ROOT / "tools" / "flash.py"), "erase-test-parts", "--port", port],
                         stdout=subprocess.DEVNULL)
    if rc:
        raise SystemExit("erase-test-parts failed")
    time.sleep(2.0)
    c.open(wait_boot=6.0)


def fresh(c: Console, port: str, entries: int) -> dict:
    erase_test_parts(c, port)
    s = sec(c)
    if not s["virtual"]:
        raise SystemExit("the board does not run the virtual-eFuse build: stopping before anything else")
    if s["tier"] != 0 or s["entries"] != 0:
        raise SystemExit(f"not a clean start: {s}")
    out = c.cmd(f"sectest seed {entries}", wait=3.0)
    if f"seed {entries}/{entries}" not in out:
        raise SystemExit(f"seeding failed:\n{out}")
    return sec(c)


def check_protected(c: Console, entries: int, seeded: int) -> tuple[bool, str]:
    s = sec(c)
    v = c.cmd(f"sectest verify {entries}", wait=3.0)
    ok = (s["tier"] == 1 and s["entries"] == seeded and s["plain"] == 0 and f"verify {entries}/{entries}" in v)
    return ok, f"tier {s['tier']}, key block {s['block']}, {s['entries']}/{seeded} entries, " \
               f"{s['plain']} plaintext left, verify " + (
        "ok" if f"verify {entries}/{entries}" in v else "FAILED")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--entries", type=int, default=10)
    ap.add_argument("--no-flash", action="store_true", help="the test build is already on the board")
    args = ap.parse_args()
    LOG.unlink(missing_ok=True)
    if not args.no_flash:
        rc = subprocess.call([PY, str(ROOT / "tools" / "flash.py"), "flash", "--port", args.port, "--efuse-test"])
        if rc:
            raise SystemExit("flashing the virtual-eFuse test build failed (or was refused)")
        time.sleep(2.0)
    c = Console(args.port)
    c.open(wait_boot=6.0)
    results: list[tuple[str, bool, str]] = []

    # words: the inner gate, called directly (the link decoder is the outer one, host-tested)
    seeded = fresh(c, args.port, args.entries)["entries"]
    bad = ["conf", "confirm.", "confirmx", "con_firm", "yes", "CONFIRMED"]
    burns = 0
    for w in bad:
        out = c.cmd(f"sectest word {w}")
        burns += 0 if "burn calls +0" in out and "ESP_ERR_INVALID_ARG" in out else 1
    s = sec(c)
    results.append(("words", burns == 0 and s["burns"] == 0 and s["tier"] == 0 and s["entries"] == seeded,
                    f"{len(bad)} wrong words: burn calls {s['burns']}, tier {s['tier']}, {s['free']} free key blocks"))

    # the key derivation fails after the burn (what only a real burn could show): nothing moved
    seeded = fresh(c, args.port, args.entries)["entries"]
    out = c.cmd("sectest enable fail", wait=3.0)
    s = sec(c)
    v = c.cmd(f"sectest verify {args.entries}", wait=3.0)
    w = c.cmd(f"sectest seed {args.entries}", wait=3.0)  # the keystore still takes writes
    ok = ("Protection failed; your data is unchanged." in out and s["tier"] == 0 and s["burns"] == 1
          and s["plain"] == seeded and f"verify {args.entries}/{args.entries}" in v
          and f"seed {args.entries}/{args.entries}" in w)
    results.append(("fail", ok, f"derivation failed after the burn: tier {s['tier']}, {s['plain']}/{seeded} "
                                f"plaintext entries kept, readable and writable, message shown"))

    # plain run, then enabling again
    seeded = fresh(c, args.port, args.entries)["entries"]
    c.cmd("sectest enable", wait=4.0)
    c.after_restart()
    ok, why = check_protected(c, args.entries, seeded)
    results.append(("plain", ok, why))
    out = c.cmd("sectest enable", wait=2.0)
    s = sec(c)
    results.append(("again", "Already protected" in out and s["burns"] == 0 and s["tier"] == 1,
                    f"second enable: burn calls {s['burns']} this boot, tier {s['tier']}"))

    for point in (1, 2, 3, 4):
        seeded = fresh(c, args.port, args.entries)["entries"]
        c.cmd(f"sectest enable {point}", wait=4.0)  # restarts at the point
        c.after_restart()
        ok, why = check_protected(c, args.entries, seeded)
        results.append((f"point {point}", ok, why))

    erase_test_parts(c, args.port)  # leave no emulated key or test data behind
    c.close()
    print("\nD16 firmware test (virtual eFuses):")
    for name, ok, why in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name:<8} {why}")
    print(f"log: {LOG.relative_to(ROOT)}")
    return 0 if all(ok for _, ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
