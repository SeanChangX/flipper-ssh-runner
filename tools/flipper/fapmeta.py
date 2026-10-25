"""Read the API version a .fap was built against (ELF section .fapmeta).

FlipperApplicationManifestBase: u32 magic, u32 manifest_version, u16 api_minor,
u16 api_major, u16 hardware_target_id. Used to refuse installing a FAP whose API
major differs from the Flipper's firmware (it would not load).

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import struct


def api_version(fap: bytes) -> tuple[int, int, int]:
    """(api_major, api_minor, hardware_target) from a 32-bit little-endian ELF .fap."""
    if fap[:4] != b"\x7fELF" or fap[4] != 1 or fap[5] != 1:
        raise ValueError("not a 32-bit little-endian ELF")
    shoff, = struct.unpack_from("<I", fap, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", fap, 0x2E)

    def section(i: int) -> tuple[int, int, int]:
        name, _type, _flags, _addr, off, size = struct.unpack_from("<IIIIII", fap, shoff + i * shentsize)
        return name, off, size

    _, str_off, _ = section(shstrndx)
    for i in range(shnum):
        name_off, off, size = section(i)
        end = fap.index(b"\0", str_off + name_off)
        if fap[str_off + name_off : end] == b".fapmeta":
            _magic, _ver, minor, major, target = struct.unpack_from("<IIHHH", fap, off)
            return major, minor, target
    raise ValueError("no .fapmeta section")
