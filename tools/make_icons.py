"""Generate the Flipper app's 1-bit PNG icons from the ASCII art below.

Run from the repo root:  venv\\Scripts\\python tools\\make_icons.py
'#' = black (drawn) pixel, anything else = white. Every icon follows the stock
10x10 grid unless its name says otherwise.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "flipper-app"

ICONS: dict[str, str] = {
    # App icon (manifest fap_icon): a terminal prompt ">_" in a window.
    "icons/ssh_runner_10px": """
##########
#........#
#.#......#
#..#.....#
#...#....#
#..#.....#
#.#..###.#
#........#
##########
..........
""",
    # Status header (stock status-bar height, 8 rows; odd widths, symmetric). The
    # firmware has no LAN or WiFi glyph (checked: OFW 1.5.1, Unleashed unlshd-094,
    # Momentum, all-the-plugins), so these are drawn here.
    # LAN: an RJ45 socket, solid with its contacts when the link is up, outline when down.
    "images/LanOn_9x8": """
#########
#.#.#.#.#
#########
#########
###...###
..#...#..
..#####..
.........
""",
    # WiFi: three arcs over a dot while associated; no icon otherwise (iOS/Android/Flipper
    # status bars show a link only while it is up). Joining fills it arc by arc.
    # 9x8 like the LAN socket, so the pair reads as one set.
    "images/WifiOn_9x8": """
.#######.
#.......#
..#####..
.#.....#.
...###...
..#...#..
....#....
.........
""",
    # Key icon for the Keys screen and remembered-secret markers.
    "images/Key_10x10": """
..........
.###......
#...#.....
#.#.######
#...#.#.#.
.###......
..........
..........
..........
..........
""",
    # Warning sign of every error screen: the firmware's own status-bar glyph, pixel for
    # pixel (assets/icons/StatusBar/Alert_9x8.png, identical in OFW 1.5.1 and Unleashed
    # unlshd-094, GPL-3.0; THIRD_PARTY.md). FAPs cannot reference firmware icons, so it
    # is copied (a stock warning glyph, the height of a capital).
    # Lock for secured networks in the WiFi list (preview "Bars"): the firmware's own
    # assets/icons/SubGhz/Lock_7x8.png, pixel for pixel (GPL-3.0; THIRD_PARTY.md).
    # The Back button's sign (the Flipper's own: a U-turn arrow), for "Back = stop" hints.
    "images/Back_8x7": """
..#.....
.##.....
#######.
.##....#
..#....#
.......#
..#####.
""",
    "images/Lock_7x8": """
..###..
.#...#.
.#...#.
#######
#######
###.###
#######
.#####.
""",
    "images/Alert_9x8": """
....#....
...###...
...#.#...
..##.##..
..##.##..
.#######.
.###.###.
#########
""",
}

# Busy spinner: eight dots on a ring, a three-dot head that moves one step (45 degrees)
# per frame, the usual 1-bit throbber.
SPINNER_SIZE = 13
SPINNER_STEPS = 8


def spinner_frames() -> list[list[list[int]]]:
    import math

    c, r = (SPINNER_SIZE - 1) / 2, 4.8
    frames = []
    for f in range(SPINNER_STEPS):
        px = [[0] * SPINNER_SIZE for _ in range(SPINNER_SIZE)]
        for k in range(SPINNER_STEPS):
            a = 2 * math.pi * k / SPINNER_STEPS - math.pi / 2  # dot 0 at the top, clockwise
            x, y = c + r * math.cos(a), c + r * math.sin(a)
            head = (f - k) % SPINNER_STEPS < 3
            # head dots 2x2 (3x3 for the leading one), the rest one pixel
            size = 3 if k == f else 2 if head else 1
            x0, y0 = round(x - (size - 1) / 2), round(y - (size - 1) / 2)
            for yy in range(y0, y0 + size):
                for xx in range(x0, x0 + size):
                    if 0 <= xx < SPINNER_SIZE and 0 <= yy < SPINNER_SIZE:
                        px[yy][xx] = 1
        frames.append(px)
    return frames


BUSY_STEPS = 6


def busy_frames() -> list[list[list[int]]]:
    """The small busy sign for a status line, after the terminal's braille spinners: six dots
    in a 2x3 cell (2x2 px each, 1 px apart), four lit, the gap walking clockwise."""
    ring = [(0, 0), (1, 0), (1, 1), (1, 2), (0, 2), (0, 1)]  # cells, clockwise from top left
    frames = []
    for f in range(BUSY_STEPS):
        px = [[0] * 5 for _ in range(8)]
        for k, (cx, cy) in enumerate(ring):
            if (k - f) % BUSY_STEPS < 2:
                continue  # the gap
            for yy in range(cy * 3, cy * 3 + 2):
                for xx in range(cx * 3, cx * 3 + 2):
                    px[yy][xx] = 1
        frames.append(px)
    return frames


def parse(art: str) -> list[list[int]]:
    rows = [r for r in art.strip("\n").splitlines()]
    width = max(len(r) for r in rows)
    return [[1 if (i < len(r) and r[i] == "#") else 0 for i in range(width)] for r in rows]


def write_png(path: Path, pixels: list[list[int]]) -> None:
    h = len(pixels)
    w = len(pixels[0])
    raw = b"".join(b"\x00" + bytes(0 if p else 255 for p in row) for row in pixels)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    old = path.read_bytes() if path.exists() else None
    if old != png:
        path.write_bytes(png)


def main() -> int:
    for name, art in ICONS.items():
        px = parse(art)
        dims = name.rsplit("_", 1)[-1]
        if dims.count("x") == 1 and dims.replace("x", "").isdigit():
            w, h = (int(v) for v in dims.split("x"))
            assert (len(px[0]), len(px)) == (w, h), f"{name}: art is {len(px[0])}x{len(px)}"
        write_png(APP / f"{name}.png", px)

    # Static frames: the progress view draws icons without animating them, so
    # util_progress_tick() steps through the spinner's frames.
    for i, px in enumerate(spinner_frames()):
        write_png(APP / "images" / f"Spinner{i}_{SPINNER_SIZE}x{SPINNER_SIZE}.png", px)
    for i, px in enumerate(busy_frames()):
        write_png(APP / "images" / f"Busy{i}_5x8.png", px)
    print(f"icons written under {APP}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
