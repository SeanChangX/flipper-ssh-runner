"""Routes the board: the hand routes of routes.py, ground vias at every ground pad, stitching, the
keep-outs and the ground pours.

    hardware/tools/kicad.sh python3 tools/route.py           # finish ssh-runner.kicad_pcb in place
    hardware/tools/kicad.sh python3 tools/route.py --hand    # hand routes only (review, debugging)

Run after place.py and title_font.py (the board must have no tracks yet).

Stack-up use (DESIGN.md 5.1):
  F.Cu    parts, short connections, the differential pairs, ground pour
  In1.Cu  solid ground, the reference for F.Cu; never routed
  In2.Cu  long signals and power, referenced to the ground pour on B.Cu, ground pour between them
  B.Cu    the art side: ground pour only, except the USB D+ hop under the connector, outside the art

1. HAND routes (routes.py: lists of points per layer, written to be reviewed like a drawing): every
   signal and power connection on the board.
2. Ground: every GND pad gets a via to the planes; stitching vias fence the In2 USB and SPI lines,
   sit beside every signal via, line the board edge and cover the rest on a coarse grid.
3. Rule areas (the module's antenna keep-out, the RJ45 cable side) stay empty on every layer; the
   ground pours fill the rest of all four layers.
"""
import math
import os
import sys
from array import array

import pcbnew

BOARD = os.environ.get("ROUTE_BOARD", "/project/hardware/ssh-runner.kicad_pcb")
G = 0.1                       # placement grid for the ground vias, mm
CLEAR = 0.15                  # copper clearance (ssh-runner.kicad_dru)
EDGE = 0.3                    # copper to board edge
VIA_D, VIA_DRILL = 0.6, 0.3   # 0.15 mm annular ring (ssh-runner.kicad_dru)
MARGIN = 0.05                 # grid discretisation: every obstacle grows by this much more
HOLE_TO_HOLE = 0.5
LAYERS = [pcbnew.F_Cu, pcbnew.In2_Cu, pcbnew.B_Cu]

mm = pcbnew.FromMM
um = pcbnew.ToMM


def P(x, y):
    return pcbnew.VECTOR2I(mm(x), mm(y))


# ------------------------------------------------------------------------------------------------
# Hand routes: (net, layer, width, [(x, y), ...]); "via" entries: (net, "via", (x, y))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from routes import HAND  # noqa: E402

LAYER_NAMES = {"F": pcbnew.F_Cu, "In2": pcbnew.In2_Cu, "B": pcbnew.B_Cu}

# ------------------------------------------------------------------------------------------------
# Mitres: every right-angle bend in a hand route becomes a 45-degree chamfer. Each leg gives up
# MITRE x the track width (the chamfer is then about 2.1 x the width long), at most half a leg
# that has another mitre at its far end, otherwise all of the leg but 0.02 mm. On a differential
# pair the outer line of a bend takes the longer chamfer, so the gap stays constant through it.
MITRE = 1.5
PAIRS = {"/MCU/USB_DN": "/MCU/USB_DP", "/Ethernet/ETH_RXP": "/Ethernet/ETH_RXN",
         "/Ethernet/ETH_TXP": "/Ethernet/ETH_TXN", "/Ethernet/ETH_RDP": "/Ethernet/ETH_RDN"}
PAIRS.update({b: a for a, b in list(PAIRS.items())})
# Bends whose full chamfer would reach a neighbouring pad: (net, layer, vertex) -> leg given up
MITRE_LIMIT = {
    ("VBUS", "F", (139.83, 122.9)): 0.3,          # 0.5 mm line leaving A4/B9: a longer chamfer meets A12 (GND)
    ("FLIPPER_3V3", "F", (124.4, 121.6)): 0.2,    # into U102 pin 1: a longer chamfer passes pin 7 (+3V3)
}


def _unit(a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    n = math.hypot(dx, dy)
    return (dx / n, dy / n), n


def _corners(pts):
    """{index: (d_in, d_out)} for every right-angle vertex of a polyline."""
    out = {}
    for i in range(1, len(pts) - 1):
        (d1, l1), (d2, l2) = _unit(pts[i - 1], pts[i]), _unit(pts[i], pts[i + 1])
        if l1 > 1e-6 and l2 > 1e-6 and abs(d1[0] * d2[0] + d1[1] * d2[1]) < 1e-3:
            out[i] = (d1, d2)
    return out


def mitred(net, layer, width, pts, routes):
    """The polyline with each right-angle vertex replaced by a 45-degree chamfer."""
    others = [r[3] for r in routes if r[0] == net and r[1] == layer and r[3] is not pts]
    pts = [tuple(p) for p in pts]
    corners = _corners(pts)
    if not corners:
        return pts
    partner = [r for r in routes if r[0] == PAIRS.get(net) and r[1] == layer]
    out = [pts[0]]
    for i in range(1, len(pts) - 1):
        # a vertex another route of the net also reaches is a T-junction, not a bend
        if i not in corners or any(tuple(q) == pts[i] for r in others for q in r):
            out.append(pts[i])
            continue
        (d1, d2), A = corners[i], pts[i]
        c = MITRE_LIMIT.get((net, layer, A), MITRE * width)
        for r in partner:                      # the same bend on the other line of the pair
            for j, (e1, e2) in _corners(r[3]).items():
                B = r[3][j]
                if e1 == d1 and e2 == d2 and math.hypot(B[0] - A[0], B[1] - A[1]) < 1.5:
                    off = ((B[0] - A[0]) * (d1[0] - d2[0]) + (B[1] - A[1]) * (d1[1] - d2[1])) / 2
                    if off < 0:                # this line is the outer one
                        c += -off * (2 - math.sqrt(2))
        l_in = _unit(pts[i - 1], A)[1]
        l_out = _unit(A, pts[i + 1])[1]
        c = min(c, l_in / 2 if i - 1 in corners else l_in - 0.02,
                l_out / 2 if i + 1 in corners else l_out - 0.02)
        out.append((round(A[0] - c * d1[0], 4), round(A[1] - c * d1[1], 4)))
        out.append((round(A[0] + c * d2[0], 4), round(A[1] + c * d2[1], 4)))
    out.append(pts[-1])
    return out


def track(board, net, layer, width, pts):
    n = board.FindNet(net)
    if n is None:
        raise SystemExit("no net " + net)
    out = []
    for a, b in zip(pts, pts[1:]):
        if abs(a[0] - b[0]) < 1e-6 and abs(a[1] - b[1]) < 1e-6:
            continue
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(P(*a))
        t.SetEnd(P(*b))
        t.SetLayer(layer)
        t.SetWidth(mm(width))
        t.SetNet(n)
        board.Add(t)
        out.append(t)
    return out


def via(board, net, x, y, d=VIA_D, drill=VIA_DRILL):
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(P(x, y))
    v.SetWidth(mm(d))
    v.SetDrill(mm(drill))
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    v.SetNet(board.FindNet(net))
    board.Add(v)
    return v


# ------------------------------------------------------------------------------------------------
class Grid:
    """Occupancy per routing layer: 0 free, >0 the only net allowed there, -1 nobody."""

    def __init__(self, board):
        bb = board.GetBoardEdgesBoundingBox()
        # grid on round 0.1 mm board coordinates, so 0.5 mm-pitch pads (W5500) sit on grid lines
        self.x0, self.y0 = math.floor(um(bb.GetLeft()) * 10) / 10, math.floor(um(bb.GetTop()) * 10) / 10
        self.nx = int(math.ceil(um(bb.GetWidth()) / G)) + 1
        self.ny = int(math.ceil(um(bb.GetHeight()) / G)) + 1
        self.occ = [array("i", [0]) * (self.nx * self.ny) for _ in LAYERS]
        self.novia = bytearray(self.nx * self.ny)      # 1: no via centre here (holes, own SMD pads)
        self.holes = []                                  # (x, y, r) for hole-to-hole
        self.board = board

    def idx(self, i, j):
        return j * self.nx + i

    def cell(self, x, y):
        return int(round((x - self.x0) / G)), int(round((y - self.y0) / G))

    def xy(self, i, j):
        return self.x0 + i * G, self.y0 + j * G

    def claim(self, li, poly, net, grow):
        """Marks the cells inside poly grown by grow (mm) for net (or -1)."""
        p = pcbnew.SHAPE_POLY_SET(poly)
        if grow > 0:
            p.Inflate(mm(grow), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, mm(0.01))
        bb = p.BBox()
        i0, j0 = self.cell(um(bb.GetLeft()), um(bb.GetTop()))
        i1, j1 = self.cell(um(bb.GetRight()), um(bb.GetBottom()))
        occ = self.occ[li]
        for j in range(max(j0 - 1, 0), min(j1 + 2, self.ny)):
            for i in range(max(i0 - 1, 0), min(i1 + 2, self.nx)):
                x, y = self.xy(i, j)
                if p.Contains(P(x, y)):
                    k = self.idx(i, j)
                    o = occ[k]
                    if o == 0:
                        occ[k] = net
                    elif o != net:
                        occ[k] = -1

    def block_outside_board(self):
        outline = pcbnew.SHAPE_POLY_SET()
        self.board.GetBoardPolygonOutlines(outline, True)
        inner = pcbnew.SHAPE_POLY_SET(outline)
        inner.Deflate(mm(EDGE + MARGIN), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, mm(0.01))
        for j in range(self.ny):
            for i in range(self.nx):
                if not inner.Contains(P(*self.xy(i, j))):
                    k = self.idx(i, j)
                    for occ in self.occ:
                        occ[k] = -1


def build_grid(board):
    g = Grid(board)
    g.block_outside_board()
    # rule areas (the module's antenna keep-out): every layer, nobody
    for fp in board.GetFootprints():
        for z in fp.Zones():
            if z.GetIsRuleArea():
                for li in range(len(LAYERS)):
                    g.claim(li, z.Outline(), -1, CLEAR)
    for z in board.Zones():
        if z.GetIsRuleArea():
            for li in range(len(LAYERS)):
                g.claim(li, z.Outline(), -1, CLEAR)
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            net = pad.GetNetCode() or -1
            local = pad.GetLocalClearance()                   # e.g. the fiducials' 0.6 mm
            clear = max(CLEAR, pcbnew.ToMM(local)) if local else CLEAR
            for li, layer in enumerate(LAYERS):
                if pad.IsOnLayer(layer):
                    g.claim(li, pad.GetEffectivePolygon(layer), net, clear + MARGIN)
            if pad.HasHole():
                c = pad.GetPosition()
                r = um(max(pad.GetDrillSize().x, pad.GetDrillSize().y)) / 2
                g.holes.append((um(c.x), um(c.y), r))
                if pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
                    hole = pcbnew.SHAPE_POLY_SET()
                    pad.TransformHoleToPolygon(hole, 0, mm(0.01), pcbnew.ERROR_INSIDE)
                    for li in range(len(LAYERS)):
                        g.claim(li, hole, -1, EDGE + MARGIN)   # NPTH counts as board edge
            elif pad.IsOnLayer(pcbnew.F_Cu):
                # no via inside an SMD pad of its own net
                poly = pad.GetEffectivePolygon(pcbnew.F_Cu)
                bb = poly.BBox()
                i0, j0 = g.cell(um(bb.GetLeft()), um(bb.GetTop()))
                i1, j1 = g.cell(um(bb.GetRight()), um(bb.GetBottom()))
                for j in range(max(j0 - 3, 0), min(j1 + 4, g.ny)):
                    for i in range(max(i0 - 3, 0), min(i1 + 4, g.nx)):
                        g.novia[g.idx(i, j)] = 1
    for t in board.GetTracks():
        add_item(g, t)
    return g


def add_item(g, t):
    net = t.GetNetCode() or -1
    if t.GetClass() == "PCB_VIA":
        c = t.GetPosition()
        g.holes.append((um(c.x), um(c.y), VIA_DRILL / 2))
        for li, layer in enumerate(LAYERS):
            poly = pcbnew.SHAPE_POLY_SET()
            t.TransformShapeToPolygon(poly, layer, 0, mm(0.01), pcbnew.ERROR_INSIDE)
            g.claim(li, poly, net, CLEAR + MARGIN)
    else:
        layer = t.GetLayer()
        if layer in LAYERS:
            poly = pcbnew.SHAPE_POLY_SET()
            t.TransformShapeToPolygon(poly, layer, 0, mm(0.01), pcbnew.ERROR_INSIDE)
            g.claim(LAYERS.index(layer), poly, net, CLEAR + MARGIN)


def dilate(mask, nx, ny, r):
    """Square dilation of a bytearray mask by r cells (separable max filter)."""
    if r <= 0:
        return mask
    out = bytearray(len(mask))
    for j in range(ny):
        row = mask[j * nx:(j + 1) * nx]
        idx = [i for i in range(nx) if row[i]]
        base = j * nx
        for i in idx:
            a, b = max(i - r, 0), min(i + r + 1, nx)
            out[base + a:base + b] = b"\x01" * (b - a)
    res = bytearray(len(mask))
    for j in range(ny):
        base = j * nx
        if not any(out[base:base + nx]):
            continue
        for jj in range(max(j - r, 0), min(j + r + 1, ny)):
            b2 = jj * nx
            seg = out[base:base + nx]
            cur = res[b2:b2 + nx]
            res[b2:b2 + nx] = bytes(x | y for x, y in zip(cur, seg))
    return res


def prepare(g, net, width, b_ok):
    """Cells a track of this net and width may not use, and cells a via may not use."""
    nx, ny = g.nx, g.ny
    r = int(math.ceil((width / 2) / G - 1e-6))
    rv = int(math.ceil((VIA_D / 2) / G - 1e-6))
    free = []
    for li in range(len(LAYERS)):
        occ = g.occ[li]
        foreign = bytearray(1 if (o != 0 and o != net) else 0 for o in occ)
        free.append(dilate(foreign, nx, ny, r))
    viablk = bytearray(len(free[0]))
    for li in range(len(LAYERS)):
        occ = g.occ[li]
        foreign = bytearray(1 if (o != 0 and o != net) else 0 for o in occ)
        d = dilate(foreign, nx, ny, rv)
        viablk = bytearray(a | b for a, b in zip(viablk, d))
    if not b_ok:
        free[2] = bytearray(b"\x01") * len(free[2])
    return free, viablk


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = 0.0 if dx == dy == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def plane_vias(board, g, net_name, width=0.3):
    """A via beside every SMD pad of a plane net (GND to In1/B, +3V3 to In2), joined by a short track."""
    n = board.FindNet(net_name)
    code = n.GetNetCode()
    maps = {width: prepare(g, code, width, False), 0.2: prepare(g, code, 0.2, False)}
    wide = width
    placed, missing = 0, []
    existing = [(um(v.GetPosition().x), um(v.GetPosition().y)) for v in board.GetTracks()
                if v.GetClass() == "PCB_VIA" and v.GetNetCode() == code]
    hand_gnd = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK" and t.GetNetCode() == code
                and t.GetLayer() == pcbnew.F_Cu]
    new_vias, new_stubs = [], []
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNetCode() != code or pad.HasHole() or not pad.IsOnLayer(pcbnew.F_Cu):
                continue
            if fp.GetReference() in NO_PLANE_VIA:
                continue
            c = pad.GetPosition()
            cx, cy = um(c.x), um(c.y)
            bb = pad.GetBoundingBox()
            half = max(um(bb.GetWidth()), um(bb.GetHeight())) / 2
            width = wide if min(um(bb.GetWidth()), um(bb.GetHeight())) >= 0.5 else 0.2   # fine pitch
            if any((cx - x) ** 2 + (cy - y) ** 2 < (half + 1.0) ** 2 for x, y in existing) or                     any(bb.Contains(t.GetStart()) or bb.Contains(t.GetEnd()) for t in hand_gnd):
                continue                                  # a hand-placed ground via or track already serves it
            free, viablk = maps[width]
            done = False
            for dist in [half + 0.45 + k * 0.1 for k in range(32)]:
                for a in range(0, 360, 45):                  # stubs at 45-degree steps only
                    x = cx + dist * math.cos(math.radians(a))
                    y = cy + dist * math.sin(math.radians(a))
                    i, j = g.cell(x, y)
                    if not (0 <= i < g.nx and 0 <= j < g.ny):
                        continue
                    k = g.idx(i, j)
                    if viablk[k] or g.novia[k] or free[0][k]:
                        continue
                    vx, vy = x, y                         # exactly on the 45-degree ray (cell checked)
                    if in_art(vx, vy) and not fp.GetReference().startswith(ART_PAD_VIA_OK):
                        continue
                    if any((hx - vx) ** 2 + (hy - vy) ** 2 < (hr + VIA_DRILL / 2 + HOLE_TO_HOLE) ** 2
                           for hx, hy, hr in g.holes):
                        continue
                    # the stub from the pad to the via must stay on free cells
                    steps = int(dist / G) + 1
                    ok = True
                    for s in range(steps + 1):
                        px = cx + (vx - cx) * s / steps
                        py = cy + (vy - cy) * s / steps
                        pi, pj = g.cell(px, py)
                        if free[0][g.idx(pi, pj)]:
                            ok = False
                            break
                    if not ok:
                        continue
                    # keep clear of the vias and stubs placed in this pass (the maps are not rebuilt)
                    if any((px - vx) ** 2 + (py - vy) ** 2 < 0.8 ** 2 for px, py in new_vias):
                        continue
                    if any(seg_dist(vx, vy, *s) < VIA_D / 2 + CLEAR + width / 2 + 0.02 for s in new_stubs):
                        continue
                    items = track(board, net_name, pcbnew.F_Cu, width, [(cx, cy), (vx, vy)])
                    items.append(via(board, net_name, vx, vy))
                    for t in items:
                        add_item(g, t)
                    new_vias.append((vx, vy))
                    new_stubs.append((cx, cy, vx, vy))
                    placed += 1
                    done = True
                    break
                if done:
                    break
            if not done:
                missing.append("%s.%s" % (fp.GetReference(), pad.GetNumber()))
    print("%s: %d plane vias, none for %s" % (net_name, placed, " ".join(missing) or "-"))


# ------------------------------------------------------------------------------------------------
# Keep-outs, stitching, pours

# B-side art block (gen_board.art_layout, silkscreen extent): stitching vias under it are the small
# ones, tented like every via
ART = (113.4, 124.8, 148.0, 136.6)
VIA_SMALL = (0.5, 0.3)                # 0.3 mm drill everywhere: JLC's free tier (CAM.md)
# No stitching vias in the art block plus 1 mm: each via dimples the black mask on the art side.
# Pad vias stay only where the electrical case needs them (decoupling caps, the USB ESD array);
# other ground pads there join the planes through the pours.
ART_PAD_VIA_OK = ("C", "U202")
ART_CLEAR = (ART[0] - 1.0, ART[1] - 1.0, ART[2] + 1.0, ART[3] + 1.0)


def in_art(x, y, r=0.3):
    return ART_CLEAR[0] - r < x < ART_CLEAR[2] + r and ART_CLEAR[1] - r < y < ART_CLEAR[3] + r
# RJ45 (HY911105AE) cable side: no copper between the board edge and the jack's LED pins, on any
# layer (DESIGN.md 4.3: the shield and pin 8 tie to ground at their own pins)
RJ45_KEEPOUT = (148.3, 113.0, 168.0, 119.0)
FENCE_NETS = ("/MCU/USB_DN", "/MCU/USB_DP", "ETH_CS", "ETH_SCLK", "ETH_MISO", "ETH_MOSI", "ETH_INT", "ETH_RST")
FENCE_STEP, FENCE_OFFSET = 1.5, 0.62    # along the In2 lines, sideways from their centre
EDGE_STEP, EDGE_INSET = 2.5, 0.8
GRID_STEP = 3.5                          # general In1-B stitching


def keepouts(board):
    x0, y0, x1, y1 = RJ45_KEEPOUT
    z = pcbnew.ZONE(board)
    z.SetIsRuleArea(True)
    z.SetLayerSet(pcbnew.LSET.AllCuMask())
    z.SetDoNotAllowTracks(True)
    z.SetDoNotAllowVias(True)
    z.SetDoNotAllowZoneFills(True)
    z.SetDoNotAllowPads(False)
    z.SetDoNotAllowFootprints(False)
    z.SetZoneName("RJ45 cable side")
    ol = z.Outline()
    ol.NewOutline()
    for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
        ol.Append(mm(x), mm(y))
    board.Add(z)


def stitch(board, g):
    """GND vias joining In1 and B (and the F/In2 pours): fences along the In2 USB and SPI lines,
    one beside every signal via (its return path changes reference plane there), the board
    edge, and a coarse grid everywhere else."""
    code = board.FindNet("GND").GetNetCode()
    free, viablk = prepare(g, code, 0.3, False)
    gnd = [(um(v.GetPosition().x), um(v.GetPosition().y)) for v in board.GetTracks()
           if v.GetClass() == "PCB_VIA" and v.GetNetCode() == code]
    counts = {}

    def ok(x, y, spacing):
        if in_art(x, y):
            return False
        i, j = g.cell(x, y)
        if not (0 <= i < g.nx and 0 <= j < g.ny):
            return False
        k = g.idx(i, j)
        if viablk[k] or g.novia[k]:
            return False
        if any((hx - x) ** 2 + (hy - y) ** 2 < (hr + VIA_DRILL / 2 + HOLE_TO_HOLE) ** 2 for hx, hy, hr in g.holes):
            return False
        return all((gx - x) ** 2 + (gy - y) ** 2 >= spacing ** 2 for gx, gy in gnd)

    def put(x, y, kind):
        art = ART[0] <= x <= ART[2] and ART[1] <= y <= ART[3]
        d, drill = VIA_SMALL if art else (VIA_D, VIA_DRILL)
        v = via(board, "GND", x, y, d, drill)
        add_item(g, v)
        gnd.append((x, y))
        counts[kind] = counts.get(kind, 0) + 1

    # fences
    for t in list(board.GetTracks()):
        if t.GetClass() != "PCB_TRACK" or t.GetLayer() != pcbnew.In2_Cu or t.GetNetname() not in FENCE_NETS:
            continue
        ax, ay, bx, by = um(t.GetStart().x), um(t.GetStart().y), um(t.GetEnd().x), um(t.GetEnd().y)
        n = math.hypot(bx - ax, by - ay)
        if n < 0.5:
            continue
        ux, uy = (bx - ax) / n, (by - ay) / n
        for s in range(int(n / FENCE_STEP) + 1):
            px, py = ax + ux * s * FENCE_STEP, ay + uy * s * FENCE_STEP
            for side in (1, -1):
                x, y = px - uy * FENCE_OFFSET * side, py + ux * FENCE_OFFSET * side
                x, y = g.xy(*g.cell(x, y))
                if ok(x, y, 1.2):
                    put(x, y, "fence")
    # beside every signal via
    for v in list(board.GetTracks()):
        if v.GetClass() != "PCB_VIA" or v.GetNetCode() == code:
            continue
        cx, cy = um(v.GetPosition().x), um(v.GetPosition().y)
        if any((gx - cx) ** 2 + (gy - cy) ** 2 < 1.6 ** 2 for gx, gy in gnd):
            continue
        done = False
        for r in [0.9 + 0.1 * k for k in range(8)]:
            for a in range(0, 360, 15):
                x, y = g.xy(*g.cell(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))))
                if ok(x, y, 0.85):
                    put(x, y, "at signal vias")
                    done = True
                    break
            if done:
                break
    # board edge
    outline = pcbnew.SHAPE_POLY_SET()
    board.GetBoardPolygonOutlines(outline, True)
    inner = pcbnew.SHAPE_POLY_SET(outline)
    inner.Deflate(mm(EDGE_INSET), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, mm(0.01))
    o = inner.Outline(0)
    pts = [(um(o.CPoint(i).x), um(o.CPoint(i).y)) for i in range(o.PointCount())]
    acc = 0.0
    for (ax, ay), (bx, by) in zip(pts, pts[1:] + pts[:1]):
        n = math.hypot(bx - ax, by - ay)
        s = (EDGE_STEP - acc) if acc else 0.0
        while s <= n:
            x, y = g.xy(*g.cell(ax + (bx - ax) * s / n, ay + (by - ay) * s / n))
            if ok(x, y, 1.5):
                put(x, y, "edge")
            s += EDGE_STEP
        acc = (acc + n) % EDGE_STEP
    # coarse grid
    bb = board.GetBoardEdgesBoundingBox()
    y = um(bb.GetTop()) + GRID_STEP / 2
    while y < um(bb.GetBottom()):
        x = um(bb.GetLeft()) + GRID_STEP / 2
        while x < um(bb.GetRight()):
            for dx, dy in ((0, 0), (0.4, 0), (-0.4, 0), (0, 0.4), (0, -0.4), (0.4, 0.4), (-0.4, -0.4)):
                xx, yy = g.xy(*g.cell(x + dx, y + dy))
                if ok(xx, yy, 2.0):
                    put(xx, yy, "grid")
                    break
            x += GRID_STEP
        y += GRID_STEP
    print("GND stitching:", ", ".join("%d %s" % (n, k) for k, n in counts.items()))


def zones(board):
    """Ground pours on all four layers (In2 fills around its signals); the rule areas keep them out
    of the antenna area and the RJ45 cable side on every layer."""
    outline = pcbnew.SHAPE_POLY_SET()
    board.GetBoardPolygonOutlines(outline, True)
    for layer in (pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(board.FindNet("GND"))
        z.SetZoneName("GND %s" % board.GetLayerName(layer))
        z.Outline().AddOutline(outline.Outline(0))
        z.SetLocalClearance(mm(0.2))
        z.SetMinThickness(mm(0.2))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THT_THERMAL)   # SMD pads solid, through-hole pins on spokes
        z.SetThermalReliefGap(mm(0.25))
        z.SetThermalReliefSpokeWidth(mm(0.3))
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        board.Add(z)
    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())


def main(hand_only=False):
    board = pcbnew.LoadBoard(BOARD)
    if len(board.GetTracks()):
        raise SystemExit("board already has tracks: regenerate it (gen_board, place, title_font) first")
    routes = [item for item in HAND if item[1] != "via"]
    for item in HAND:
        if item[1] == "via":
            via(board, item[0], item[2][0], item[2][1], item[3], item[4])
        else:
            track(board, item[0], LAYER_NAMES[item[1]], item[2],
                  mitred(item[0], item[1], item[2], item[3], routes))
    if not hand_only:
        keepouts(board)
        g = build_grid(board)
        plane_vias(board, g, "GND")
        stitch(board, g)
        zones(board)
    pcbnew.SaveBoard(BOARD, board)
    print("hand routes" + ("" if hand_only else ", plane vias, stitching, pours"))


# DNP parts get no plane vias of their own (their pads still join the pours)
NO_PLANE_VIA = {"J202", "J203", "D102"}

if __name__ == "__main__":
    main("--hand" in sys.argv[1:])
