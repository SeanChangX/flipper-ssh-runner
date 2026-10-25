"""Check the render test's fake GUI against the device: every text it predicts for a
screenshot must have its inked box exactly where the device drew it.

The host render test (flipper-app/tests/test_render.c) writes render_frames.txt with the
draw operations of each frame named like a walk screenshot. For every text there whose
screenshot exists in docs/screenshots/, this compares the predicted ink box with the
device pixels: each edge row/column of the box has ink, and the ring just outside it has
none (where no other predicted shape is). Stock module layouts in the fake are a re-draw
of the firmware's; this is how they are verified.

    python -m tools.flipper.render_calibrate [render_frames.txt] [screenshots dir]

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import sys
from pathlib import Path

from .screen import H, W, from_png

ROOT = Path(__file__).resolve().parents[2]


def parse(path: Path) -> dict[str, list[tuple]]:
    frames: dict[str, list[tuple]] = {}
    cur = None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("frame "):
            cur = frames.setdefault(line.split()[1], [])
        elif cur is not None and line.startswith("  "):
            parts = line.split(" ", 9)
            kind, x, y, w, h, color, font = parts[2], *map(int, parts[3:9])
            cur.append((kind, x, y, w, h, color, font, parts[9] if len(parts) > 9 else ""))
    return frames


def check_text(px, op, others) -> str | None:
    kind, x, y, w, h, color, _font, text = op
    if color == 2:
        return None  # XOR: ink colour depends on what is under it
    ink = 1 if color == 1 else 0

    def at(cx: int, cy: int) -> int | None:
        return px[cy][cx] if 0 <= cx < W and 0 <= cy < H else None

    def covered(cx: int, cy: int) -> bool:
        for o in others:
            if o is op:
                continue
            ok, ox, oy, ow, oh = o[0], o[1], o[2], o[3], o[4]
            if ok in ("text", "glyph", "icon", "line", "frame") and ox - 1 <= cx <= ox + ow and oy - 1 <= cy <= oy + oh:
                return True
            if ok == "box" and color == 1 and ox <= cx < ox + ow and oy <= cy < oy + oh:
                return True
        return False

    edges = {
        "top": [(cx, y) for cx in range(x, x + w)],
        "bottom": [(cx, y + h - 1) for cx in range(x, x + w)],
        "left": [(x, cy) for cy in range(y, y + h)],
        "right": [(x + w - 1, cy) for cy in range(y, y + h)],
    }
    for name, pts in edges.items():
        if all(at(cx, cy) is None for cx, cy in pts):
            continue  # this edge is off the screen (a title row the display cannot show)
        if not any(at(cx, cy) == ink for cx, cy in pts):
            return f"no ink on the {name} edge"
    ring = [(cx, y - 1) for cx in range(x, x + w)] + [(cx, y + h) for cx in range(x, x + w)]
    ring += [(x - 1, cy) for cy in range(y, y + h)] + [(x + w, cy) for cy in range(y, y + h)]

    def in_black_box(cx: int, cy: int) -> bool:
        return any(o[0] == "box" and o[5] == 1 and o[1] <= cx < o[1] + o[3] and o[2] <= cy < o[2] + o[4] for o in others)

    for cx, cy in ring:
        if ink == 0 and not in_black_box(cx, cy):
            continue  # white text: outside its black box the screen is white anyway
        if at(cx, cy) == ink and not covered(cx, cy):
            return f"ink outside the box at {cx},{cy}"
    return None


def main(argv: list[str]) -> int:
    frames_path = Path(argv[0]) if argv else ROOT / ".cache" / "build" / "flipper-app" / "render_frames.txt"
    shots = Path(argv[1]) if len(argv) > 1 else ROOT / "docs" / "screenshots"
    frames = parse(frames_path)
    checked = bad = 0
    for name, ops in frames.items():
        png = shots / f"{name}.png"
        if not png.exists():
            continue
        px = from_png(png.read_bytes())
        for op in ops:
            if op[0] != "text":
                continue
            checked += 1
            err = check_text(px, op, ops)
            if err:
                bad += 1
                print(f"{name}: \"{op[7]}\" at {op[1]},{op[2]} {op[3]}x{op[4]}: {err}")
    print(f"calibration: {checked} texts compared, {bad} differ")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
