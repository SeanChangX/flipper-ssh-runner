"""Key-press planner for SSH Runner's on-screen keyboard (flipper-app/views/kbd.c).

Mirrors flipper-app/views/kbd_layout.c exactly (tools/tests check the table): four rows,
Left/Right wrap within a row, Up/Down pick the key whose centre is nearest (Up from the
top row would enter text-cursor mode, so the planner never uses it), long press = shift.
A UI walk types text by sending these arrow keys.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

from collections import deque

BS, ENTER, SHIFT_KEY = "\b", "\r", "\x0e"  # SHIFT_KEY: the Shift key (the planner uses long presses)
ORIGIN_X, ORIGIN_Y = 1, 29

# (key, x, y) relative to the origin, as in kbd_layout.c
ROWS: list[list[tuple[str, int, int]]] = [
    [("{", 1, 0), ("(", 9, 0), ("[", 17, 0), ("|", 25, 0), ("@", 33, 0), ("&", 41, 0), ("#", 49, 0),
     (";", 57, 0), ("^", 65, 0), ("*", 73, 0), ("`", 81, 0), ('"', 89, 0), ("~", 97, 0), ("'", 105, 0),
     (".", 113, 0), ("/", 120, 0)],
    [("q", 1, 10), ("w", 9, 10), ("e", 17, 10), ("r", 25, 10), ("t", 33, 10), ("y", 41, 10), ("u", 49, 10),
     ("i", 57, 10), ("o", 65, 10), ("p", 73, 10), ("0", 81, 10), ("1", 89, 10), ("2", 97, 10), ("3", 105, 10),
     ("=", 113, 10), ("-", 120, 10)],
    [("a", 1, 21), ("s", 9, 21), ("d", 17, 21), ("f", 25, 21), ("g", 33, 21), ("h", 41, 21), ("j", 49, 21),
     ("k", 57, 21), ("l", 65, 21), (BS, 72, 13), ("4", 89, 21), ("5", 97, 21), ("6", 105, 21), ("$", 113, 21),
     (SHIFT_KEY, 120, 21)],
    [("z", 1, 33), ("x", 9, 33), ("c", 17, 33), ("v", 25, 33), ("b", 33, 33), ("n", 41, 33), ("m", 49, 33),
     ("_", 57, 33), (ENTER, 64, 24), ("7", 89, 33), ("8", 97, 33), ("9", 105, 33), ("!", 113, 33),
     ("+", 120, 33)],
]

SHIFT = {"_": " ", "(": ")", "{": "}", "[": "]", "/": "\\", ";": ":", ".": ",", "!": "?", "&": "<", "|": ">",
         "5": "%"}

START = (1, 0)  # a freshly opened keyboard is on 'q'


def shift(c: str) -> str:
    if "a" <= c <= "z":
        return c.upper()
    return SHIFT.get(c, c)


def key_rect(row: int, col: int) -> tuple[int, int, int, int]:
    k, x, y = ROWS[row][col]
    x, y = ORIGIN_X + x, ORIGIN_Y + y
    if k == BS:
        return x, y, 16, 9
    if k == ENTER:
        return x, y, 24, 11
    return x - 1, y - 8, 7, 10


def on_screen(px: list[list[int]]) -> bool:
    """The keyboard is up: the backspace and save keys' frames (top and bottom edges, corners
    aside) are lit where kbd_layout.c draws them. `px` from screen.pixels()."""
    for key in (BS, ENTER):
        x, y, w, h = key_rect(*find(key))
        for row in (y, y + h - 1):
            if not all(px[row][i] for i in range(x + 2, x + w - 2)):
                return False
    return True


def centre(row: int, col: int) -> int:
    x, _, w, _ = key_rect(row, col)
    return x + w // 2


def nearest(row: int, col: int, to_row: int) -> int:
    cx = centre(row, min(col, len(ROWS[row]) - 1))
    best, best_d = 0, 999
    for c in range(len(ROWS[to_row])):
        d = abs(centre(to_row, c) - cx)
        if d < best_d:
            best, best_d = c, d
    return best


def move(row: int, col: int, key: str) -> tuple[int, int] | None:
    """New (row, col) after an arrow key, as kbd.c; None where the planner must not go."""
    n = len(ROWS[row])
    if key == "left":
        return row, (col - 1) % n
    if key == "right":
        return row, (col + 1) % n
    if key == "up":
        return (row - 1, nearest(row, col, row - 1)) if row > 0 else None
    if key == "down":
        return (row + 1, nearest(row, col, row + 1)) if row + 1 < len(ROWS) else None
    raise ValueError(key)


def path(start: tuple[int, int], target: tuple[int, int]) -> list[str]:
    """Shortest arrow sequence between two keys (BFS)."""
    seen = {start: []}
    q = deque([start])
    while q:
        cur = q.popleft()
        if cur == target:
            return seen[cur]
        for k in ("left", "right", "up", "down"):
            nxt = move(cur[0], cur[1], k)
            if nxt is not None and nxt not in seen:
                seen[nxt] = seen[cur] + [k]
                q.append(nxt)
    raise ValueError("unreachable key")


def find(key: str) -> tuple[int, int]:
    for r, row in enumerate(ROWS):
        for c, (k, _, _) in enumerate(row):
            if k == key:
                return r, c
    raise ValueError(f"no key {key!r}")


def locate(ch: str) -> tuple[int, int, bool]:
    """(row, col, long_press) that types `ch`."""
    for r, row in enumerate(ROWS):
        for c, (k, _, _) in enumerate(row):
            if k == ch and k not in (BS, ENTER, SHIFT_KEY):
                return r, c, False
    for r, row in enumerate(ROWS):
        for c, (k, _, _) in enumerate(row):
            if k not in (BS, ENTER, SHIFT_KEY) and shift(k) == ch:
                return r, c, True
    raise ValueError(f"character {ch!r} is not on the keyboard")


def plan_ex(
    text: str,
    clear_first: bool = False,
    confirm: bool = True,
    start: tuple[int, int] | None = None,
) -> tuple[list[tuple[str, bool]], tuple[int, int]]:
    """Presses (key, long) that type `text` and optionally confirm, plus the cursor
    (row, col) afterwards. With clear_first, a long press on Backspace empties the field."""
    out: list[tuple[str, bool]] = []
    pos = start or START

    def go(target: tuple[int, int]) -> None:
        nonlocal pos
        out.extend((k, False) for k in path(pos, target))
        pos = target

    if clear_first:
        go(find(BS))
        out.append(("ok", True))
    for ch in text:
        r, c, lng = locate(ch)
        go((r, c))
        out.append(("ok", lng))
    if confirm:
        go(find(ENTER))
        out.append(("ok", False))
    return out, pos


def plan(text: str, clear_first: bool = False, confirm: bool = True) -> list[tuple[str, bool]]:
    """plan_ex() from a freshly opened keyboard, presses only."""
    return plan_ex(text, clear_first, confirm)[0]
