"""Flash the ESP32 firmware from Windows with esptool from the project venv.

Docker on Windows cannot see COM ports: the firmware is built in the
container (tools/idf.sh build, or tools/check.ps1) into .cache/build/esp32/, and this
script flashes that build tree from the host.

    venv\\Scripts\\python tools\\flash.py flash --port COMx
    venv\\Scripts\\python tools\\flash.py flash --port COMx --build path\\to\\build
    venv\\Scripts\\python tools\\flash.py monitor --port COMx
    venv\\Scripts\\python tools\\flash.py flash --port COMx --efuse-test
    venv\\Scripts\\python tools\\flash.py erase-test-parts --port COMx

Safety:
  * This talks to the ESP32 board only; it never touches the Flipper: a port whose USB ID
    is a Flipper's (0483:5740) is refused.
  * A build whose sdkconfig enables flash encryption, NVS encryption, Secure Boot or any
    other eFuse-burning option is refused (it would burn eFuses irreversibly on first
    boot). Only an explicit --i-understand-this-burns-efuses overrides that.
  * --efuse-test flashes the virtual-eFuse test build (.cache/build/esp32-efusetest) and
    only if its sdkconfig proves CONFIG_EFUSE_VIRTUAL and CONFIG_EFUSE_VIRTUAL_KEEP_IN_FLASH
    are on and every eFuse-burning option above is off: its "Protect secrets" tests then
    write emulated eFuses in flash, never the chip's.
  * erase-test-parts erases only the partitions that test build uses (fes_sec, nvs_test,
    efuse_em); the board's own nvs (WiFi credentials, pinned keys) is never touched.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / ".cache" / "build" / "esp32"
EFUSE_TEST_BUILD = ROOT / ".cache" / "build" / "esp32-efusetest"
PARTITIONS = ROOT / "firmware-esp32" / "partitions.csv"
TEST_PARTS = ("fes_sec", "nvs_test", "efuse_em")

FLIPPER_VID_PID = (0x0483, 0x5740)

# sdkconfig options that make the bootloader or app burn eFuses on first boot.
EFUSE_BURNING = (
    "CONFIG_SECURE_FLASH_ENC_ENABLED",
    "CONFIG_SECURE_BOOT",
    "CONFIG_SECURE_BOOT_V2_ENABLED",
    "CONFIG_NVS_ENCRYPTION",
    "CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC",
    "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE",
    "CONFIG_SECURE_DISABLE_ROM_DL_MODE",
)


def _same_port(a: str, b: str) -> bool:
    """COM8 == com8 on Windows; on Linux a /dev/serial/by-id/... link is the /dev/ttyACMn it
    points to (the port list names only the latter)."""
    if os.name != "nt":
        a, b = os.path.realpath(a), os.path.realpath(b)
    return a.lower() == b.lower()


def guard_port(port: str) -> None:
    from serial.tools import list_ports

    for p in list_ports.comports():
        if _same_port(p.device, port) and (p.vid, p.pid) == FLIPPER_VID_PID:
            raise SystemExit(f"{port} is a Flipper Zero, not the ESP32; refusing: "
                             "this tool never writes to the Flipper")


def efuse_burning_options(build: Path) -> list[str]:
    cfg = build / "sdkconfig"
    if not cfg.exists():
        raise SystemExit(f"{cfg} not found: cannot check the build for eFuse-burning options; refusing")
    found = []
    for line in cfg.read_text(encoding="utf-8", errors="replace").splitlines():
        key, _, val = line.partition("=")
        if key.strip() in EFUSE_BURNING and val.strip() == "y":
            found.append(key.strip())
    return found


def sdkconfig_values(build: Path) -> dict[str, str]:
    cfg = build / "sdkconfig"
    if not cfg.exists():
        raise SystemExit(f"{cfg} not found: cannot check the build; refusing")
    out = {}
    for line in cfg.read_text(encoding="utf-8", errors="replace").splitlines():
        key, eq, val = line.partition("=")
        if eq and not key.startswith("#"):
            out[key.strip()] = val.strip()
    return out


def efuse_test_problems(build: Path) -> list[str]:
    """Why `build` is not a safe virtual-eFuse test image ([] when it is)."""
    v = sdkconfig_values(build)
    problems = [f"{k} is not set" for k in ("CONFIG_EFUSE_VIRTUAL", "CONFIG_EFUSE_VIRTUAL_KEEP_IN_FLASH")
                if v.get(k) != "y"]
    problems += [f"{k}=y" for k in EFUSE_BURNING if v.get(k) == "y"]
    return problems


def test_partitions(csv: Path = PARTITIONS) -> list[tuple[str, int, int]]:
    """(name, offset, size) of the test build's partitions, from partitions.csv."""
    out = []
    for line in csv.read_text(encoding="utf-8").splitlines():
        f = [x.strip() for x in line.split(",")]
        if len(f) >= 5 and f[0] in TEST_PARTS:
            out.append((f[0], int(f[3], 0), int(f[4], 0)))
    if sorted(n for n, _, _ in out) != sorted(TEST_PARTS):
        raise SystemExit(f"{csv}: expected partitions {', '.join(TEST_PARTS)}")
    for name, off, size in out:
        if off < 0x320000:  # never below the app: nvs, phy_init and the app itself live there
            raise SystemExit(f"{name} at {off:#x} overlaps the board's own partitions; refusing")
    return out


def cmd_erase_test_parts(args) -> int:
    for name, off, size in test_partitions():
        print(f"erasing {name} ({size // 1024} KiB at {off:#x})")
        rc = subprocess.call([sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", args.port,
                              "--after", "hard-reset", "erase-region", hex(off), hex(size)])
        if rc:
            return rc
    return 0


def cmd_flash(args) -> int:
    build = Path(args.build).resolve() if args.build else (EFUSE_TEST_BUILD if args.efuse_test else BUILD)
    if not (build / "flash_args").exists():
        raise SystemExit(f"no build in {build}; run tools/idf.sh build first")
    if args.efuse_test:
        problems = efuse_test_problems(build)
        if problems:
            raise SystemExit("refusing: not a virtual-eFuse test build (" + "; ".join(problems) + ")")
        print("virtual-eFuse test build: CONFIG_EFUSE_VIRTUAL and KEEP_IN_FLASH on, no burning option set")
    burning = efuse_burning_options(build)
    if burning and not args.i_understand_this_burns_efuses:
        raise SystemExit(
            "refusing: this build burns eFuses irreversibly on first boot (" + ", ".join(burning) + ").\n"
            "Flash the dev build (.cache/build/esp32) instead.")
    # flash_args lists paths relative to the build directory.
    return subprocess.call(
        [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", args.port, "-b", str(args.baud),
         "--before", "default_reset", "--after", "hard_reset", "write_flash", "@flash_args"],
        cwd=build,
    )


def cmd_monitor(args) -> int:
    return subprocess.call([sys.executable, "-m", "serial.tools.miniterm", args.port, "115200", "--raw"])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("flash")
    f.add_argument("--port", required=True)
    f.add_argument("--baud", type=int, default=460800)
    f.add_argument("--build", help="build directory (default .cache/build/esp32, the dev build)")
    f.add_argument("--efuse-test", action="store_true",
                   help="flash the virtual-eFuse test build (checked to be virtual)")
    f.add_argument("--i-understand-this-burns-efuses", action="store_true", help=argparse.SUPPRESS)
    e = sub.add_parser("erase-test-parts", help="erase fes_sec, nvs_test and efuse_em only")
    e.add_argument("--port", required=True)
    m = sub.add_parser("monitor")
    m.add_argument("--port", required=True)
    args = ap.parse_args()
    guard_port(args.port)
    return {"flash": cmd_flash, "monitor": cmd_monitor, "erase-test-parts": cmd_erase_test_parts}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
