"""Convert a Flipper screen frame (128x64, 1 bpp) to a PNG, without Pillow.

Frame layout (u8g2 page buffer, as sent in PB_Gui.ScreenFrame): 8 pages of 128
bytes; byte (page * 128 + x) holds pixels (x, page*8 .. page*8+7), LSB on top.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import struct
import zlib

W, H = 128, 64
ORANGE = (255, 140, 40)  # Flipper LCD backlight look, like qFlipper screenshots
BLACK = (0, 0, 0)


def pixels(frame: bytes) -> list[list[int]]:
    if len(frame) != W * H // 8:
        raise ValueError(f"frame must be 1024 bytes, got {len(frame)}")
    return [[(frame[(y // 8) * W + x] >> (y % 8)) & 1 for x in range(W)] for y in range(H)]


def to_png(frame: bytes, scale: int = 4) -> bytes:
    px = pixels(frame)
    rows = []
    for y in range(H):
        line = bytearray([0])  # filter: none
        for x in range(W):
            line += bytes(BLACK if px[y][x] else ORANGE) * scale
        rows.extend([bytes(line)] * scale)
    raw = b"".join(rows)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", W * scale, H * scale, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def from_png(data: bytes, scale: int = 4) -> list[list[int]]:
    """Pixels (1 = set) of a PNG written by to_png."""
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            w = struct.unpack(">I", body[:4])[0]
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = 1 + w * 3
    out = []
    for y in range(H):
        row = raw[y * scale * stride + 1:(y * scale + 1) * stride]
        out.append([1 if row[x * scale * 3:x * scale * 3 + 3] == bytes(BLACK) else 0 for x in range(W)])
    return out


def to_text(frame: bytes) -> str:
    """ASCII rendering for logs and quick inspection ('#' = pixel set)."""
    return "\n".join("".join("#" if p else "." for p in row) for row in pixels(frame))
