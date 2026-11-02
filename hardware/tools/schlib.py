"""Small helpers to write KiCad 10 schematics from a parts-and-nets description.

Used by gen_sch.py. Each part carries its pin-to-net map; the drawing on top of it is free: a pin
is either wired explicitly (Sheet.link, Sheet.tap, Sheet.wire) or gets a short stub wire ending in
a net label, a hierarchical label (nets on more than one sheet) or a power symbol (GND, +3V3, VBUS).
A sheet's hierarchical labels become pins on its sheet symbol in the root sheet, wired there.
Before a sheet is written, its drawing is checked against the nets: every connected group of pins,
wires and labels must carry exactly one net name, the one the parts say, so the netlist follows
from the description and not from the drawing. kicad-cli ERC and the exported netlist then prove
it (gen_sch.py check).

Symbol, sheet and file UUIDs are kept from the schematic already on disk (Design.keep_uuids), so a
redraw never changes the links the board's footprints hold to their symbols.

Runs inside the kicad/kicad container (reads the stock symbol libraries there).
"""
import copy
import math
import os
import uuid as uuidlib

SYMDIR = "/usr/share/kicad/symbols"
PROJECT = "ssh-runner"
G = 1.27  # connection grid


class Q(str):
    """A quoted s-expression string."""


def parse(text):
    pos, n = 0, len(text)
    stack, cur = [], []
    while pos < n:
        c = text[pos]
        if c in " \t\r\n":
            pos += 1
        elif c == "(":
            stack.append(cur)
            cur = []
            pos += 1
        elif c == ")":
            done = cur
            cur = stack.pop()
            cur.append(done)
            pos += 1
        elif c == '"':
            pos += 1
            out = []
            while text[pos] != '"':
                if text[pos] == "\\":
                    pos += 1
                    out.append({"n": "\n", "t": "\t"}.get(text[pos], text[pos]))
                else:
                    out.append(text[pos])
                pos += 1
            pos += 1
            cur.append(Q("".join(out)))
        else:
            start = pos
            while pos < n and text[pos] not in ' \t\r\n()"':
                pos += 1
            cur.append(text[start:pos])
    return cur[0]


def dump(node, indent=0):
    if isinstance(node, Q):
        return '"' + node.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'
    if isinstance(node, str):
        return node
    if isinstance(node, (int, float)):
        return fmt(node)
    simple = all(not isinstance(x, list) for x in node)
    if simple:
        return "(" + " ".join(dump(x) for x in node) + ")"
    pad = "\t" * (indent + 1)
    head = [dump(x) for x in node if not isinstance(x, list)]
    parts = ["(" + " ".join(head)]
    for x in node:
        if isinstance(x, list):
            parts.append("\n" + pad + dump(x, indent + 1))
    return "".join(parts) + "\n" + "\t" * indent + ")"


def fmt(v):
    v = round(float(v), 4)
    if v == int(v):
        return str(int(v))
    return ("%.4f" % v).rstrip("0").rstrip(".")


def find(node, key):
    for x in node:
        if isinstance(x, list) and x and x[0] == key:
            return x
    return None


def findall(node, key):
    return [x for x in node if isinstance(x, list) and x and x[0] == key]


def uid():
    return str(uuidlib.uuid4())


_libs = {}


def lib_symbol(lib_id):
    """Flattened copy of a stock symbol, named lib_id, as embedded in lib_symbols."""
    lib, name = lib_id.split(":")
    if lib not in _libs:
        with open("%s/%s.kicad_sym" % (SYMDIR, lib), encoding="utf-8") as f:
            _libs[lib] = {s[1]: s for s in findall(parse(f.read()), "symbol")}
    syms = _libs[lib]
    child = syms[name]
    ext = find(child, "extends")
    if ext is None:
        out = copy.deepcopy(child)
        out[1] = Q(lib_id)
        return out
    parent = syms[ext[1]]
    out = copy.deepcopy(parent)
    out[1] = Q(lib_id)
    props = {p[1]: p for p in findall(child, "property")}
    for i, x in enumerate(out):
        if isinstance(x, list) and x and x[0] == "property" and x[1] in props:
            out[i] = copy.deepcopy(props.pop(x[1]))
    for p in props.values():
        out.insert(2, copy.deepcopy(p))
    for x in out:
        if isinstance(x, list) and x and x[0] == "symbol":
            x[1] = Q(name + x[1][len(ext[1]):])
    return out


def pins_of(sym):
    """{number: (x, y, angle, type, name)} in library coordinates (y up), unit 1."""
    pins = {}
    for sub in findall(sym, "symbol"):
        unit, style = sub[1].rsplit("_", 2)[-2:]
        if unit not in ("0", "1") or style not in ("0", "1"):
            continue
        for p in findall(sub, "pin"):
            at = find(p, "at")
            num = find(p, "number")[1]
            nm = find(p, "name")[1]
            pins.setdefault(num, (float(at[1]), float(at[2]), float(at[3]), p[1], nm))
    return pins


def prop(sym, key):
    p = [x for x in findall(sym, "property") if x[1] == key]
    return p[0][2] if p else ""


def place(x, y, rot, px, py):
    """Library pin (px, py) -> schematic coordinates for a symbol at (x, y) rotated rot."""
    r = math.radians(rot)
    rx = px * math.cos(r) - py * math.sin(r)
    ry = px * math.sin(r) + py * math.cos(r)
    return round(x + rx, 4), round(y - ry, 4)


def font(size=1.27, justify=None, hide=False, bold=False):
    f = ["font", ["size", fmt(size), fmt(size)]]
    if bold:
        f.append(["bold", "yes"])
    e = ["effects", f]
    if justify:
        e.append(["justify"] + justify.split())
    if hide:
        e.append(["hide", "yes"])
    return e


def label_justify(angle):
    return {0: "left", 90: "left", 180: "right", 270: "right"}[int(angle) % 360]


def step(x, y, angle, d):
    """The point d from (x, y) in direction angle (0 right, 90 up, schematic y down)."""
    a = int(angle) % 360
    dx, dy = {0: (d, 0), 90: (0, -d), 180: (-d, 0), 270: (0, d)}[a]
    return round(x + dx, 4), round(y + dy, 4)


# net -> (power symbol, the direction its pin points out of the symbol body)
POWER = {"GND": ("power:GND", 270), "+3V3": ("power:+3V3", 90), "VBUS": ("power:VBUS", 90)}


class Part:
    def __init__(self, sheet, ref, lib_id, at, rot, nets, value, fp, fields, dnp, in_bom):
        self.sheet, self.ref, self.lib_id, self.at, self.rot = sheet, ref, lib_id, at, rot
        self.sym = lib_symbol(lib_id)
        self.pins = pins_of(self.sym)
        self.nets, self.dnp, self.in_bom = nets, dnp, in_bom
        self.value = value or prop(self.sym, "Value")
        self.fp = fp if fp is not None else prop(self.sym, "Footprint")
        self.fields = fields
        self.wired = set()                    # pins joined by explicit wires: no stub of their own
        self.stub = {}                        # pin -> stub length (default 2.54)
        self.mirror = None                    # "y": flipped left to right (pins of a connector on its right)
        self.side_power = False               # power symbols point along the stub (connector rows)
        self.text_at = {}                     # "Reference"/"Value" -> (dx, dy, justify): placed by hand
        missing = [n for n, p in self.pins.items() if n not in nets and p[3] != "no_connect"]
        unknown = [n for n in nets if n not in self.pins]
        if missing or unknown:
            raise SystemExit("%s: pins without a net %s, unknown pins %s" % (ref, missing, unknown))

    def pin_end(self, num, moved=False):
        x, y, a, _, _ = self.pins[num]
        if self.mirror == "y":
            x, a = -x, (180 - a) % 360
        px, py = place(self.at[0], self.at[1], self.rot, x, y)
        if moved:
            px, py = round(px + self.off[0], 4), round(py + self.off[1], 4)
        return px, py, (a + 180 + self.rot) % 360

    def pt(self, num):
        """Pin num's end, in the coordinates of the block the part belongs to."""
        if self.off != self.sheet.off:
            raise SystemExit("%s is drawn from outside its block" % self.ref)
        return self.pin_end(num)[:2]


class Sheet:
    def __init__(self, file, name, paper="A3", title=""):
        self.file, self.name, self.paper, self.title = file, name, paper, title or name
        self.parts, self.texts, self.wires, self.labels, self.frames = [], [], [], [], []
        self.ports = []          # (net, pt, angle): extra hierarchical labels (power nets that leave the sheet)
        self.shapes = {}         # net -> hierarchical label shape seen from this sheet (input, output, ...)
        self.root_pins = []      # (net, side "L"/"R"/"T"/"B", offset along the edge from top / left)
        self.off = (0, 0)        # offset of the block being drawn (Sheet.move)
        self.uuid = uid()        # the sheet symbol in the root sheet
        self.file_uuid = uid()

    def part(self, ref, lib_id, at, nets, rot=0, value=None, fp=None, dnp=False, in_bom=True, mirror=None,
             side_power=False, stub=None, **fields):
        p = Part(self, ref, lib_id, at, rot, nets, value, fp, fields, dnp, in_bom)
        p.mirror, p.side_power, p.off = mirror, side_power, self.off
        if stub:
            p.stub = {n: stub for n in p.pins}
        self.parts.append(p)
        return p

    def move(self, dx, dy):
        """Draw what follows (one block) in its own coordinates, moved by (dx, dy) on the page."""
        self.off = (dx, dy)

    def _m(self, pt):
        return round(pt[0] + self.off[0], 4), round(pt[1] + self.off[1], 4)

    def text(self, at, s, size=1.27, bold=False):
        self.texts.append((self._m(at), s, size, bold))

    def frame(self, x0, y0, x1, y1, title):
        """A titled block: a dashed rectangle with its name in its top-left corner."""
        (x0, y0), (x1, y1) = self._m((x0, y0)), self._m((x1, y1))
        self.frames.append((x0, y0, x1, y1, title))

    def wire(self, *pts):
        for a, b in zip(pts, pts[1:]):
            if a != b:
                if a[0] != b[0] and a[1] != b[1]:
                    raise SystemExit("%s: diagonal wire %s-%s" % (self.name, a, b))
                self.wires.append((self._m(a), self._m(b)))

    def label(self, net, pt, angle):
        """A label (or power symbol) for net at pt, its text toward angle."""
        self.labels.append((net, self._m(pt), angle))

    def port(self, net, pt, angle):
        """A hierarchical label for a power net, so it shows as a pin on the sheet symbol."""
        self.ports.append((net, self._m(pt), angle))

    def route(self, a, b, first="h"):
        """Orthogonal wire from a to b: horizontal first ("h") or vertical first ("v")."""
        if a[0] == b[0] or a[1] == b[1]:
            self.wire(a, b)
        elif first == "h":
            self.wire(a, (b[0], a[1]), b)
        else:
            self.wire(a, (a[0], b[1]), b)

    def link(self, pa, na, pb, nb, first="h"):
        """Wire pin na of part pa to pin nb of part pb (both must be on the same net)."""
        if pa.nets[na] != pb.nets[nb]:
            raise SystemExit("%s.%s and %s.%s are on different nets" % (pa.ref, na, pb.ref, nb))
        pa.wired.add(na)
        pb.wired.add(nb)
        self.route(pa.pt(na), pb.pt(nb), first)

    def tap(self, p, num, pt, first="h"):
        """Wire pin num of part p to a point (on a rail or a label)."""
        p.wired.add(num)
        self.route(p.pt(num), pt, first)


class Design:
    def __init__(self, title, rev, date, note=""):
        self.title, self.rev, self.date, self.note = title, rev, date, note
        self.sheets = []
        self.root_uuid = uid()
        self.root_texts = []
        self.root_wires = []    # polylines of real wires between sheet pins
        self.root_labels = []   # (net, pt, angle): global labels on root wires (they keep the bare net names)
        self.root_junctions = []
        self.pwr = 0
        self.keep = {}          # ref -> symbol uuid from the schematic on disk

    def sheet(self, *a, **k):
        s = Sheet(*a, **k)
        self.sheets.append(s)
        return s

    def keep_uuids(self, outdir):
        """Reuse the root, sheet and symbol UUIDs of the schematic already in outdir."""
        root = os.path.join(outdir, PROJECT + ".kicad_sch")
        if not os.path.exists(root):
            return
        with open(root, encoding="utf-8") as f:
            r = parse(f.read())
        self.root_uuid = find(r, "uuid")[1]
        by_file = {}
        for sh in findall(r, "sheet"):
            props = {p[1]: p[2] for p in findall(sh, "property")}
            by_file[props["Sheetfile"]] = find(sh, "uuid")[1]
        for s in self.sheets:
            if s.file in by_file:
                s.uuid = by_file[s.file]
            path = os.path.join(outdir, s.file)
            if not os.path.exists(path):
                continue
            with open(path, encoding="utf-8") as f:
                doc = parse(f.read())
            s.file_uuid = find(doc, "uuid")[1]
            for sym in findall(doc, "symbol"):
                props = {p[1]: p[2] for p in findall(sym, "property")}
                ref = props.get("Reference", "")
                if ref and not ref.startswith("#"):
                    self.keep[ref] = find(sym, "uuid")[1]

    def net_sheets(self):
        out = {}
        for s in self.sheets:
            for p in s.parts:
                for n in p.nets.values():
                    if n != "NC":
                        out.setdefault(n, set()).add(s.name)
        return out

    def expected(self):
        """[(ref, pin, net)] for every connected pin (NC excluded)."""
        return [(p.ref, num, n) for s in self.sheets for p in s.parts for num, n in p.nets.items() if n != "NC"]

    def title_block(self, sheet_title):
        return ["title_block", ["title", Q(sheet_title)], ["date", Q(self.date)], ["rev", Q(self.rev)],
                ["company", Q("flipper-ssh-runner")],
                ["comment", "1", Q(self.title)],
                ["comment", "2", Q("License: CERN-OHL-S-2.0 (logo: all rights reserved, hardware/art/LICENSE.md)")],
                ["comment", "3", Q("Design notes: hardware/DESIGN.md")]]

    def write(self, outdir):
        nsheets = self.net_sheets()
        for s in self.sheets:
            self.write_sheet(s, outdir, nsheets)
        self.write_root(outdir)

    def symbol_instance(self, lib_id, sym, at, rot, ref, value, fp, path, fields=(), dnp=False,
                        in_bom=True, hide_ref=False, pins=None, keep=None, mirror=None, text_at=None):
        x, y = at
        node = ["symbol", ["lib_id", Q(lib_id)], ["at", fmt(x), fmt(y), fmt(rot)]]
        if mirror:
            node.append(["mirror", mirror])
        node += [["unit", "1"],
                ["exclude_from_sim", "no"], ["in_bom", "yes" if in_bom else "no"], ["on_board", "yes"],
                ["dnp", "yes" if dnp else "no"], ["uuid", Q(keep or uid())]]
        lib_props = {p[1]: p for p in findall(sym, "property")}
        for key, val in [("Reference", ref), ("Value", value), ("Footprint", fp),
                         ("Datasheet", prop(sym, "Datasheet")), ("Description", prop(sym, "Description"))]:
            lp = lib_props.get(key)
            if lp is not None and find(lp, "at") is not None:
                la = find(lp, "at")
                fx, fy = place(x, y, rot, -float(la[1]) if mirror == "y" else float(la[1]), float(la[2]))
                ang = (float(la[3]) if len(la) > 3 else 0) + rot
                eff = copy.deepcopy(find(lp, "effects")) or font()
                hidden = find(lp, "hide")  # KiCad 10 libraries keep (hide yes) outside (effects)
                if hidden is not None and hidden[1] == "yes" and not find(eff, "hide"):
                    eff.append(["hide", "yes"])
            else:
                fx, fy, ang, eff = x, y, 0, font(hide=True)
            if text_at and key in text_at:
                dx, dy, just = text_at[key]
                fx, fy, ang = x + dx, y + dy, -rot      # field angles are stored relative to the symbol
                if just and rot % 360 in (90, 180):           # KiCad mirrors the justification there
                    just = {"left": "right", "right": "left"}[just]
                eff = font(justify=just) if just else font()
            if key == "Reference" and hide_ref and not find(eff, "hide"):
                eff.append(["hide", "yes"])
            node.append(["property", Q(key), Q(val), ["at", fmt(fx), fmt(fy), fmt(ang % 180)], eff])
        for key, val in fields:
            node.append(["property", Q(key), Q(val), ["at", fmt(x), fmt(y), "0"], font(hide=True)])
        for num in sorted(pins if pins is not None else pins_of(sym)):
            node.append(["pin", Q(num), ["uuid", Q(uid())]])
        node.append(["instances", ["project", Q(PROJECT), ["path", Q(path), ["reference", Q(ref)], ["unit", "1"]]]])
        return node

    def label_items(self, net, pt, angle, nsheets, libs, path, shape="passive", port=False):
        x, y = pt
        if port or (nsheets is not None and net not in POWER and len(nsheets[net]) > 1):
            return [["hierarchical_label", Q(net), ["shape", shape], ["at", fmt(x), fmt(y), fmt(angle)],
                     ["fields_autoplaced", "yes"], font(justify=label_justify(angle)), ["uuid", Q(uid())]]]
        if net in POWER:
            lib_id, natural = POWER[net]
            sym = lib_symbol(lib_id)
            libs.setdefault(lib_id, sym)
            self.pwr += 1
            # a sideways symbol (connector rows) reads its name level, beyond the symbol
            side = {0: {"Value": (3.81, 0, "left")}, 180: {"Value": (-3.81, 0, "right")}}.get(angle)
            return [self.symbol_instance(lib_id, sym, (x, y), (angle - natural) % 360,
                                         "#PWR%03d" % self.pwr, net, "", path, hide_ref=True, text_at=side)]
        if nsheets is None:                        # root sheet: keeps the bare name of a sheet-crossing net
            return [["global_label", Q(net), ["shape", "passive"], ["at", fmt(x), fmt(y), fmt(angle)],
                     ["fields_autoplaced", "yes"], font(justify=label_justify(angle)), ["uuid", Q(uid())],
                     ["property", Q("Intersheetrefs"), Q("${INTERSHEET_REFS}"), ["at", fmt(x), fmt(y), "0"],
                      font(hide=True)]]]
        return [["label", Q(net), ["at", fmt(x), fmt(y), fmt(angle)], ["fields_autoplaced", "yes"],
                 font(justify=label_justify(angle) + " bottom"), ["uuid", Q(uid())]]]

    def check_drawing(self, s, pins, labels, wires):
        """Every connected group carries exactly one net, the one the parts give its pins."""
        parent = {}

        def root(a):
            parent.setdefault(a, a)
            while parent[a] != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a

        for a, b in wires:
            parent[root(a)] = root(b)
        nets = {}
        for pt, net, who in pins + labels:
            nets.setdefault(root(pt), set()).add((net, who))
        for a, b in wires:
            nets.setdefault(root(a), set())
        for grp, members in nets.items():
            names = {n for n, _ in members}
            if len(names) > 1:
                raise SystemExit("%s: one wire group joins %s" % (s.name, sorted(members)))
            if not any(w == "label" for _, w in members):
                raise SystemExit("%s: a wire group has no label: %s" % (s.name, sorted(members)))

    def write_sheet(self, s, outdir, nsheets):
        path = "/%s/%s" % (self.root_uuid, s.uuid)
        libs, items, wires, labels, pins, ncs = {}, [], list(s.wires), list(s.labels) + list(s.ports), [], []
        for p in s.parts:
            libs.setdefault(p.lib_id, p.sym)
            fields = [(k, v) for k, v in p.fields.items() if v]
            items.append(self.symbol_instance(p.lib_id, p.sym, (p.at[0] + p.off[0], p.at[1] + p.off[1]), p.rot, p.ref, p.value, p.fp, path,
                                              fields, p.dnp, p.in_bom, pins=p.pins, keep=self.keep.get(p.ref),
                                              mirror=p.mirror, text_at=p.text_at))
            done = {}
            for num, net in p.nets.items():
                x, y, out = p.pin_end(num, moved=True)
                if (x, y) in done:
                    if done[(x, y)] != net:
                        raise SystemExit("%s pin %s: stacked pins on different nets" % (p.ref, num))
                    continue
                done[(x, y)] = net
                if net == "NC":
                    ncs.append((x, y))
                    continue
                pins.append(((x, y), net, p.ref))
                if num in p.wired:
                    continue
                end = step(x, y, out, p.stub.get(num, 2.54))
                wires.append(((x, y), end))
                angle = out
                if net in POWER and not p.side_power and out in (0, 180):
                    angle = 270 if net == "GND" else 90     # power symbols stand upright off a sideways stub
                labels.append((net, end, angle))
        # split wires at every pin, label and wire end lying inside them; junction dots where 3+ meet
        pts = {a for w in wires for a in w} | {pt for pt, _, _ in pins} | {pt for _, pt, _ in labels}
        split = []
        for a, b in wires:
            inner = sorted((q for q in pts if q not in (a, b) and min(a[0], b[0]) <= q[0] <= max(a[0], b[0])
                            and min(a[1], b[1]) <= q[1] <= max(a[1], b[1])
                            and (a[0] == b[0] == q[0] or a[1] == b[1] == q[1])),
                           key=lambda q: abs(q[0] - a[0]) + abs(q[1] - a[1]))
            seq = [a] + inner + [b]
            split += [(u, v) for u, v in zip(seq, seq[1:]) if u != v]
        wires = split
        self.check_drawing(s, pins, [(pt, n, "label") for n, pt, _ in labels], wires)
        degree = {}
        for a, b in wires:
            degree[a] = degree.get(a, 0) + 1
            degree[b] = degree.get(b, 0) + 1
        for pt, _, _ in pins:
            if pt in degree:
                degree[pt] += 1
        for (x, y), n in sorted(degree.items()):
            if n >= 3:
                items.append(["junction", ["at", fmt(x), fmt(y)], ["diameter", "0"], ["color", "0", "0", "0", "0"],
                              ["uuid", Q(uid())]])
        for a, b in wires:
            items.append(["wire", ["pts", ["xy", fmt(a[0]), fmt(a[1])], ["xy", fmt(b[0]), fmt(b[1])]],
                          ["stroke", ["width", "0"], ["type", "default"]], ["uuid", Q(uid())]])
        ports = set(s.ports)
        for net, pt, angle in labels:
            items += self.label_items(net, pt, angle, nsheets, libs, path, s.shapes.get(net, "passive"),
                                      (net, pt, angle) in ports)
        for x, y in ncs:
            items.append(["no_connect", ["at", fmt(x), fmt(y)], ["uuid", Q(uid())]])
        for x0, y0, x1, y1, title in s.frames:
            items.append(["rectangle", ["start", fmt(x0), fmt(y0)], ["end", fmt(x1), fmt(y1)],
                          ["stroke", ["width", "0.254"], ["type", "dash"], ["color", "72", "72", "72", "1"]],
                          ["fill", ["type", "none"]], ["uuid", Q(uid())]])
            items.append(["text", Q(title), ["exclude_from_sim", "no"], ["at", fmt(x0 + 1.27), fmt(y0 + 1.27), "0"],
                          font(2.0, "left top", bold=True), ["uuid", Q(uid())]])
        for (x, y), txt, size, bold in s.texts:
            items.append(["text", Q(txt), ["exclude_from_sim", "no"], ["at", fmt(x), fmt(y), "0"],
                          font(size, "left top", bold=bold), ["uuid", Q(uid())]])
        doc = ["kicad_sch", ["version", "20250114"], ["generator", Q("eeschema")], ["generator_version", Q("9.0")],
               ["uuid", Q(s.file_uuid)], ["paper", Q(s.paper)], self.title_block(s.title),
               ["lib_symbols"] + list(libs.values())] + items + [["embedded_fonts", "no"]]
        with open("%s/%s" % (outdir, s.file), "w", encoding="utf-8", newline="\n") as f:
            f.write(dump(doc) + "\n")

    def write_root(self, outdir):
        """The root sheet: the four sheet symbols, their pins wired, with notes."""
        items = []
        for i, s in enumerate(self.sheets):
            x, y, w, h = s.root_box
            node = ["sheet", ["at", fmt(x), fmt(y)], ["size", fmt(w), fmt(h)], ["exclude_from_sim", "no"],
                    ["in_bom", "yes"], ["on_board", "yes"], ["dnp", "no"], ["fields_autoplaced", "yes"],
                    ["stroke", ["width", "0.1524"], ["type", "solid"]],
                    ["fill", ["color", "0", "0", "0", "0.0000"]], ["uuid", Q(s.uuid)],
                    ["property", Q("Sheetname"), Q(s.name), ["at", fmt(x), fmt(y - 0.7116), "0"],
                     font(1.524, "left bottom", bold=True)],
                    ["property", Q("Sheetfile"), Q(s.file), ["at", fmt(x), fmt(y + h + 0.5684), "0"],
                     font(justify="left top")]]
            for net, side, off in s.root_pins:
                px, py, ang, just = {"L": (x, y + off, 180, "left"), "R": (x + w, y + off, 0, "right"),
                                     "T": (x + off, y, 90, "right"), "B": (x + off, y + h, 270, "left")}[side]
                node.append(["pin", Q(net), s.shapes.get(net, "passive"), ["at", fmt(px), fmt(py), str(ang)],
                             ["uuid", Q(uid())], font(justify=just)])
            node.append(["instances", ["project", Q(PROJECT),
                                       ["path", Q("/" + self.root_uuid), ["page", Q(str(i + 2))]]]])
            items.append(node)
        for pts in self.root_wires:
            for a, b in zip(pts, pts[1:]):
                items.append(["wire", ["pts", ["xy", fmt(a[0]), fmt(a[1])], ["xy", fmt(b[0]), fmt(b[1])]],
                              ["stroke", ["width", "0"], ["type", "default"]], ["uuid", Q(uid())]])
        for x, y in self.root_junctions:
            items.append(["junction", ["at", fmt(x), fmt(y)], ["diameter", "0"], ["color", "0", "0", "0", "0"],
                          ["uuid", Q(uid())]])
        for net, pt, angle in self.root_labels:
            items += self.label_items(net, pt, angle, None, {}, "/" + self.root_uuid)
        for (x, y), txt, size, bold in self.root_texts:
            items.append(["text", Q(txt), ["exclude_from_sim", "no"], ["at", fmt(x), fmt(y), "0"],
                          font(size, "left top", bold=bold), ["uuid", Q(uid())]])
        doc = ["kicad_sch", ["version", "20250114"], ["generator", Q("eeschema")], ["generator_version", Q("9.0")],
               ["uuid", Q(self.root_uuid)], ["paper", Q("A3")], self.title_block(self.title), ["lib_symbols"]] + items + [
            ["sheet_instances", ["path", Q("/"), ["page", Q("1")]]], ["embedded_fonts", "no"]]
        with open("%s/%s.kicad_sch" % (outdir, PROJECT), "w", encoding="utf-8", newline="\n") as f:
            f.write(dump(doc) + "\n")
