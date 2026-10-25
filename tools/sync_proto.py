"""Copy the shared protocol codec into the Flipper app's private library.

uFBT only compiles files below the app directory, so proto/ is mirrored into
flipper-app/lib/proto/. The copy is committed so `ufbt` works on its own;
`--check` (used by tools/check.ps1) fails if the copy has drifted.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import argparse
import filecmp
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "proto"
DST = ROOT / "flipper-app" / "lib" / "proto"
PARTS = ("include", "src")


def files(base: Path) -> set[Path]:
    return {p.relative_to(base) for part in PARTS for p in (base / part).rglob("*") if p.is_file()}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="only verify, do not copy")
    args = ap.parse_args()

    src_files = files(SRC)
    dst_files = files(DST) if DST.exists() else set()
    drift = sorted(
        str(f)
        for f in src_files | dst_files
        if f not in src_files or f not in dst_files or not filecmp.cmp(SRC / f, DST / f, shallow=False)
    )
    if args.check:
        if drift:
            print("flipper-app/lib/proto is out of date; run tools/sync_proto.py:", *drift, sep="\n  ")
            return 1
        print("flipper-app/lib/proto matches proto/")
        return 0

    for f in dst_files - src_files:
        (DST / f).unlink()
    for f in src_files:
        (DST / f).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(SRC / f, DST / f)
    print(f"synced {len(src_files)} files ({len(drift)} changed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
