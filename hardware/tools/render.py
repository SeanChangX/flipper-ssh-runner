"""Product renders of the assembled board for the README (raytraced, transparent background).

    hardware/tools/kicad.sh python3 tools/render.py              # all views, final quality
    hardware/tools/kicad.sh python3 tools/render.py top 0.25     # one view, quarter size (a quick look)

Writes .cache/hardware/render/<view>.png. Each view is raytraced by kicad-cli at RENDER_W wide
(2x the final width), cropped to the board, and downsampled with Lanczos onto a canvas of the same
size and margins for every view, so the three line up in the README.

Appearance (none of it in the board file):
  - finish: ENIG gold pads, matte black
    solder mask, white silkscreen, 1.2 mm board (the stack-up: 1.12 mm);
  - only fitted parts: the DNP footprints carry no 3D model and are hidden;
  - soft, even light (VIEW_LIGHTS: the bottom view by side light only), soft shadows, no floor.
The colours are set in the stack-up of a scratch copy of the board (.cache/hardware/render/board,
with the 3D models beside it); ssh-runner.kicad_pcb itself is not touched. The raytracer's settings
live in a KiCad config made for this script (.cache/hardware/render/home), so the result does not
depend on anyone's KiCad preferences.
"""
import json
import os
import re
import shutil
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
HW = os.path.normpath(os.path.join(HERE, ".."))
OUT = os.path.normpath(os.path.join(HW, "..", ".cache", "hardware", "render"))
HOME = os.path.join(OUT, "home")
CFG = os.path.join(HOME, ".config", "kicad", "10.0")
BOARD = os.path.join(OUT, "board")

FINAL = (1800, 1100)          # final canvas: 2x a ~900 px README column
RENDER_W, RENDER_H = 3600, 2200
MARGIN = 0.07                 # free border on each side, as a fraction of the canvas

# view -> kicad-cli pcb render arguments
VIEWS = {
    # 3/4 view of the parts side: headers toward the viewer, USB-C and RJ45 at the back
    "hero": ["--side", "top", "--rotate", "-55,0,25", "--perspective"],
    "top": ["--side", "top"],
    "bottom": ["--side", "bottom"],
}

# stack-up colours of the scratch copy (rendered with --use-board-stackup-colors); the copper
# colour follows the stack-up's finish, ENIG
STACKUP_COLOURS = {"Top Solder Mask": "#0E0E10F2", "Bottom Solder Mask": "#0E0E10F2",
                   "Top Silk Screen": "#F2F2F0FF", "Bottom Silk Screen": "#F2F2F0FF"}

# kicad-cli takes the lights from its own options, not from the config: soft, even light from
# above and from the sides, little from the camera, which would grey the black mask head-on
# (KiCad's defaults are about 0.25 / 0.2 / 0.17)
LIGHTS = {"top": 0.15, "camera": 0.04, "side": 0.18, "bottom": 0.12, "side-elevation": 50}
# per view; RENDER_LIGHTS="top,camera,side,bottom,side-elevation" in the environment overrides
# them for a trial
VIEW_LIGHTS = {
    "hero": LIGHTS,
    "top": LIGHTS,
    # straight at the back face, the bottom light and the camera light reflect off the mask as a
    # bright spot in the middle; side light only, which keeps the mask even edge to edge
    "bottom": {"top": 0.15, "camera": 0, "side": 0.32, "bottom": 0, "side-elevation": 45},
}


def lights(view):
    l = dict(VIEW_LIGHTS[view])
    if os.environ.get("RENDER_LIGHTS"):
        l = dict(zip(("top", "camera", "side", "bottom", "side-elevation"),
                     (float(v) for v in os.environ["RENDER_LIGHTS"].split(","))))
    return [a for k, v in l.items() for a in ("--light-" + k, "%g" % v)]

RENDER = {  # raytracer settings (3d_viewer.json "render"), used with --quality user
    "engine": 1,
    "material_mode": 0,
    "raytrace_anti_aliasing": True,
    "raytrace_backfloor": False,
    "raytrace_post_processing": True,         # ambient occlusion: the soft contact shadows
    "raytrace_procedural_textures": False,    # clean surfaces, no noise
    "raytrace_reflections": True,
    "raytrace_refractions": True,
    "raytrace_shadows": True,
    "raytrace_nrsamples_shadows": 8,
    "raytrace_nrsamples_reflections": 6,
    "raytrace_nrsamples_refractions": 6,
    "raytrace_recursivelevel_reflections": 3,
    "raytrace_recursivelevel_refractions": 2,
    "raytrace_spread_shadows": 0.12,          # soft-edged shadows
    "raytrace_spread_reflections": 0.06,      # satin, not mirror
    "raytrace_spread_refractions": 0.025,
    "show_footprints_dnp": False,
    "show_fp_references": False,
    "show_fp_values": False,
    "show_fp_text": False,
    "show_comments": False,
    "show_drawings": False,
    "show_eco1": False,
    "show_eco2": False,
    "show_solderpaste": False,
    "show_adhesive": False,
    "show_navigator": False,
}


def write_config():
    for d in (HOME, BOARD):
        if os.path.isdir(d):
            shutil.rmtree(d)
    os.makedirs(CFG)
    with open(os.path.join(CFG, "3d_viewer.json"), "w") as f:
        json.dump({"render": RENDER, "use_stackup_colors": True}, f, indent=2)
    for t in ("sym-lib-table", "fp-lib-table"):
        shutil.copyfile("/usr/share/kicad/template/" + t, os.path.join(CFG, t))
    # scratch board: the stack-up colours above, the project's 3D models beside it (${KIPRJMOD}/3d)
    if os.path.isdir(os.path.join(HW, "3d")):     # the LCSC models, fetched by tools/lcsc_3d.py
        shutil.copytree(os.path.join(HW, "3d"), os.path.join(BOARD, "3d"))
    else:
        os.makedirs(BOARD)
        print("hardware/3d/ is missing: USB-C, RJ45 and LEDs render without models (tools/lcsc_3d.py)")
    with open(os.path.join(HW, "ssh-runner.kicad_pcb"), encoding="utf-8") as f:
        pcb = f.read()
    assert '(copper_finish "ENIG")' in pcb
    for layer_type, colour in STACKUP_COLOURS.items():
        pcb, n = re.subn(r'(\(type "%s"\)\s*\(color )"[^"]*"' % layer_type, r'\g<1>"%s"' % colour, pcb)
        assert n == 1, layer_type
    with open(os.path.join(BOARD, "ssh-runner.kicad_pcb"), "w", encoding="utf-8", newline="\n") as f:
        f.write(pcb)


def render(view, scale):
    raw = os.path.join(OUT, "raw-%s.png" % view)
    w, h = int(RENDER_W * scale), int(RENDER_H * scale)
    cmd = ["kicad-cli", "pcb", "render", "-o", raw, "-w", str(w), "-h", str(h), "--quality", "user",
           "--background", "transparent", "--use-board-stackup-colors"] + lights(view) + VIEWS[view] + [
        os.path.join(BOARD, "ssh-runner.kicad_pcb")]
    subprocess.run(cmd, check=True, env=dict(os.environ, HOME=HOME), stdout=subprocess.DEVNULL)
    return raw


def frame(raw, view, scale):
    """Crop to the board and place it, same margins on every view, on the FINAL canvas."""
    im = Image.open(raw).convert("RGBA")
    im = im.crop(im.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox())
    cw, ch = int(FINAL[0] * scale), int(FINAL[1] * scale)
    k = min(cw * (1 - 2 * MARGIN) / im.width, ch * (1 - 2 * MARGIN) / im.height)
    im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
    canvas = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    canvas.paste(im, ((cw - im.width) // 2, (ch - im.height) // 2), im)
    dst = os.path.join(OUT, view + ".png")
    canvas.save(dst, optimize=True)
    return dst, k


def main():
    views = [sys.argv[1]] if len(sys.argv) > 1 else list(VIEWS)
    scale = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    os.makedirs(OUT, exist_ok=True)
    write_config()
    for v in views:
        dst, k = frame(render(v, scale), v, scale)
        print("%s: %s, %d bytes (downsampled x%.2f)" % (v, os.path.relpath(dst, HW), os.path.getsize(dst), k))


if __name__ == "__main__":
    main()
