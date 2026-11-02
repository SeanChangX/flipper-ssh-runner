"""3D models from LCSC/EasyEDA for the parts KiCad's library has no model for (hardware/3d).

    hardware/tools/kicad.sh python3 -I tools/lcsc_3d.py fetch     # download into .cache/hardware/easyeda3d
    hardware/tools/kicad.sh python3 -I tools/lcsc_3d.py wrl       # write hardware/3d/*.wrl and *.step

EasyEDA serves each part's model twice, as STEP and as an OBJ mesh, in the same coordinates (mm,
z up, z = 0 on the board's top face). The STEP is copied as is (KiCad's STEP export substitutes
it). The .wrl, which the footprints reference and the renders use, is made from the OBJ without
the EasyEDA logo that some models carry on a face (its fill dropped, the lettering cut into the
face flattened), the way KiCad's own library pairs a .wrl for rendering with a .step for export.
Data only: nothing fetched is executed.

The placement on our footprints (offset, rotation in ssh-runner.kicad_pcb) comes from the model's
own legs and pins matched to our pads, not from EasyEDA's footprint origin:
  J201  shell legs and pegs on ours: offset (0, -1.04, 0), rotate 180
  J301  8 signal, 4 LED pins and 2 shield legs on ours (within 0.05 mm): offset (4.445, -6.55, 0)
  D103  frame resting on the pads (this model sits 0.29 mm low): offset (0, 0, 0.29), rotate 180
  D201  offset 0; the rotations follow EasyEDA's (the two LEDs' models have opposite polarity)
"""
import gzip
import json
import os
import shutil
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "..", "..", ".cache", "hardware", "easyeda3d")
OUT = os.path.join(HERE, "..", "3d")

# LCSC part -> (file name in hardware/3d, OBJ materials to drop: the logo's fill,
#               (material, axis, side) faces flattened into one plain face: the logo cut into it)
PARTS = {
    "C165948": ("USB_C_Receptacle_HRO_TYPE-C-31-M-12", {"5"}, [("1", 2, "max")]),   # on the shell's top
    "C69427": ("RJ45_HanRun_HY911105AE", {"7"}, [("2", 1, "max")]),               # on the shield's back
    "C3646935": ("LED_XINGLIGHT_XL-3216UWC-FB", set(), []),
    "C3646937": ("LED_XINGLIGHT_XL-3216UGC-FB", {"4"}, []),                       # on the side
}


def get(url):
    req = urllib.request.Request(url, headers={
        "User-Agent": "Mozilla/5.0 (X11; Linux x86_64)", "Accept-Encoding": "gzip, deflate",
        "Accept": "application/json, text/javascript, */*; q=0.01"})
    with urllib.request.urlopen(req, timeout=60) as r:
        data = r.read()
        return gzip.decompress(data) if r.headers.get("Content-Encoding") == "gzip" else data


def fetch():
    os.makedirs(CACHE, exist_ok=True)
    for lcsc in PARTS:
        comp = json.loads(get("https://easyeda.com/api/products/%s/components?version=6.4.19.5" % lcsc))
        with open(os.path.join(CACHE, lcsc + ".json"), "w") as f:
            json.dump(comp, f)
        shapes = comp["result"]["packageDetail"]["dataStr"]["shape"]
        uuid = [json.loads(s.split("~", 1)[1])["attrs"]["uuid"] for s in shapes if s.startswith("SVGNODE~")][0]
        for ext, url in ((".step", "https://modules.easyeda.com/qAxj6KHrDKw4blvCG8QJPs7Y/"),
                         (".obj", "https://modules.easyeda.com/3dmodel/")):
            with open(os.path.join(CACHE, lcsc + ext), "wb") as f:
                f.write(get(url + uuid))
        print(lcsc, comp["result"]["title"])


def obj_to_wrl(src, dst, drop, flatten):
    """OBJ (mm) -> VRML 2.0 in KiCad's units (0.1 inch), one Shape per material, `drop` left out and
    the faces of each `flatten` plane replaced by one rectangle over their extent."""
    verts, mats, groups, cur, name = [], {}, {}, None, None
    for line in open(src):
        t = line.split()
        if not t:
            continue
        if t[0] == "newmtl":
            name = t[1]
            mats[name] = {}
        elif t[0] in ("Kd", "Ks") and name:
            mats[name][t[0]] = [float(v) for v in t[1:4]]
        elif t[0] == "v":
            verts.append([float(v) / 2.54 for v in t[1:4]])
        elif t[0] == "usemtl":
            cur = t[1]
        elif t[0] == "f" and cur not in drop:
            groups.setdefault(cur, []).append([int(x.split("/")[0]) - 1 for x in t[1:]])
    for mat, axis, side in flatten:
        level = (max if side == "max" else min)(v[axis] for v in verts)
        on = [f for f in groups[mat] if all(abs(verts[i][axis] - level) < 0.06 / 2.54 for i in f)]
        pts = [verts[i] for f in on for i in f]
        a, b = [k for k in range(3) if k != axis]
        lo_a, hi_a = min(p[a] for p in pts), max(p[a] for p in pts)
        lo_b, hi_b = min(p[b] for p in pts), max(p[b] for p in pts)
        groups[mat] = [f for f in groups[mat] if f not in on]
        n = len(verts)
        for ca, cb in ((lo_a, lo_b), (hi_a, lo_b), (hi_a, hi_b), (lo_a, hi_b)):
            p = [0.0, 0.0, 0.0]
            p[axis], p[a], p[b] = level, ca, cb
            verts.append(p)
        # wind it so the normal points out of the part (KiCad shades by the winding)
        outward = (1 if side == "max" else -1) * (1 if (a, b) != (0, 2) else -1)
        groups[mat].append([n, n + 1, n + 2, n + 3] if outward > 0 else [n + 3, n + 2, n + 1, n])
    out = ["#VRML V2.0 utf8", "# %s, from LCSC/EasyEDA (hardware/tools/lcsc_3d.py)" % os.path.basename(dst)]
    for mat, faces in groups.items():
        used = sorted({i for f in faces for i in f})
        index = {v: n for n, v in enumerate(used)}
        kd = mats.get(mat, {}).get("Kd", [0.8, 0.8, 0.8])
        ks = mats.get(mat, {}).get("Ks", [0.3, 0.3, 0.3])
        out.append("Shape { appearance Appearance { material Material { diffuseColor %.4f %.4f %.4f "
                   "specularColor %.4f %.4f %.4f ambientIntensity 0.2 shininess 0.3 transparency 0 } }" % (*kd, *ks))
        out.append("geometry IndexedFaceSet { solid FALSE creaseAngle 0.5 coord Coordinate { point [")
        out.append(",\n".join("%.5f %.5f %.5f" % tuple(verts[v]) for v in used))
        out.append("] } coordIndex [")
        out.append(",\n".join(" ".join(str(index[i]) for i in f) + " -1" for f in faces))
        out.append("] } }")
    with open(dst, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def wrl():
    for lcsc, (name, drop, flatten) in PARTS.items():
        shutil.copyfile(os.path.join(CACHE, lcsc + ".step"), os.path.join(OUT, name + ".step"))
        obj_to_wrl(os.path.join(CACHE, lcsc + ".obj"), os.path.join(OUT, name + ".wrl"), drop, flatten)
        print(name, {e: os.path.getsize(os.path.join(OUT, name + e)) for e in (".step", ".wrl")})


if __name__ == "__main__":
    {"fetch": fetch, "wrl": wrl}[sys.argv[1]]()
