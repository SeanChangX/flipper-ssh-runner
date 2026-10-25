"""Build the README's hero image and demo GIF from the screenshots in docs/screenshots/.

    venv\\Scripts\\python tools\\readme_media.py        (Windows)
    venv/bin/python tools/readme_media.py              (Linux)
    ... --style white --preview DIR                    (the other surround, written to DIR)

Writes docs/media/hero.png (the core flow as a 2x2 grid, transparent background, so it sits
on GitHub's light and dark themes alike) and docs/media/demo.gif (the same flow, one screen
at a time). Each screen is drawn like the Flipper's screen window: the orange backlight shows
around the 128x64 pixels, the window has rounded corners and sits in a surround: dark grey in
the hero, white with a thin grey edge in the GIF (transparent around it).

Both are built at an integer multiple of the Flipper's pixels (nearest-neighbour), so every
pixel stays a crisp square: the hero at 4x and shown at half its width (2x on the page, 1:1 on
HiDPI screens), the GIF at 3x and shown at its own size.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
SHOTS = ROOT / "docs" / "screenshots"
OUT = ROOT / "docs" / "media"
SHOT_SCALE = 4  # the screenshots are 4x the Flipper's 128x64

# The core flow: Home, Quick Connect, host key, output.
HERO = [
    ("52b-home-quick-connect.png", "1  Home"),
    ("08-quick-connect-typed.png", "2  Quick Connect"),
    ("10-hostkey-new.png", "3  Check the host key"),
    ("14-output-uname.png", "4  Output and exit code"),
]
HERO_SCALE = 4  # 2x2 tiles at 4x (1288 px), shown at half width: crisp on 1x and on HiDPI screens
# The same flow as a walk, one screen per frame (ms on screen).
DEMO = [
    ("52b-home-quick-connect.png", "Home", 1400),
    ("07-quick-connect-empty.png", "Quick Connect", 1200),
    ("08-quick-connect-typed.png", "Type user@host", 1400),
    ("09-connecting.png", "Connecting", 1200),
    ("10-hostkey-new.png", "Check the host key", 1500),
    ("12-password-typed.png", "Password", 1300),
    ("13b-run-menu-type-command.png", "Run a command", 1300),
    ("13c-command-typed.png", "Type the command", 1400),
    ("14-output-uname.png", "Output and exit code", 2800),
]
DEMO_SCALE = 3

# The Flipper's screen window, in 1x display pixels (about 0.28 mm each): the backlit margin
# around the 128x64 pixels, the same on every side, and the corner radius of the orange area.
# Measured on a real Flipper from a straight-on photo rectified to the case's window opening
# (39.4 x 24.2 mm).
WINDOW_MARGIN = 8
WINDOW_MARGINS = (WINDOW_MARGIN,) * 4  # left, top, right, bottom
WINDOW_RADIUS = 7
LCD = (255, 140, 40, 255)  # backlight orange, as in the screenshots' background

# Surround of each screen: "dark" (a grey screen surround) or "white" (the Flipper's face)
STYLES = {
    "dark": {"bezel": (40, 42, 46, 255), "rim": (22, 23, 25, 255), "label": (232, 232, 232, 255), "edge": None},
    "white": {"bezel": (242, 242, 238, 255), "rim": (205, 205, 199, 255), "label": (110, 110, 110, 255),
              "edge": (212, 212, 206, 255)},  # keeps the tile apart from a white page
}
STYLE = "dark"
DEMO_STYLE = "white"  # the demo GIF's surround, whatever --style picks for the hero
# Sizes in 1x Flipper pixels, multiplied by the scale
PAD = 5.5  # surround beside and above the window
RIM = 1.25  # a thin recess line around the window
LABEL_H = 14.5  # surround below the window, holding the label
LABEL_SIZE = 6.5  # label font size
RADIUS = 9  # outer corners of a tile
GAP = 8  # between tiles
MARGIN = 2  # transparent edge of the hero
SS = 4  # supersampling for the frame's curves (the screen pixels are pasted as they are)


def font(size: int) -> ImageFont.ImageFont:
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow built without FreeType
        return ImageFont.load_default()


def screen_at(shot: str, k: int) -> Image.Image:
    """The screenshot at k x the Flipper's pixels: down to 1x, then up, both nearest-neighbour."""
    im = Image.open(SHOTS / shot).convert("RGBA")
    w, h = im.width // SHOT_SCALE, im.height // SHOT_SCALE
    return im.resize((w, h), Image.Resampling.NEAREST).resize((w * k, h * k), Image.Resampling.NEAREST)


def px(v: float, k: int) -> int:
    return max(1, round(v * k))


def tile(shot: str, label: str, k: int, rounded: bool = True,
         style: str | None = None) -> Image.Image:
    st = STYLES[style or STYLE]
    screen = screen_at(shot, k)
    w, h = screen.size
    ml, mt, mr, mb = (v * k for v in WINDOW_MARGINS)
    r = WINDOW_RADIUS * k
    pad, rim, label_h = px(PAD, k), px(RIM, k), px(LABEL_H, k)
    ww, wh = w + ml + mr, h + mt + mb  # the window
    tw, th = ww + 2 * pad, wh + pad + label_h
    # frame shapes drawn large and scaled down, for smooth rounded corners
    big = Image.new("RGBA", (tw * SS, th * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    box = (0, 0, tw * SS - 1, th * SS - 1)
    edge = {"outline": st["edge"], "width": max(1, k // 2) * SS} if st["edge"] else {}
    if rounded:
        d.rounded_rectangle(box, radius=px(RADIUS, k) * SS, fill=st["bezel"], **edge)
    else:
        d.rectangle(box, fill=st["bezel"], **edge)
    x0, y0 = pad * SS, pad * SS
    d.rounded_rectangle((x0 - rim * SS, y0 - rim * SS, x0 + (ww + rim) * SS - 1, y0 + (wh + rim) * SS - 1),
                        radius=(r + rim) * SS, fill=st["rim"])
    d.rounded_rectangle((x0, y0, x0 + ww * SS - 1, y0 + wh * SS - 1), radius=r * SS, fill=LCD)
    t = big.resize((tw, th), Image.Resampling.LANCZOS)
    t.paste(screen, (pad + ml, pad + mt))  # pixel for pixel
    # every label on the same baseline, whatever its letters (no jitter between GIF frames)
    d = ImageDraw.Draw(t)
    f = font(px(LABEL_SIZE, k))
    d.text((tw // 2, pad + wh + round(label_h * 0.68)), label, font=f, fill=st["label"], anchor="ms")
    return t


def hero(out: Path) -> Path:
    k = HERO_SCALE
    tiles = [tile(s, label, k) for s, label in HERO]
    tw, th = tiles[0].size
    gap, margin = GAP * k, MARGIN * k
    img = Image.new("RGBA", (2 * margin + 2 * tw + gap, 2 * margin + 2 * th + gap), (0, 0, 0, 0))
    for i, t in enumerate(tiles):
        img.paste(t, (margin + (i % 2) * (tw + gap), margin + (i // 2) * (th + gap)), t)
    img.save(out, optimize=True)
    return out


def demo(out: Path) -> Path:
    # rounded corners on a transparent page; GIF transparency is all or nothing, so a corner pixel
    # is kept from a quarter coverage up (the grey edge stays whole) and the rest is the key colour
    key = (255, 0, 255)
    frames = []
    for s, label, _ in DEMO:
        t = tile(s, label, DEMO_SCALE, style=DEMO_STYLE)
        f = Image.new("RGB", t.size, key)
        f.paste(t.convert("RGB"), (0, 0), t.getchannel("A").point(lambda v: 255 if v >= 64 else 0))
        frames.append(f)
    # one shared palette for every frame keeps the file small and the colours steady
    sheet = Image.new("RGB", (frames[0].width, frames[0].height * len(frames)))
    for i, f in enumerate(frames):
        sheet.paste(f, (0, i * f.height))
    n = 64
    pal = sheet.quantize(colors=n, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    p = pal.getpalette()[: n * 3]
    clear = min(range(n), key=lambda i: sum((p[3 * i + c] - key[c]) ** 2 for c in range(3)))
    q = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]
    q[0].save(out, save_all=True, append_images=q[1:], duration=[ms for *_, ms in DEMO], loop=0,
              optimize=True, disposal=1, transparency=clear)
    return out


def main(argv: list[str] | None = None) -> int:
    global STYLE
    ap = argparse.ArgumentParser(description="Build the README hero image and demo GIF.")
    ap.add_argument("--style", choices=sorted(STYLES), default=STYLE, help="the surround of each screen")
    ap.add_argument("--preview", metavar="DIR", help="write hero-<style>.png and demo-<style>.gif to DIR instead")
    args = ap.parse_args(argv)
    STYLE = args.style
    if args.preview:
        out = Path(args.preview)
        names = (f"hero-{STYLE}.png", f"demo-{STYLE}.gif")
    else:
        out = OUT
        names = ("hero.png", "demo.gif")
    out.mkdir(parents=True, exist_ok=True)
    for p in (hero(out / names[0]), demo(out / names[1])):
        im = Image.open(p)
        print(f"{p}: {p.stat().st_size} B, {im.size[0]}x{im.size[1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
