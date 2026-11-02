"""JLCPCB fabrication and assembly files from a KiCad board: Gerbers, drill files, BOM and CPL.

    venv\\Scripts\\python hardware\\tools\\cam.py --board path\\to\\board.kicad_pcb --sch path\\to\\board.kicad_sch
    venv/bin/python hardware/tools/cam.py --board ... --sch ...

kicad-cli runs in the pinned kicad/kicad image (docker run --rm, the board's folder mounted
read-only). Output, all in .cache/hardware/cam/ (replaced on every run):

    gerbers/              Gerbers (Protel extensions, no X2, no netlist attributes) and the
                          Excellon drill files (mm, PTH and NPTH apart), as JLC's KiCad guide
    <board>-gerbers.zip   the folder above, for the PCB order
    <board>-bom-jlc.csv   Comment, Designator, Footprint, LCSC Part #  (one row per part kind)
    <board>-cpl-jlc.csv   Designator, Mid X, Mid Y, Layer, Rotation   (one row per placement)
    raw/                  kicad-cli's position file and ungrouped BOM the two CSVs come from

The checks fail the run (exit 1, nothing half-written is left as if it were good): every
fitted part has an LCSC number, DNP parts are in neither CSV, no designator repeats, and the
BOM and the CPL name the same parts.

Placement follows the mainstream convention of Bouni/kicad-jlcpcb-tools: a part's position is the
centre of its soldered pads' bounding box (JLC places its model by that centre, not by KiCad's
footprint origin), read from the board with pcbnew in the same image. Rotations and optional
offsets come from jlc_rotations.csv next to this file, seeded from that plugin's default
correction table (matthewlai/JLCKicadTools cpl_rotations_db.csv) plus this project's own rows.
Parts whose correction is not yet verified in JLC's preview are listed.

SPDX-License-Identifier: GPL-3.0-only
"""

from __future__ import annotations

import argparse
import csv
import io
import math
import re
import shutil
import subprocess
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / ".cache" / "hardware" / "cam"
ROTATIONS = Path(__file__).with_name("jlc_rotations.csv")
IMAGE = "kicad/kicad:10.0.7-full"

# A 4-layer board's fabrication layers
LAYERS = ["F.Cu", "In1.Cu", "In2.Cu", "B.Cu", "F.Mask", "B.Mask", "F.Paste", "B.Paste", "F.SilkS", "B.SilkS",
          "Edge.Cuts"]
# Fields read from the schematic, one row per symbol (no grouping)
BOM_FIELDS = ["Reference", "Value", "Footprint", "LCSC", "${DNP}", "${EXCLUDE_FROM_BOM}", "${EXCLUDE_FROM_BOARD}"]
BOM_LABELS = ["Reference", "Value", "Footprint", "LCSC", "DNP", "ExcludeFromBOM", "ExcludeFromBoard"]


class CamError(Exception):
    """A check failed: the files must not be sent to the fab."""


@dataclass(frozen=True)
class Rotation:
    match: str
    offset: float
    verified: bool
    dx: float = 0.0  # mm, footprint frame (KiCad axes, y down, before rotation): KiCad origin -> JLC's centre
    dy: float = 0.0


@dataclass(frozen=True)
class Placement:
    ref: str
    footprint: str
    x: float
    y: float
    rot: float
    bottom: bool


# ---------------------------------------------------------------- pure part (host-tested)


def read_rotations(text: str) -> list[Rotation]:
    rows = [line for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")]
    out = []
    for r in csv.DictReader(rows):
        out.append(Rotation(r["match"].strip(), float(r["offset"]), r["verified"].strip().lower() == "yes",
                            float((r.get("dx") or "0").strip() or 0), float((r.get("dy") or "0").strip() or 0)))
    return out


def rotation_for(ref: str, footprint: str, rotations: list[Rotation]) -> Rotation | None:
    for r in rotations:
        if r.match == f"ref:{ref}":
            return r
    for r in rotations:
        if not r.match.startswith("ref:") and re.search(r.match, footprint):
            return r
    return None


def jlc_rotation(p: Placement, rot: Rotation | None) -> float:
    """KiCad's rotation as JLC places it. A bottom part is seen from below, mirrored left to
    right, so its angle becomes 180 - angle and its correction turns the other way."""
    off = rot.offset if rot else 0.0
    if p.bottom:
        return (180.0 - p.rot - off) % 360.0
    return (p.rot + off) % 360.0


def jlc_position(p: Placement, rot: Rotation | None) -> tuple[float, float]:
    """KiCad's position moved by the correction's offset, turned with the part. The position file is
    y up and its angle counter-clockwise; the offset is given y down, so its y flips first."""
    if rot is None or (rot.dx == 0 and rot.dy == 0):
        return p.x, p.y
    if p.bottom:
        raise CamError(f"{p.ref}: position offsets are not supported on the bottom side")
    a = math.radians(p.rot)
    lx, ly = rot.dx, -rot.dy
    return (p.x + lx * math.cos(a) - ly * math.sin(a), p.y + lx * math.sin(a) + ly * math.cos(a))


def parse_pos(text: str) -> list[Placement]:
    """kicad-cli pcb export pos --format csv --units mm: Ref,Val,Package,PosX,PosY,Rot,Side."""
    out = []
    for r in csv.DictReader(io.StringIO(text)):
        side = r["Side"].strip().lower()
        if side not in ("top", "bottom"):
            raise CamError(f"{r['Ref']}: unknown side {r['Side']!r} in the position file")
        out.append(Placement(r["Ref"].strip(), r["Package"].strip(), float(r["PosX"]), float(r["PosY"]),
                             float(r["Rot"]), side == "bottom"))
    return out


def parse_bom(text: str) -> list[dict[str, str]]:
    """kicad-cli sch export bom with BOM_FIELDS / BOM_LABELS, ungrouped."""
    return [{k: (v or "").strip() for k, v in r.items()} for r in csv.DictReader(io.StringIO(text))]


def _flag(v: str) -> bool:
    return v.strip().lower() in ("dnp", "excluded", "yes", "true", "1", "x")


def _footprint_name(fp: str) -> str:
    return fp.split(":", 1)[-1]


def _ref_key(ref: str) -> tuple[str, int, str]:
    m = re.match(r"([A-Za-z_]+)(\d*)(.*)", ref)
    return (m.group(1), int(m.group(2) or 0), m.group(3)) if m else (ref, 0, "")


@dataclass
class Result:
    bom: list[list[str]]  # JLC BOM rows, header first
    cpl: list[list[str]]  # JLC CPL rows, header first
    unverified: dict[str, list[str]]  # footprint -> designators whose rotation is not verified yet
    dnp: list[str]


# pcbnew, run in the image: every footprint's soldered-pad bounding-box centre, in the position
# file's frame (mm, y up), as kicad-jlcpcb-tools' Fabrication.get_position does
PAD_CENTRES = """
import sys, pcbnew
b = pcbnew.LoadBoard(sys.argv[1])
print("Ref,X,Y")
for f in b.GetFootprints():
    pads = [p for p in f.Pads() if p.GetAttribute() != pcbnew.PAD_ATTRIB_NPTH and p.IsOnCopperLayer()]
    if not pads:
        continue
    bb = pads[0].GetBoundingBox()
    for p in pads[1:]:
        bb.Merge(p.GetBoundingBox())
    c = bb.GetCenter()
    print("%s,%.6f,%.6f" % (f.GetReference(), pcbnew.ToMM(c.x), -pcbnew.ToMM(c.y)))
"""


def parse_centres(text: str) -> dict[str, tuple[float, float]]:
    return {r["Ref"]: (float(r["X"]), float(r["Y"])) for r in csv.DictReader(io.StringIO(text))}


def build(bom_text: str, pos_text: str, rotations: list[Rotation],
          centres: dict[str, tuple[float, float]] | None = None) -> Result:
    """The JLC BOM and CPL, or CamError with every failed check."""
    errors: list[str] = []
    symbols = parse_bom(bom_text)
    places = parse_pos(pos_text)
    if centres:                                       # pad centres replace KiCad's origins
        places = [Placement(p.ref, p.footprint, *centres.get(p.ref, (p.x, p.y)), p.rot, p.bottom) for p in places]

    seen: dict[str, int] = {}
    for s in symbols:
        seen[s["Reference"]] = seen.get(s["Reference"], 0) + 1
    errors += [f"designator {r} appears {n} times in the schematic" for r, n in sorted(seen.items()) if n > 1]
    pseen: dict[str, int] = {}
    for p in places:
        pseen[p.ref] = pseen.get(p.ref, 0) + 1
    errors += [f"designator {r} appears {n} times on the board" for r, n in sorted(pseen.items()) if n > 1]

    dnp = sorted((s["Reference"] for s in symbols if _flag(s.get("DNP", ""))), key=_ref_key)
    fitted = [s for s in symbols if not _flag(s.get("DNP", "")) and not _flag(s.get("ExcludeFromBOM", ""))
              and not _flag(s.get("ExcludeFromBoard", ""))]
    for s in fitted:
        if not re.fullmatch(r"C\d+", s.get("LCSC", "")):
            errors.append(f"{s['Reference']} ({s['Value']}) has no LCSC part number"
                          + (f" (got {s['LCSC']!r})" if s.get("LCSC") else ""))

    fitted_refs = {s["Reference"] for s in fitted}
    placed = [p for p in places if p.ref not in dnp]
    placed_refs = {p.ref for p in placed}
    for r in sorted(fitted_refs - placed_refs, key=_ref_key):
        errors.append(f"{r} is in the BOM but not placed (not on the board, or DNP on the board only)")
    for r in sorted(placed_refs - fitted_refs, key=_ref_key):
        errors.append(f"{r} is placed but not in the BOM (not in the schematic, or excluded there)")
    if errors:
        raise CamError("\n".join(errors))

    groups: dict[tuple[str, str, str], list[str]] = {}
    for s in fitted:
        groups.setdefault((s["Value"], _footprint_name(s["Footprint"]), s["LCSC"]), []).append(s["Reference"])
    bom = [["Comment", "Designator", "Footprint", "LCSC Part #"]]
    for (value, fp, lcsc), refs in sorted(groups.items(), key=lambda kv: _ref_key(sorted(kv[1], key=_ref_key)[0])):
        bom.append([value, ",".join(sorted(refs, key=_ref_key)), fp, lcsc])

    cpl = [["Designator", "Mid X", "Mid Y", "Layer", "Rotation"]]
    unverified: dict[str, list[str]] = {}
    for p in sorted(placed, key=lambda p: _ref_key(p.ref)):
        rot = rotation_for(p.ref, p.footprint, rotations)
        if rot is None or not rot.verified:
            unverified.setdefault(p.footprint, []).append(p.ref)
        x, y = jlc_position(p, rot)
        cpl.append([p.ref, f"{x:.4f}mm", f"{y:.4f}mm", "Bottom" if p.bottom else "Top",
                    f"{jlc_rotation(p, rot):g}"])
    return Result(bom, cpl, unverified, dnp)


def write_csv(path: Path, rows: list[list[str]]) -> None:
    with open(path, "w", newline="", encoding="utf-8") as f:
        csv.writer(f, lineterminator="\r\n").writerows(rows)


# ---------------------------------------------------------------- kicad-cli in Docker


def docker_path(p: Path) -> str:
    return str(p).replace("\\", "/")


def kicad(board_dir: Path, out: Path, args: list[str]) -> None:
    cmd = ["docker", "run", "--rm", "-v", f"{docker_path(board_dir)}:/board:ro", "-v", f"{docker_path(out)}:/out",
           "-w", "/board", "-e", "HOME=/tmp", IMAGE, "kicad-cli", *args]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise CamError(f"kicad-cli {' '.join(args[:3])} failed ({r.returncode}):\n{r.stdout}{r.stderr}")


def pad_centres(board_dir: Path, board_name: str) -> str:
    cmd = ["docker", "run", "--rm", "-v", f"{docker_path(board_dir)}:/board:ro", "-w", "/board", "-e", "HOME=/tmp",
           IMAGE, "python3", "-c", PAD_CENTRES, board_name]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise CamError(f"pad centres failed ({r.returncode}):\n{r.stdout}{r.stderr}")
    return r.stdout[r.stdout.index("Ref,X,Y"):]


def safe_out(out: Path) -> Path:
    """Only a folder inside this repository's .cache/ is ever emptied."""
    out = out.resolve()
    cache = (ROOT / ".cache").resolve()
    if cache not in out.parents:
        raise CamError(f"output {out} is not inside {cache}")
    return out


def export(board: Path, sch: Path, out: Path) -> Result:
    board, sch = board.resolve(), sch.resolve()
    if board.parent != sch.parent:
        raise CamError("the board and the schematic must be in the same folder (one project)")
    out = safe_out(out)
    if out.exists():
        shutil.rmtree(out)
    gerbers, raw = out / "gerbers", out / "raw"
    gerbers.mkdir(parents=True)
    raw.mkdir()
    d = board.parent
    kicad(d, out, ["pcb", "export", "gerbers", "--output", "/out/gerbers/", "--layers", ",".join(LAYERS),
                   "--no-x2", "--no-netlist", "--subtract-soldermask", board.name])
    kicad(d, out, ["pcb", "export", "drill", "--output", "/out/gerbers/", "--format", "excellon",
                   "--excellon-units", "mm", "--excellon-zeros-format", "decimal", "--excellon-oval-format",
                   "alternate", "--excellon-separate-th", "--drill-origin", "absolute", "--generate-map",
                   "--map-format", "gerberx2", board.name])
    kicad(d, out, ["pcb", "export", "pos", "--output", "/out/raw/pos.csv", "--format", "csv", "--units", "mm",
                   "--side", "both", "--exclude-dnp", board.name])
    kicad(d, out, ["sch", "export", "bom", "--output", "/out/raw/bom.csv", "--fields", ",".join(BOM_FIELDS),
                   "--labels", ",".join(BOM_LABELS), "--group-by", "", "--ref-range-delimiter", "", sch.name])

    centres = pad_centres(d, board.name)
    (raw / "pad-centres.csv").write_text(centres, encoding="utf-8")
    result = build((raw / "bom.csv").read_text(encoding="utf-8"), (raw / "pos.csv").read_text(encoding="utf-8"),
                   read_rotations(ROTATIONS.read_text(encoding="utf-8")), parse_centres(centres))
    stem = board.stem
    write_csv(out / f"{stem}-bom-jlc.csv", result.bom)
    write_csv(out / f"{stem}-cpl-jlc.csv", result.cpl)
    files = sorted(p for p in gerbers.iterdir() if p.is_file())
    if not any(p.suffix.lower() == ".gm1" or "Edge_Cuts" in p.name for p in files):
        raise CamError("no board outline (Edge.Cuts) among the Gerbers")
    with zipfile.ZipFile(out / f"{stem}-gerbers.zip", "w", zipfile.ZIP_DEFLATED) as z:
        for p in files:
            z.write(p, p.name)
    return result


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="JLCPCB Gerbers, drill, BOM and CPL from a KiCad board.")
    ap.add_argument("--board", required=True, type=Path, help="the .kicad_pcb")
    ap.add_argument("--sch", required=True, type=Path, help="the root .kicad_sch")
    ap.add_argument("--out", type=Path, default=OUT, help=f"output folder inside .cache/ (default {OUT})")
    args = ap.parse_args(argv)
    try:
        result = export(args.board, args.sch, args.out)
    except CamError as e:
        print(f"cam: FAILED\n{e}", file=sys.stderr)
        return 1
    out = safe_out(args.out)
    for p in sorted(out.rglob("*")):
        if p.is_file():
            print(f"{p.stat().st_size:>9}  {p.relative_to(out).as_posix()}")
    parts = sum(len(r[1].split(",")) for r in result.bom[1:])
    print(f"BOM: {len(result.bom) - 1} lines, {parts} parts; CPL: {len(result.cpl) - 1} placements")
    if result.dnp:
        print(f"DNP, left out: {', '.join(result.dnp)}")
    if result.unverified:
        n = sum(len(v) for v in result.unverified.values())
        print(f"rotation not yet verified in JLC's preview ({n} placements; set verified=yes in "
              f"{ROTATIONS.name} once checked):")
        for fp, refs in sorted(result.unverified.items()):
            print(f"  {fp}: {', '.join(refs)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
