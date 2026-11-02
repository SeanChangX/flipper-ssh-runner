"""Numeric fit check of the board on the Flipper, from Flipper Devices' own STEP files.

    hardware/tools/kicad.sh python3 tools/fit_check.py

Run after tools/place.py (it reads the fit-check copy in .cache/hardware/fit/).

Flipper's body STEP and its reference module STEP with headers share one assembly frame, in
which the reference module is plugged in. place.FLIPPER_FRAME puts both on our board with the
same transform. This script exports our headers, the reference module and the body as meshes
(kicad-cli GLB) and measures:

  - each of our 18 header pins against the reference module's pin and the body's socket
    opening: across the board (x) and through the thickness (h);
  - the insertion depth past the socket face, and the header bodies against the shell slots;
  - the reference module's board outline against ours.

It fails if a pin is more than TOL off, or if a header pad's number is not the Flipper pin at
its position (J401 pad n = pin n, J402 pad n = pin n + 8). It also draws true cross-sections
through pins 1 and 18 (triangle-plane intersections, not a render) to
.cache/hardware/fit/section-pin*.png:
ours filled, the reference module as an outline, the body in grey.

The script uses no numpy: the container has none.
"""
import collections
import json
import os
import struct
import subprocess
import sys

import pcbnew
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from place import FIT, flipper_model  # noqa: E402

TOL = 0.2                      # mm, pin to socket
OUT = os.path.dirname(FIT)
REFBOARD = os.path.join(OUT, "ssh-runner-fit-ref.kicad_pcb")
GLB = os.path.join(OUT, "fit-check.glb")


# --- GLB (binary glTF) reader: world-space triangles per top-level node ------------------------
def _qmat(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def _compose(a, b):
    ra, ta = a
    rb, tb = b
    r = [[sum(ra[i][k] * rb[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return r, [sum(ra[i][k] * tb[k] for k in range(3)) + ta[i] for i in range(3)]


def _node_tf(n):
    if "matrix" in n:
        m = n["matrix"]
        return [[m[0], m[4], m[8]], [m[1], m[5], m[9]], [m[2], m[6], m[10]]], [m[12], m[13], m[14]]
    r = _qmat(n.get("rotation", [0, 0, 0, 1]))
    s = n.get("scale", [1, 1, 1])
    return [[r[i][j] * s[j] for j in range(3)] for i in range(3)], list(n.get("translation", [0, 0, 0]))


def load_glb(path):
    """{node name: [triangles]}, each vertex (x, y, h) in board mm: x and y as in pcbnew,
    h = height, positive toward F (the parts side)."""
    data = open(path, "rb").read()
    jlen = struct.unpack("<I", data[12:16])[0]
    j = json.loads(data[20:20 + jlen])
    off = 20 + jlen
    blob = data[off + 8:off + 8 + struct.unpack("<I", data[off:off + 4])[0]]

    def accessor(i):
        a = j["accessors"][i]
        bv = j["bufferViews"][a["bufferView"]]
        start = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
        fmt = {5126: "f", 5125: "I", 5123: "H", 5121: "B"}[a["componentType"]]
        size = struct.calcsize(fmt)
        stride = bv.get("byteStride", size * n)
        unpack = struct.Struct("<" + fmt * n).unpack_from
        return [unpack(blob, start + k * stride) if n > 1 else unpack(blob, start + k * stride)[0]
                for k in range(a["count"])]

    res = collections.defaultdict(list)

    def walk(ni, tf, name):
        n = j["nodes"][ni]
        tf = _compose(tf, _node_tf(n))
        name = name or n.get("name")
        if "mesh" in n:
            r, t = tf
            for prim in j["meshes"][n["mesh"]]["primitives"]:
                pos = accessor(prim["attributes"]["POSITION"])
                idx = accessor(prim["indices"]) if "indices" in prim else list(range(len(pos)))
                w = [tuple(sum(r[i][k] * v[k] for k in range(3)) + t[i] for i in range(3)) for v in pos]
                w = [(p[0] * 1000, p[2] * 1000, p[1] * 1000) for p in w]     # glTF is metres, Y up
                res[name].extend((w[idx[k]], w[idx[k + 1]], w[idx[k + 2]]) for k in range(0, len(idx), 3))
        for c in n.get("children", []):
            walk(c, tf, name)

    ident = ([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 0, 0])
    for root in j["scenes"][j.get("scene", 0)]["nodes"]:
        rn = j["nodes"][root]
        for c in rn.get("children", []):
            walk(c, _compose(ident, _node_tf(rn)), None)
    return res


# --- measurements -------------------------------------------------------------------------------
def verts(tris):
    return {(round(v[0], 3), round(v[1], 3), round(v[2], 3)) for t in tris for v in t}


def clusters(values, gap):
    out = []
    for v in sorted(set(values)):
        if out and v - out[-1][-1] < gap:
            out[-1].append(v)
        else:
            out.append([v])
    return out


def pin_tips(vs):
    """x centres of the pins over their last 2 mm, their h centre and the tip y."""
    ymax = max(v[1] for v in vs)
    tip = [v for v in vs if v[1] > ymax - 2]
    xs = [(c[0] + c[-1]) / 2 for c in clusters([v[0] for v in tip], 1.0)]
    return xs, (min(v[2] for v in tip) + max(v[2] for v in tip)) / 2, ymax


def section(parts, x0, path, label):
    """Cross-section through the plane x = x0, in (y, h)."""
    y0, y1, h0, h1, s = 138.0, 160.0, -10.0, 19.0, 30
    im = Image.new("RGB", (int((y1 - y0) * s), int((h1 - h0) * s)), (255, 255, 255))
    d = ImageDraw.Draw(im)
    font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 15)
    P = lambda y, h: ((y - y0) * s, (h1 - h) * s)
    styles = {"BODY": ((150, 150, 150), 2), "BOARD": ((20, 110, 40), 2), "OURS": ((200, 30, 30), 2),
              "REF": ((30, 60, 220), 1)}
    for name, tris in parts.items():
        col, width = styles[name]
        for t in tris:
            pts = []
            for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
                if (a[0] - x0) * (b[0] - x0) < 0:
                    k = (x0 - a[0]) / (b[0] - a[0])
                    pts.append((a[1] + k * (b[1] - a[1]), a[2] + k * (b[2] - a[2])))
            if len(pts) == 2:
                d.line([P(*pts[0]), P(*pts[1])], fill=col, width=width)
    for yy in range(int(y0), int(y1) + 1):
        d.line([P(yy, h0), P(yy, h0 + (0.6 if yy % 5 else 1.2))], fill=(0, 0, 0))
        if yy % 5 == 0:
            d.text(P(yy + 0.1, h0 + 1.6), "y %d" % yy, fill=(0, 0, 0), font=font)
    for hh in range(int(h0), int(h1) + 1):
        d.line([P(y0, hh), P(y0 + (0.6 if hh % 5 else 1.2), hh)], fill=(0, 0, 0))
        if hh % 5 == 0:
            d.text(P(y0 + 1.4, hh + 0.4), "h %d" % hh, fill=(0, 0, 0), font=font)
    d.text((10, 8), label, fill=(0, 0, 0), font=font)
    d.text((10, 28), "red: our header   green: our board   blue: Flipper's reference module   "
                     "grey: Flipper body.  y: toward the Flipper (tab edge y 150); h: toward the back (F side)",
           fill=(0, 0, 0), font=font)
    im.save(path)


def main():
    board = pcbnew.LoadBoard(FIT)
    board.Add(flipper_model(board, "REFMOD", "external-module-pcb-3d-with-headers.stp"))
    pcbnew.SaveBoard(REFBOARD, board)
    subprocess.run(["kicad-cli", "pcb", "export", "glb", "-f", "--component-filter", "FLIPPER,REFMOD,J401,J402",
                    "-o", GLB, REFBOARD], check=True, stdout=subprocess.DEVNULL)
    mesh = load_glb(GLB)
    boardname = [k for k in mesh if k not in ("FLIPPER", "REFMOD", "J401", "J402")][0]
    ours = {k: verts(mesh[k]) for k in ("J401", "J402")}
    ref, body = verts(mesh["REFMOD"]), verts(mesh["FLIPPER"])

    # socket openings: rounded squares in the socket face, the plane just past the tab edge with most vertices
    rx, rh, rtip = pin_tips(ref)
    counts = collections.Counter(v[1] for v in body if 150 < v[1] < 152 and 100 < v[0] < 168)
    face = max(counts, key=counts.get)
    rim = [v for v in body if abs(v[1] - face) < 0.02 and abs(v[2] - rh) < 1.0]
    edges = [(c[0] + c[-1]) / 2 for c in clusters([round(v[0], 2) for v in rim], 0.6)]
    holes = [(edges[i] + edges[i + 1]) / 2 for i in range(0, len(edges) - 1, 2)]
    hole_h = (min(v[2] for v in rim) + max(v[2] for v in rim)) / 2

    j401, j402 = pin_tips(ours["J401"]), pin_tips(ours["J402"])
    # Flipper pin 1 is the outer end of the 8-pin group (J401, highest x in the F view); 9..18 follow
    order = sorted(j401[0], reverse=True) + sorted(j402[0], reverse=True)
    print("socket openings found: %d, socket face y %.3f" % (len(holes), face))
    print("pin   ours x   ref x  socket x    dx   |  ours h  ref h  socket h    dh")
    worst = 0.0
    for n, x in enumerate(order, 1):
        r = min(rx, key=lambda a: abs(a - x))
        sx = min(holes, key=lambda a: abs(a - x))
        h = (j401 if n <= 8 else j402)[1]
        worst = max(worst, abs(x - sx), abs(h - hole_h), abs(x - r), abs(h - rh))
        print("%3d  %7.3f %7.3f %8.3f  %+.3f  |  %6.3f %6.3f %8.3f  %+.3f" % (n, x, r, sx, x - sx, h, rh, hole_h, h - hole_h))
    print("insertion past the socket face: ours %.2f mm, reference %.2f mm (drawing: 6 mm pin, tab edge y 150)"
          % (j401[2] - face, rtip - face))

    # pad numbers by position: J401 pad n must be Flipper pin n, J402 pad n Flipper pin n + 8 (a
    # mirrored or renumbered footprint would still pass the geometry above)
    misnumbered = []
    for hdr, base in (("J401", 0), ("J402", 8)):
        for pad in board.FindFootprintByReference(hdr).Pads():
            n, x = int(pad.GetNumber()), pcbnew.ToMM(pad.GetPosition().x)
            if abs(x - order[base + n - 1]) > TOL:
                misnumbered.append("%s pad %d at x %.2f, Flipper pin %d at x %.2f" % (hdr, n, x, base + n, order[base + n - 1]))
    for m in misnumbered:
        print("PAD NUMBER", m)
    print("pad numbers against Flipper pins 1-18: %s" % ("OK" if not misnumbered else "FAIL"))

    # reference board outline vs ours: the plate is the reference's thin slab
    plate = [v for v in ref if -0.1 < v[2] < 1.2]
    bb = verts(mesh[boardname])
    print("board outline: ours x %.3f..%.3f y %.3f..%.3f; reference x %.3f..%.3f y %.3f..%.3f"
          % (min(v[0] for v in bb), max(v[0] for v in bb), min(v[1] for v in bb), max(v[1] for v in bb),
             min(v[0] for v in plate), max(v[0] for v in plate), min(v[1] for v in plate), max(v[1] for v in plate)))
    # header plastic against the shell slots (slot = opening in the shell top, around the sockets)
    for k in ("J401", "J402"):
        top = max(v[2] for v in ours[k])
        pl = [v for v in ours[k] if v[2] > top - 0.05]
        print("%s plastic: x %.2f..%.2f, y %.2f..%.2f, h %.2f..%.2f"
              % (k, min(v[0] for v in pl), max(v[0] for v in pl), min(v[1] for v in pl), max(v[1] for v in pl),
                 min(v[2] for v in ours[k] if v[1] > 147.6 and v[1] < 150.3), top))
    shell = [v for v in body if 149.8 < v[1] < 150.1 and 0.5 < v[2] < 4.5]
    xs = sorted({v[0] for v in shell})
    print("shell slot corners near the tab edge: x %s, h %.2f..%.2f"
          % (" ".join("%.2f" % x for x in xs), min(v[2] for v in shell), max(v[2] for v in shell)))

    for n, label in ((1, "pin 1 (5V)"), (18, "pin 18 (GND)")):
        x = order[n - 1]
        section({"BODY": mesh["FLIPPER"], "BOARD": mesh[boardname], "REF": mesh["REFMOD"],
                 "OURS": mesh["J401" if n <= 8 else "J402"]}, x + 0.05,
                os.path.join(OUT, "section-pin%d.png" % n), "Section through Flipper %s, x = %.2f mm" % (label, x))
    ok = worst <= TOL and not misnumbered
    print("worst pin offset %.3f mm (limit %.1f): %s" % (worst, TOL, "PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
