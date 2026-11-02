"""Floorplan: puts every footprint of the schematic netlist at its planned position.

    hardware/tools/kicad.sh sh -c 'kicad-cli sch export netlist -o /project/.cache/hardware/ssh-runner.net ssh-runner.kicad_sch && python3 tools/gen_board.py --force && python3 tools/place.py /project/.cache/hardware/ssh-runner.net'

Coordinates are the F (parts) view in mm: x to the right, y down; the board spans x 100..167.34,
the tab edge is at y 150 and the top edge at y TOP. The F side faces the back of the Flipper
(DESIGN.md 5.2). Every part has a fixed place chosen for its function (DESIGN.md 5.4):

    RF/MCU      module upright at the left end, antenna at the top edge; 3V3/EN parts at pins 2/3
    USB         USB-C at the top edge; ESD and CC resistors at its pins
    Ethernet    W5500 next to the RJ45, PHY pins facing the jack; 3V3A bead and decoupling above
                the chip, crystal at XI/XO, termination network under the jack
    Power       right of the module, below the antenna keep-out
    LEDs        reverse-mount, shining through holes to the art side, two dots above the title
    Flipper     headers at the official positions; UART series resistors at pins 13/14
    Back        the art only (gen_board.py)

Also writes a fit-check copy with Flipper's body model to .cache/hardware/fit/ (registered
through Flipper's own reference module, see FLIPPER_FRAME and tools/fit_check.py). Afterwards,
tools/title_font.py Orbitron ... --bold turns the title into Orbitron Bold outlines in both
boards.
Run gen_board.py --force first: pcbnew cannot reliably remove footprints from a placed board.
"""
import os
import re
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from schlib import parse, find, findall  # noqa: E402
from gen_board import art_layout  # noqa: E402

HW = os.path.join(HERE, "..")
BOARD = os.environ.get("BOARD_OUT", os.path.join(HW, "ssh-runner.kicad_pcb"))   # BOARD_OUT: studies only
FIT = os.environ.get("FIT_OUT", "/project/.cache/hardware/fit/ssh-runner-fit.kicad_pcb")
FPDIR = "/usr/share/kicad/footprints"

HOLE_Y = 150.0 - 3.84                        # header hole row (official drawing)
PIN1_X = 100.0 + 182.103088 - 119.233093    # Flipper pin 1 (mirrored DXF)
PIN9_X = 100.0 + 182.103088 - 154.793091    # Flipper pin 9


def floorplan(top):
    """ref -> (x, y, rotation, side, anchor); anchor "o" = footprint origin, "c" = courtyard centre."""
    t = top
    art = art_layout()
    p = {
        # --- mechanics: Flipper headers (origin = pin 1), module, connectors
        "J401": (PIN1_X, HOLE_Y, 270, "F", "o"),
        "J402": (PIN9_X, HOLE_Y, 270, "F", "o"),
        "U201": (112.25, t + 12.8, 0, "F", "o"),          # antenna flush with the top edge
        "J301": (162.26, t + 17.31, 180, "F", "o"),       # RJ45, opening up, flush right
        "J201": (142.28, t + 3.7, 180, "F", "o"),         # USB-C, opening up
        "U301": (142.8, t + 24.3, 180, "F", "o"),         # W5500, PHY pins toward the RJ45
        # --- RF/MCU: strip left of the module, at pins 2 (3V3) and 3 (EN)
        #     top to bottom: 100 nF level with pin 2, 22 uF under it, then the EN pull-up and delay,
        #     each fed through a via (3V3 rail and EN both on In2)
        "C202": (101.6, t + 8.33, 90, "F", "c"),          # 100 nF at 3V3, 3V3 pad level with pin 2
        "C201": (101.5, t + 11.0, 270, "F", "c"),         # 22 uF bulk at 3V3
        "R201": (101.6, t + 14.35, 270, "F", "c"),        # EN pull-up
        "C203": (101.6, t + 16.35, 270, "F", "c"),        # EN delay
        # --- UART1 series resistors next to pins 10/11; the lines run down this strip to the header
        "R402": (100.85, t + 19.35, 270, "F", "c"),       # TX (pin 10), outer lane
        "R401": (101.95, t + 21.3, 90, "F", "c"),         # RX (pin 11), inner lane
        # --- below the module: reset, status LED, SPI series resistors at the source
        "SW201": (106.45, t + 28.4, 0, "F", "c"),         # RESET
        # --- DNP links above Flipper pins 16 (PC0, EN) and 15 (PC1, BOOT)
        "R403": (110.7, t + 29.3, 180, "F", "c"),
        "R404": (112.8, t + 29.3, 0, "F", "c"),
        # --- right of the module: BOOT, Qwiic (DNP) at the I2C pins
        "SW202": (125.4, t + 27.9, 0, "F", "c"),          # BOOT, near IO0 (pin 27)
        "R202": (122.95, t + 23.65, 270, "F", "c"),       # BOOT pull-up beside pin 27, 3V3 from J203 pad 9
        "J202": (128.05, t + 22.8, 0, "F", "c"),          # pads 1 mm below the status LED hole: room for their vias
        # --- expansion / UART0 pads facing the module's right column, I2C pull-ups at pins 39/38
        "J203": (123.25, t + 11.3, 0, "F", "o"),          # pad 1 faces pin 37 (U0TXD); a via fits between
        "R206": (122.6, t + 7.6, 270, "F", "c"),          # 3V3 pads up, through a via to the In2 rail
        "R207": (123.65, t + 7.6, 270, "F", "c"),
        # --- power: right of the module, below the antenna keep-out
        #     flow from the USB side: D101 -> LDO U101 -> mux U102 -> output caps, left to right reversed
        "D101": (134.45, t + 7.4, 0, "F", "c"),           # anode toward the USB-C VBUS pins
        "U101": (129.9, t + 8.05, 180, "F", "c"),         # inputs face D101, output toward the mux
        "C101": (133.5, t + 10.95, 0, "F", "c"),          # LDO input
        "R104": (133.8, t + 9.5, 0, "F", "c"),            # V5_IN bleed, between D101 and C101
        "C102": (130.35, t + 10.95, 0, "F", "c"),         # LDO output
        "U102": (125.9, t + 7.9, 180, "F", "c"),          # VIN1 faces the LDO; VIN2 from Flipper pin 9
        "C105": (124.55, t + 11.6, 270, "F", "c"),        # VIN2 (Flipper 3V3), at its via
        "R101": (135.6, t + 10.6, 90, "F", "c"),          # PR1 divider
        "R102": (136.7, t + 10.6, 270, "F", "c"),
        "C104": (125.9, t + 11.0, 270, "F", "c"),         # +3V3 output, under the mux's output via
        "C103": (127.85, t + 11.5, 270, "F", "c"),        # +3V3 output bulk 22 uF
        # --- LEDs: reverse-mount 1206, light through a hole to the art side.
        #     The holes are part of the art block (gen_board.art_layout): on the revision's line,
        #     flush with the end of the title, power then status as seen from the front.
        "D103": art["led_power"] + (0, "F", "o"),         # power, white
        "R103": (art["led_power"][0] + 3.1, art["led_power"][1], 90, "F", "c"),
        "D201": art["led_status"] + (180, "F", "o"),      # status, green, IO21
        "R203": (art["led_status"][0] - 3.1, art["led_status"][1], 90, "F", "c"),
        # --- USB: ESD and CC resistors at the connector pins
        "U202": (142.3, t + 11.6, 0, "F", "c"),
        "C204": (144.95, t + 12.2, 270, "F", "c"),        # 100 nF at U202 pin 5 (VBUS), pad 1 on top
        "R204": (145.9, t + 11.0, 270, "F", "c"),         # CC1, CC pad on top
        "R205": (138.6, t + 11.0, 270, "F", "c"),         # CC2, CC pad on top
        "R307": (147.4, t + 12.0, 270, "F", "c"),         # LINK LED, cathode pad up toward the jack's LED pins
        # --- Ethernet: 3V3A bead + decoupling rows between the ESD and the W5500 top pins
        "FB301": (139.3, t + 14.6, 0, "F", "c"),
        "R308": (147.4, t + 14.6, 90, "F", "c"),          # ACT LED
        #     W5500 top side as a comb: each AVDD/V1V2/TOCAP pin runs straight up its own lane,
        #     the caps hang on the lanes (AVDD pad on the lane, GND pad beside it on a via), and the
        #     AVDD bus from FB301 closes the lanes at y 128.55
        "C305": (145.33, t + 16.05, 0, "F", "c"),         # 10 uF 3V3A, on the pin 15/17 lane
        "C313": (145.03, t + 17.35, 0, "F", "c"),         # pin 15/17 lane
        "C307": (145.03, t + 18.4, 0, "F", "c"),          # AVDD pins 15/17
        "C303": (142.8, t + 17.6, 90, "F", "c"),          # TOCAP 4.7 uF, on pin 20's lane
        "C309": (148.95, t + 22.4, 0, "F", "c"),          # AVDD pin 11, at the pin
        "C306": (141.07, t + 16.45, 180, "F", "c"),       # AVDD pin 21
        "C308": (148.95, t + 23.55, 0, "F", "c"),         # AVDD pin 8, at the pin
        "C304": (139.9, t + 18.45, 180, "F", "c"),        # 1V2O 10 nF, beside pin 22
        "C312": (149.6, t + 25.8, 0, "F", "c"),           # AVDD pin 4, at the pin, between the pairs
        "R306": (142.25, t + 30.15, 180, "F", "c"),          # EXRES1 12.4 k, below the chip, west of the header (EXRES leaves pin 10 inward)
        # --- Ethernet: left of the W5500 (VDD, crystal at XI/XO, MISO series, RST pull-up)
        "C311": (134.6, t + 20.0, 0, "F", "c"),           # 10 uF VDD
        "C310": (136.83, t + 22.45, 90, "F", "c"),        # 100 nF at VDD (pin 28), upright between crystal and chip courtyards
        # crystal left of XI/XO (pins 30/31): XI leaves through the gap between the pad rows at the
        # height of pin 30, XO straight from the lower right pad to pin 31; the SPI pins below
        # fan out to staggered vias between the crystal and the chip
        "Y301": (134.45, t + 24.05, 270, "F", "o"),
        "C301": (133.6, t + 21.45, 180, "F", "c"),          # XI load, on the XI pad
        "C302": (134.7, t + 26.7, 0, "F", "c"),          # XO load, below the crystal
        "R305": (137.05, t + 24.3, 270, "F", "c"),         # 1 M across XI/XO, bridging the two lines at the chip
        "R304": (139.6, t + 30.2, 0, "F", "c"),           # RST pull-up, under pin 37
        # --- Ethernet: termination along the two pairs (DESIGN.md 5.5). Both pairs leave the W5500 east
        #     on F; RX turns north into its coupling caps below jack pins 8/6, TX turns north into jack
        #     pins 2/1. The RX shunts sit on either line before the turn, the TX pull-ups above and
        #     below the pair before its turn; the centre taps reach the jack on In2.
        "R309": (157.0, t + 26.295, 270, "F", "c"),        # TXP 49.9 to 3V3A, above the TX pair
        "C316": (155.6, t + 22.9, 90, "F", "c"),          # RX coupling 6.8 nF (RXN)
        "R313": (154.1, t + 25.76, 270, "F", "c"),        # RXN 49.9 to RCT, below the RX pair, beside R312
        "C317": (151.7, t + 22.6, 180, "F", "c"),         # RCT 10 nF
        "R312": (153.1, t + 23.84, 90, "F", "c"),         # RXP 49.9 to RCT, above the RX pair
        "C315": (154.65, t + 22.9, 90, "F", "c"),         # RX coupling 6.8 nF (RXP), side by side with C316
        "R310": (158.0, t + 27.96, 90, "F", "c"),         # TXN 49.9 to 3V3A, below the TX pair
        "R311": (159.75, t + 28.65, 90, "F", "c"),         # TCT 10R to 3V3A
        "C314": (160.75, t + 28.65, 270, "F", "c"),         # TCT 22 nF
        # --- DNP 5 V fallback diode at Flipper pin 1 (5V), right of the TX pair
        "D102": (164.5, t + 27.95, 270, "F", "c"),
    }
    return p


# LCSC/EasyEDA models for the parts KiCad's library has none for (hardware/3d, tools/lcsc_3d.py):
# ref -> (file, offset mm, rotation deg), placed by the model's own legs and pins on our pads
LCSC_MODELS = {
    "J201": ("USB_C_Receptacle_HRO_TYPE-C-31-M-12.wrl", (0, -1.04, 0), (0, 0, 180)),
    "J301": ("RJ45_HanRun_HY911105AE.wrl", (4.445, -6.55, 0), (0, 0, 0)),
    "D103": ("LED_XINGLIGHT_XL-3216UWC-FB.wrl", (0, 0, 0.29), (0, 0, 180)),
    "D201": ("LED_XINGLIGHT_XL-3216UGC-FB.wrl", (0, 0, 0), (0, 0, 0)),
}

# Test pads on the parts side, on the Flipper header's 2.54 mm grid between its two groups
# (x0, y, pitch). The back carries only the art and the unavoidable through-hole pins.
TP_ROW = (PIN9_X + 2 * 2.54, HOLE_Y, 2.54)   # the four inner grid positions: the outer two would touch the header courtyards

# No reference designators on the silkscreen: they stay on F.Fab, the assembly drawing. The parts
# a person handles get function labels instead (SILK_LABELS).
SILK_REF_PREFIXES = ()
SILK_REF_SIZE = 1.0      # JLC minimum text height (stroke 0.15 mm)
# Function labels on the parts-side silkscreen, in place of these parts' designators (which stay on
# F.Fab): what a person handling the board looks for
SILK_LABELS = {"SW201": "RESET", "SW202": "BOOT", "J201": "USB", "J301": "ETH", "J202": "QWIIC"}

# Global fiducials for JLC's assembly: three on F, clear of the tall parts and of the RESET label (the
# left one sits below it, above the outline's step, on the side that gets a panel rail)
FIDUCIALS = [(129.9, 142.4), (163.0, 137.6), (101.35, 145.6)]
FIDUCIAL_FP = "Fiducial:Fiducial_1mm_Mask2mm"
# Footprints whose silkscreen outline runs past the board edge by design (headers, module,
# USB-C): project copies in ssh_runner.pretty move those silkscreen items to User.Drawings, so
# nothing is clipped at the edge. The schematic points at the copies (<stock name>_EdgeTrim).
EDGE_TRIM = {
    "U201": "RF_Module:ESP32-S3-WROOM-1",
    "J201": "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
    "J401": "Connector_PinHeader_2.54mm:PinHeader_1x08_P2.54mm_Horizontal",
    "J402": "Connector_PinHeader_2.54mm:PinHeader_1x10_P2.54mm_Horizontal",
}


def add_fiducials(board):
    for i, (x, y) in enumerate(FIDUCIALS, 1):
        fp = load(FIDUCIAL_FP)
        fp.SetFPID(pcbnew.LIB_ID(*FIDUCIAL_FP.split(":")))
        fp.SetReference("FID%d" % i)
        fp.SetValue("Fiducial")
        fp.SetBoardOnly(True)
        fp.SetExcludedFromBOM(True)
        fp.SetExcludedFromPosFiles(True)
        fp.Reference().SetVisible(False)
        fp.Value().SetVisible(False)
        board.Add(fp)
        fp.SetPosition(mm(x, y))


def edge_trimmed(board, plan):
    """Writes the EDGE_TRIM copies: each stock footprint with the silkscreen items that reach within
    0.1 mm of the board edge at its planned position moved to User.Drawings."""
    outline = pcbnew.SHAPE_POLY_SET()
    board.GetBoardPolygonOutlines(outline, True)
    inner = pcbnew.SHAPE_POLY_SET(outline)
    inner.Deflate(pcbnew.FromMM(0.1), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, pcbnew.FromMM(0.01))
    for ref, stock in EDGE_TRIM.items():
        x, y, rot, side, anchor = plan[ref]
        placed = load(stock)
        placed.SetOrientationDegrees(rot)
        placed.SetPosition(mm(x, y))
        drop = []
        for i, item in enumerate(placed.GraphicalItems()):
            if item.GetLayer() not in (pcbnew.F_SilkS, pcbnew.B_SilkS):
                continue
            poly = pcbnew.SHAPE_POLY_SET()
            item.TransformShapeToPolygon(poly, item.GetLayer(), 0, pcbnew.FromMM(0.01), pcbnew.ERROR_INSIDE)
            poly.BooleanSubtract(inner)
            if poly.OutlineCount():
                drop.append(i)
        fp = load(stock)
        items = list(fp.GraphicalItems())
        for i in drop:
            items[i].SetLayer(pcbnew.Dwgs_User)     # kept as a drawing, off the silkscreen
        for pad in fp.Pads():                       # no drill below 0.3 mm (JLC's free tier, CAM.md)
            d = pad.GetDrillSize()
            if pad.HasHole() and min(d.x, d.y) < pcbnew.FromMM(0.3):
                pad.SetDrillSize(pcbnew.VECTOR2I(max(d.x, pcbnew.FromMM(0.3)), max(d.y, pcbnew.FromMM(0.3))))
        name = stock.split(":")[1] + "_EdgeTrim"
        fp.SetFPID(pcbnew.LIB_ID("ssh_runner", name))
        # the old text is kept when only the item UUIDs (new on every save) changed
        path = os.path.join(HW, "ssh_runner.pretty", name + ".kicad_mod")
        old = open(path, encoding="utf-8", newline="").read() if os.path.exists(path) else ""
        pcbnew.FootprintSave(os.path.join(HW, "ssh_runner.pretty"), fp)
        new = open(path, encoding="utf-8", newline="").read()
        uuid = re.compile(r'\(uuid "[^"]*"\)')
        if old and uuid.sub("", old) == uuid.sub("", new):
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(old)

# functional areas drawn on User.Drawings (x0, y0 offset from top, x1, y1 offset, label)
AREAS = [
    (100.3, 6.4, 121.9, 26.3, "RF / MCU"),
    (122.2, 6.4, 136.9, 16.0, "POWER"),
    (133.0, 9.3, 148.1, 30.3, "ETHERNET"),
    (102.6, 27.0, 129.2, 30.5, "FLIPPER I/F"),
]


# Flipper's body STEP and its reference module STEP (external-module-pcb-3d-with-headers.stp)
# share one assembly frame: in it the reference module sits plugged into the body, all 18 pins
# centred in the socket openings. This transform puts the reference module exactly on our board
# (outline, header holes, header side), so the same transform puts the body where the real
# Flipper is. Derived and checked by tools/fit_check.py.
FLIPPER_FRAME = {"pos": (161.505, 183.25), "rot": (0, 180, -90), "offset": (0, 0, -6.98)}
REFDIR = "/project/hardware/ref/flipper-3d-models/"


def flipper_model(board, ref, stepfile):
    fp = pcbnew.FOOTPRINT(board)
    fp.SetReference(ref)
    fp.Reference().SetVisible(False)
    model = pcbnew.FP_3DMODEL()
    model.m_Filename = REFDIR + stepfile
    model.m_Rotation = pcbnew.VECTOR3D(*FLIPPER_FRAME["rot"])
    model.m_Offset = pcbnew.VECTOR3D(*FLIPPER_FRAME["offset"])
    fp.Models().push_back(model)
    fp.SetPosition(mm(*FLIPPER_FRAME["pos"]))
    return fp


def netlist(path):
    with open(path, encoding="utf-8") as fh:
        nl = parse(fh.read())
    comps = {}
    for c in findall(find(nl, "components"), "comp"):
        ref, fp = find(c, "ref")[1], find(c, "footprint")
        dnp = any(find(pr, "name")[1] == "dnp" for pr in findall(c, "property"))
        props = {find(pr, "name")[1]: (find(pr, "value") or [None, ""])[1] for pr in findall(c, "property")}
        desc = find(c, "description")
        link = {"path": find(find(c, "sheetpath"), "tstamps")[1] + find(c, "tstamps")[1],
                "sheetname": props.get("Sheetname", ""), "sheetfile": props.get("Sheetfile", ""),
                "fields": {"LCSC": props.get("LCSC", ""), "Description": desc[1] if desc else "",
                           "Datasheet": dict((find(f, "name")[1], f[2] if len(f) > 2 else "")
                                             for f in findall(find(c, "fields"), "field")).get("Datasheet", "")}}
        comps[ref] = (find(c, "value")[1], fp[1] if fp else "", dnp, link)
    pads = {}
    for n in findall(find(nl, "nets"), "net"):
        name = find(n, "name")[1]
        for node in findall(n, "node"):
            pads[(find(node, "ref")[1], find(node, "pin")[1])] = name
    return comps, pads


def load(fpid):
    lib, name = fpid.split(":")
    libdir = os.path.join(HW, "ssh_runner.pretty") if lib == "ssh_runner" else "%s/%s.pretty" % (FPDIR, lib)
    return pcbnew.FootprintLoad(libdir, name)


def courtyard(fp):
    cy = fp.GetCourtyard(pcbnew.F_CrtYd if not fp.IsFlipped() else pcbnew.B_CrtYd)
    return cy.BBox() if cy.OutlineCount() else fp.GetBoundingBox(False)


def mm(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))


def move_centre(fp, x, y):
    c = courtyard(fp).GetCenter()
    fp.Move(pcbnew.VECTOR2I(pcbnew.FromMM(x) - c.x, pcbnew.FromMM(y) - c.y))


def rect(board, x0, y0, x1, y1, layer, width=0.1):
    for a, b in (((x0, y0), (x1, y0)), ((x1, y0), (x1, y1)), ((x1, y1), (x0, y1)), ((x0, y1), (x0, y0))):
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(mm(*a))
        s.SetEnd(mm(*b))
        s.SetLayer(layer)
        s.SetWidth(pcbnew.FromMM(width))
        board.Add(s)


def label(board, text, x, y, layer, size=0.8):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(text)
    t.SetLayer(layer)
    t.SetPosition(mm(x, y))
    t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(size), pcbnew.FromMM(size)))
    t.SetTextThickness(pcbnew.FromMM(0.12))
    t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
    board.Add(t)


def place_refs(board):
    """Puts each visible reference designator where it is legible: on the body of a large part,
    otherwise on the first side of the courtyard that is clear of pads, other labels and the edge."""
    edge = board.GetBoardEdgesBoundingBox()
    pads = [p.GetBoundingBox() for fp in board.GetFootprints() for p in fp.Pads()]
    taken = []

    def clear(bb):
        if not (edge.GetLeft() + pcbnew.FromMM(0.3) < bb.GetLeft() and bb.GetRight() < edge.GetRight() - pcbnew.FromMM(0.3)
                and edge.GetTop() + pcbnew.FromMM(0.3) < bb.GetTop() and bb.GetBottom() < edge.GetBottom() - pcbnew.FromMM(0.3)):
            return False
        grown = pcbnew.BOX2I(bb.GetPosition(), bb.GetSize())
        grown.Inflate(pcbnew.FromMM(0.15))
        return not any(grown.Intersects(o) for o in pads + taken)

    labels = []
    for fp in list(board.GetFootprints()):
        ref = fp.Reference()
        if fp.GetReference() in SILK_LABELS:
            ref.SetVisible(False)
            t = pcbnew.PCB_TEXT(board)
            t.SetText(SILK_LABELS[fp.GetReference()])
            t.SetLayer(pcbnew.F_SilkS)
            t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(SILK_REF_SIZE), pcbnew.FromMM(SILK_REF_SIZE)))
            t.SetTextThickness(pcbnew.FromMM(0.15))
            t.SetPosition(fp.GetPosition())
            labels.append(t)
            ref = t
        if not ref.IsVisible():
            continue
        ref.SetTextAngleDegrees(0)
        cy = courtyard(fp)
        c = cy.GetCenter()
        w, h = pcbnew.ToMM(cy.GetWidth()), pcbnew.ToMM(cy.GetHeight())
        tw = pcbnew.ToMM(ref.GetBoundingBox().GetWidth())
        spots = []
        if w > 6 and h > 5:
            spots.append((pcbnew.ToMM(c.x), pcbnew.ToMM(c.y)))          # on the body (ICs, module, jacks)
        cx, cyy = pcbnew.ToMM(c.x), pcbnew.ToMM(c.y)
        spots += [(cx, cyy - h / 2 - 0.7), (cx, cyy + h / 2 + 0.7),
                  (cx - w / 2 - tw / 2 - 0.3, cyy), (cx + w / 2 + tw / 2 + 0.3, cyy)]
        for x, y in spots:
            ref.SetPosition(mm(x, y))
            bb = ref.GetBoundingBox()
            on_body = w > 6 and h > 5 and (x, y) == spots[0] and edge.Contains(bb)
            if clear(bb) or on_body:
                taken.append(bb)
                break
        else:
            if fp.GetReference() in SILK_LABELS:
                # labels may also stand upright beside the part
                ref.SetTextAngleDegrees(90)
                th = pcbnew.ToMM(ref.GetBoundingBox().GetWidth())
                for x, y in ((cx - w / 2 - th / 2 - 0.3, cyy), (cx + w / 2 + th / 2 + 0.3, cyy)):
                    ref.SetPosition(mm(x, y))
                    if clear(ref.GetBoundingBox()):
                        taken.append(ref.GetBoundingBox())
                        break
                else:
                    labels.pop()                                      # label: no spot, none
                    print("label %s: no clear spot" % fp.GetReference())
                continue
            ref.SetVisible(False)                                     # no legible spot: F.Fab only
            print("reference %s: no clear spot, on the fab layer only" % fp.GetReference())
    for t in labels:
        board.Add(t)


def main(netfile):
    comps, pads = netlist(netfile)
    board = pcbnew.LoadBoard(BOARD)
    if len(board.GetFootprints()):
        raise SystemExit("board already has footprints: run gen_board.py --force first")
    top = pcbnew.ToMM(board.GetBoardEdgesBoundingBox().GetTop())
    plan = floorplan(top)
    edge_trimmed(board, plan)
    nets, missing, tps = {}, [], []
    for ref, (val, fpid, dnp, link) in sorted(comps.items()):
        if not fpid:
            continue
        fp = load(fpid)
        fp.SetFPID(pcbnew.LIB_ID(*fpid.split(":")))
        fp.SetReference(ref)
        fp.SetValue(val)
        # link to the schematic symbol, as "Update PCB from Schematic" does
        fp.SetPath(pcbnew.KIID_PATH(link["path"]))
        fp.SetSheetname(link["sheetname"])
        fp.SetSheetfile(link["sheetfile"])
        for name, text in link["fields"].items():
            if text:
                fp.SetField(name, text)
                fp.GetField(name).SetVisible(False)
        if dnp:
            fp.SetDNP(True)
            fp.Models().clear()     # not fitted: keep it out of the renders
        if ref in LCSC_MODELS:       # no library model: LCSC/EasyEDA's (hardware/3d)
            fp.Models().clear()
            name, off, rot = LCSC_MODELS[ref]
            m = pcbnew.FP_3DMODEL()
            m.m_Filename = "${KIPRJMOD}/3d/" + name
            m.m_Offset = pcbnew.VECTOR3D(*off)
            m.m_Rotation = pcbnew.VECTOR3D(*rot)
            fp.Models().push_back(m)
        board.Add(fp)
        for pad in fp.Pads():
            name = pads.get((ref, pad.GetNumber()))
            if name:
                if name not in nets:
                    nets[name] = pcbnew.NETINFO_ITEM(board, name)
                    board.Add(nets[name])
                pad.SetNet(nets[name])
        if ref in plan:
            x, y, rot, side, anchor = plan[ref]
            fp.SetOrientationDegrees(rot)
            fp.SetPosition(mm(x, y))
            if anchor == "c":
                move_centre(fp, x, y)
        elif ref.startswith("TP"):
            tps.append(fp)
        else:
            missing.append(ref)
        ref_text = fp.Reference()
        if ref.startswith(SILK_REF_PREFIXES) and not ref.startswith("TP"):
            ref_text.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(SILK_REF_SIZE), pcbnew.FromMM(SILK_REF_SIZE)))
            ref_text.SetTextThickness(pcbnew.FromMM(0.15))
        else:
            ref_text.SetVisible(False)
    x0, y, pitch = TP_ROW
    for i, fp in enumerate(sorted(tps, key=lambda f: f.GetReference())):
        move_centre(fp, x0 + i * pitch, y)
    for x0, dy0, x1, dy1, text in AREAS:
        rect(board, x0, top + dy0, x1, top + dy1, pcbnew.Dwgs_User)
        label(board, text, x0 + 0.3, top + dy0 + 0.6, pcbnew.Dwgs_User, 0.7)
    if missing:
        raise SystemExit("no place for: " + " ".join(missing))
    add_fiducials(board)
    place_refs(board)
    pcbnew.SaveBoard(BOARD, board)
    # fit-check copy with the Flipper body (alignment: DESIGN.md 5.2, check: tools/fit_check.py)
    board.Add(flipper_model(board, "FLIPPER", "flipper-zero-body-3d.stp"))
    os.makedirs(os.path.dirname(FIT), exist_ok=True)
    pcbnew.SaveBoard(FIT, board)
    with open(FIT, encoding="utf-8") as fh:       # the copy lives in .cache: point models back
        text = fh.read().replace("${KIPRJMOD}", "/project/hardware")
    with open(FIT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("board height %.1f mm: %d footprints placed" % (150.0 - top, len(board.GetFootprints()) - 1))


if __name__ == "__main__":
    main(sys.argv[1])
