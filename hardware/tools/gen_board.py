"""Creates the starting board (hardware/ssh-runner.kicad_pcb): outline, stack-up, art.

    hardware/tools/kicad.sh python3 tools/gen_board.py [--force]

- Outline: Flipper's official external-module DXF (ref/flipper-3d-models), mirrored left-right:
  the DXF is drawn from the face without the header bodies, which faces the Flipper's screen.
  Our top (F) side carries the parts and the right-angle header bodies and faces the back of the
  Flipper, so the tall RJ45 stays within the Flipper's thickness; Flipper pin 1 is at the right
  in the F view. Every entity is the DXF's own: the bottom edge (two tabs, a notch) and all four
  outer corners, 45-degree chamfers with 1 mm legs blended by R0.5 arcs, as on the
  official module. Only the two sides are longer, BOARD_H instead of 20 mm, so the top
  corners and top edge move up unchanged.
- Stack-up: 4 layers, 1.2 mm, black solder mask, white silkscreen, ENIG (JLC 4-layer).
- Art on the back (B.*) layer, which faces the user above the Flipper's screen, mirrored so it
  reads correctly from that side, all in white silkscreen:
  - the logo (art/logo-x.svg, all rights reserved, see art/LICENSE.md);
  - "SSH Runner", the revision and the repository URL.

Footprints, placement and routing come later (layout). Like gen_sch.py this only bootstraps:
it refuses to overwrite an existing board unless --force is given.
"""
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
HW = os.path.join(HERE, "..")
BOARD = os.environ.get("BOARD_OUT", os.path.join(HW, "ssh-runner.kicad_pcb"))   # BOARD_OUT: studies only
DXF = os.path.join(HW, "ref", "flipper-3d-models", "external-module-pcb-outline.dxf")
LOGO = os.path.join(HW, "art", "logo-x.svg")

BOARD_H = float(os.environ.get("BOARD_H", "36.0"))   # tab edge to top edge, see DESIGN.md 5.2
DXF_H = 20.0        # the official module's height, tab edge to top edge
LOGO_H = 9.0        # logo height: thinnest bar and gap 6.6 % = 0.59 mm (JLC silkscreen >= 0.15 mm)
REV = "rev 0.1"
URL = "github.com/SeanChangX/flipper-ssh-runner"

# DXF coordinates (y up) -> board (y down), mirrored left-right: the DXF's right edge lands at
# x = X0, the tab bottom edge at y = Y0.
DXF_X0, DXF_X1, DXF_Y0 = 114.763092, 182.103088, 103.505928
X0, Y0 = 100.0, 150.0


MIRROR = os.environ.get("OUTLINE_MIRROR", "1") == "1"   # 0 only for the orientation study (DESIGN.md 5.2)


def P(x, y):
    """DXF point -> KiCad internal units (mirrored in x unless OUTLINE_MIRROR=0)."""
    px = X0 + DXF_X1 - x if MIRROR else X0 + x - DXF_X0
    return pcbnew.VECTOR2I(pcbnew.FromMM(px), pcbnew.FromMM(Y0 - (y - DXF_Y0)))


def mm(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))


def dxf_entities(path):
    lines = open(path, encoding="latin-1").read().splitlines()
    pairs = [(lines[i].strip(), lines[i + 1].strip()) for i in range(0, len(lines) - 1, 2)]
    ents, cur, inside = [], None, False
    for c, v in pairs:
        if c == "2" and v == "ENTITIES":
            inside = True
            continue
        if inside and c == "0":
            if cur:
                ents.append(cur)
            if v == "ENDSEC":
                break
            cur = {"type": v}
        elif inside and cur is not None and c in ("10", "20", "11", "21", "40", "50", "51"):
            cur.setdefault(c, float(v))
    return ents


def add_edge(board, shape):
    shape.SetLayer(pcbnew.Edge_Cuts)
    shape.SetWidth(pcbnew.FromMM(0.1))
    board.Add(shape)


def outline(board):
    """The official outline, taller: its bottom (tabs, notch, corners) and its top corners are
    Flipper's own entities; only the two sides are longer, so the top part moves up by
    BOARD_H - DXF_H."""
    left, right = DXF_X0, DXF_X1
    shift = BOARD_H - DXF_H
    side_lo, side_hi = 107.313034, 122.29882      # the DXF's straight sides, between the corners

    def arc(cx, cy, r, a0, a1, dy):
        pts = []
        for a in (a0, (a0 + a1) / 2 if a1 > a0 else (a0 + a1 + 360) / 2, a1):
            pts.append(P(cx + r * math.cos(math.radians(a)), cy + dy + r * math.sin(math.radians(a))))
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_ARC)
        s.SetArcGeometry(*pts)
        add_edge(board, s)

    for e in dxf_entities(DXF):
        if e["type"] == "LINE":
            x0, y0, x1, y1 = e["10"], e["20"], e["11"], e["21"]
            if abs(x0 - x1) < 1e-6 and abs(x0 - left) < 1e-3 or abs(x0 - x1) < 1e-6 and abs(x0 - right) < 1e-3:
                y0, y1 = side_lo, side_hi + shift           # a side: stretched
                dy0 = dy1 = 0
            else:
                dy0 = dy1 = shift if min(y0, y1) > side_hi - 1e-3 else 0
            s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
            s.SetStart(P(x0, y0 + dy0))
            s.SetEnd(P(x1, y1 + dy1))
            add_edge(board, s)
        elif e["type"] == "ARC":
            cx, cy = e["10"], e["20"]
            arc(cx, cy, e["40"], e["50"], e["51"], shift if cy > side_hi - 1e-3 else 0)
    return (X0, Y0 - BOARD_H, X0 + DXF_X1 - DXF_X0, Y0)


def logo_polygons(path):
    root = ET.parse(path).getroot()
    vb = [float(v) for v in root.get("viewBox").split()]
    polys = []
    for el in root.iter():
        if el.tag.endswith("polygon"):
            pts = [float(v) for v in re.split(r"[ ,]+", el.get("points").strip())]
            polys.append(list(zip(pts[0::2], pts[1::2])))
    return vb, polys


def add_logo(board, cx, cy, height, layers):
    """Logo centred at (cx, cy) mm, mirrored for the back side."""
    (vx, vy, vw, vh), polys = logo_polygons(LOGO)
    k = height / vh
    for layer in layers:
        for poly in polys:
            s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY)
            pts = [mm(cx - ((x - vx) - vw / 2) * k, cy + ((y - vy) - vh / 2) * k) for x, y in poly]
            s.SetPolyPoints(pts)
            s.SetFilled(True)
            s.SetWidth(0)
            s.SetLayer(layer)
            board.Add(s)
    return vw * k, height


def add_text(board, text, x, y, size, thick, layer=pcbnew.B_SilkS):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(text)
    t.SetPosition(mm(x, y))
    t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(size), pcbnew.FromMM(size)))
    t.SetTextThickness(pcbnew.FromMM(thick))
    t.SetMirrored(layer in (pcbnew.B_SilkS, pcbnew.B_Cu, pcbnew.B_Mask))
    t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_CENTER)
    board.Add(t)


# Art block on the back (one block, vertically centred).
# Positions in board coordinates (the F view). From the front the block is mirrored: x' = X_MIRROR - x.
#   lockup: title "SSH Runner" left, logo right; under the title the revision (left-aligned with
#   the title) and the two LED light holes (right-aligned with it, power then status);
#   the URL centred under the whole lockup.
# The block is centred vertically on the board body (y 114..147.4). Horizontally it is as close to
# the board's centre as two keep-clear rules allow (1.5 mm each): the title's end against the
# RJ45 shield pin, and the logo against the module's 9 thermal vias. Its centre is 3 mm off.
X_MIRROR = 267.34
ART_LAYOUTS = {
    # name: (block shift dx, dy) from the reference block below
    "centred": (6.0, -7.32),
    "clean-region": (-0.26, -7.32),         # centred between the RJ45 shield pin and the far edge
}
ART = os.environ.get("ART_LAYOUT", "centred")
LED_PITCH = 5.0                              # light-hole spacing: the two parts sit side by side


def art_layout(name=ART):
    dx, dy = ART_LAYOUTS[name]
    title = (131.5 + dx, 135.2 + dy)
    title_w = 19.16                          # Orbitron Bold outline width (tools/title_font.py)
    left = title[0] + title_w / 2            # the title's start as seen from the front
    right = title[0] - title_w / 2           # its end
    logo = (113.9 + dx, 136.7 + dy)
    logo_end = logo[0] - 11.18 / 2           # the logo's far end as seen from the front
    return {
        "logo": logo,
        "title": title,
        "rev": (left - 5.64 / 2, 138.2 + dy),    # left-aligned with the title, seen from the front
        "led_status": (right + 0.9, 138.2 + dy),  # hole edge flush with the title's end
        "led_power": (right + 0.9 + LED_PITCH, 138.2 + dy),
        "url": ((left + logo_end) / 2, 143.0 + dy),   # centred under the whole lockup
    }


def design_rules(board):
    """Board-wide minimums inside JLCPCB's 4-layer capabilities; details in ssh-runner.kicad_dru."""
    ds = board.GetDesignSettings()
    ds.SetCopperLayerCount(4)
    ds.SetBoardThickness(pcbnew.FromMM(BOARD_T))
    ds.m_MinClearance = pcbnew.FromMM(0.15)
    ds.m_TrackMinWidth = pcbnew.FromMM(0.15)
    ds.m_ViasMinSize = pcbnew.FromMM(0.5)
    ds.m_ViasMinAnnularWidth = pcbnew.FromMM(0.1)    # 0.5/0.3 mm vias
    ds.m_MinThroughDrill = pcbnew.FromMM(0.3)        # JLC's free tier (CAM.md)
    ds.m_HoleToHoleMin = pcbnew.FromMM(0.5)
    ds.m_HoleClearance = pcbnew.FromMM(0.2)
    ds.m_CopperEdgeClearance = pcbnew.FromMM(0.3)
    ds.m_MinSilkTextHeight = pcbnew.FromMM(1.0)
    ds.m_MinSilkTextThickness = pcbnew.FromMM(0.15)
    ds.m_SolderMaskMinWidth = pcbnew.FromMM(0.1)
    ds.m_TentViasFront = True       # every via tented on both sides: nothing shows through the art
    ds.m_TentViasBack = True


# JLCPCB JLC04121H-7628, the default (free) 4-layer 1.2 mm build (DESIGN.md 5.1); the order must
# name it. Figures from JLC's impedance calculator: 7628 prepreg 0.2104 mm (Er 4.4) under each outer
# layer, a 0.6 mm core, finished 1.12 mm nominal (1.01-1.23 mm).
BOARD_T = 1.12
STACKUP = """	(stackup
		(layer "F.SilkS" (type "Top Silk Screen") (color "White"))
		(layer "F.Paste" (type "Top Solder Paste"))
		(layer "F.Mask" (type "Top Solder Mask") (color "Black") (thickness 0.01))
		(layer "F.Cu" (type "copper") (thickness 0.035))
		(layer "dielectric 1" (type "prepreg") (color "FR4 natural") (thickness 0.2104) (material "7628") (epsilon_r 4.4) (loss_tangent 0.02))
		(layer "In1.Cu" (type "copper") (thickness 0.0152))
		(layer "dielectric 2" (type "core") (color "FR4 natural") (thickness 0.6) (material "FR4") (epsilon_r 4.6) (loss_tangent 0.02))
		(layer "In2.Cu" (type "copper") (thickness 0.0152))
		(layer "dielectric 3" (type "prepreg") (color "FR4 natural") (thickness 0.2104) (material "7628") (epsilon_r 4.4) (loss_tangent 0.02))
		(layer "B.Cu" (type "copper") (thickness 0.035))
		(layer "B.Mask" (type "Bottom Solder Mask") (color "Black") (thickness 0.01))
		(layer "B.Paste" (type "Bottom Solder Paste"))
		(layer "B.SilkS" (type "Bottom Silk Screen") (color "White"))
		(copper_finish "ENIG")
		(dielectric_constraints no)
	)
"""


def main():
    if os.path.exists(BOARD) and "--force" not in sys.argv:
        raise SystemExit("ssh-runner.kicad_pcb exists; it is the source now (use --force to regenerate)")
    board = pcbnew.BOARD()
    design_rules(board)
    x0, y0, x1, y1 = outline(board)
    a = art_layout()
    w, h = add_logo(board, a["logo"][0], a["logo"][1], LOGO_H, [pcbnew.B_SilkS])
    add_text(board, "SSH Runner", a["title"][0], a["title"][1], 2.0, 0.3)
    add_text(board, REV, a["rev"][0], a["rev"][1], 1.0, 0.15)
    add_text(board, URL, a["url"][0], a["url"][1], 1.0, 0.15)
    pcbnew.SaveBoard(BOARD, board)
    text = open(BOARD, encoding="utf-8").read()
    text = re.sub(r"\(general\s*\(thickness [\d.]+\)", "(general\n\t\t(thickness %g)" % BOARD_T, text, count=1)
    text = text.replace("\t(setup\n", "\t(setup\n" + STACKUP, 1)
    with open(BOARD, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("board %.2f x %.2f mm, logo %.1f x %.1f mm" % (x1 - x0, y1 - y0, w, h))


if __name__ == "__main__":
    main()
