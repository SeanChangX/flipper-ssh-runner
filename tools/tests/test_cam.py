"""hardware/tools/cam.py: the JLC BOM and CPL from kicad-cli's ungrouped BOM and position file.
The pure part only; the kicad-cli exports need Docker and the board, and are not run here."""
import importlib.util
import os
import sys
from pathlib import Path

import pytest

CAM = Path(__file__).resolve().parents[2] / "hardware" / "tools" / "cam.py"
spec = importlib.util.spec_from_file_location("cam", CAM)
cam = importlib.util.module_from_spec(spec)
sys.modules["cam"] = cam  # dataclasses look their module up while the file runs
spec.loader.exec_module(cam)

HEAD = '"Reference","Value","Footprint","LCSC","DNP","ExcludeFromBOM","ExcludeFromBoard"\n'
BOM = HEAD + (
    '"C1","100nF","Capacitor_SMD:C_0402_1005Metric","C1525","","",""\n'
    '"C2","100nF","Capacitor_SMD:C_0402_1005Metric","C1525","","",""\n'
    '"C10","10uF","Capacitor_SMD:C_0603_1608Metric","C19702","","",""\n'
    '"U1","LDO","Package_TO_SOT_SMD:SOT-23-5","C5446","","",""\n'
    '"D1","LED","LED_SMD:LED_0603_1608Metric","C2286","","",""\n'
    '"R9","0R","Resistor_SMD:R_0402_1005Metric","","DNP","",""\n'
)
POS = (
    "Ref,Val,Package,PosX,PosY,Rot,Side\n"
    '"C1","100nF","C_0402_1005Metric",10.000000,-20.000000,0.000000,top\n'
    '"C2","100nF","C_0402_1005Metric",12.500000,-20.000000,90.000000,top\n'
    '"C10","10uF","C_0603_1608Metric",15.000000,-21.000000,0.000000,top\n'
    '"U1","LDO","SOT-23-5",20.000000,-22.000000,180.000000,top\n'
    '"D1","LED","LED_0603_1608Metric",30.000000,-25.000000,90.000000,bottom\n'
)
ROT = """# a comment
match,offset,verified,note
^SOT-23,180,no,
^LED_0603,90,yes,
ref:C2,270,yes,one part
"""


def rows(result, which):
    return {r[0]: r for r in getattr(result, which)[1:]}


def test_bom_groups_parts_and_leaves_dnp_out():
    r = cam.build(BOM, POS, cam.read_rotations(ROT))
    assert r.bom[0] == ["Comment", "Designator", "Footprint", "LCSC Part #"]
    assert r.bom[1:] == [
        ["100nF", "C1,C2", "C_0402_1005Metric", "C1525"],  # C2 before C10: natural order
        ["10uF", "C10", "C_0603_1608Metric", "C19702"],
        ["LED", "D1", "LED_0603_1608Metric", "C2286"],
        ["LDO", "U1", "SOT-23-5", "C5446"],
    ]
    assert r.dnp == ["R9"]
    assert "R9" not in rows(r, "cpl")


def test_cpl_rotations_top_bottom_and_overrides():
    r = cam.build(BOM, POS, cam.read_rotations(ROT))
    assert r.cpl[0] == ["Designator", "Mid X", "Mid Y", "Layer", "Rotation"]
    cpl = rows(r, "cpl")
    assert cpl["C1"] == ["C1", "10.0000mm", "-20.0000mm", "Top", "0"]
    assert cpl["C2"][4] == "0"  # 90 + the ref: override 270
    assert cpl["U1"][4] == "0"  # 180 + 180 for SOT-23
    # bottom: seen from below (180 - angle), the correction the other way: 180 - 90 - 90
    assert cpl["D1"][2:] == ["-25.0000mm", "Bottom", "0"]
    assert sorted(r.unverified) == ["C_0402_1005Metric", "C_0603_1608Metric", "SOT-23-5"]
    assert r.unverified["C_0402_1005Metric"] == ["C1"]  # C2's override is verified


def test_position_offset_turns_with_the_part():
    # a right-angle 1x8 header as on the board: KiCad origin pin 1, rotated -90; JLC's model centre
    # is the middle of the pin row, 8.89 mm down the row in the footprint's frame
    bom = HEAD + '"J1","Header","Connector_PinHeader_2.54mm:PinHeader_1x08","C492416","","",""\n'
    pos = ("Ref,Val,Package,PosX,PosY,Rot,Side\n"
           '"J1","Header","PinHeader_1x08_P2.54mm_Horizontal",162.870000,-146.160000,-90.000000,top\n')
    rot = "match,offset,dx,dy,verified,note\n^PinHeader_1x08,90,0,8.89,no,\n"
    r = cam.build(bom, pos, cam.read_rotations(rot))
    assert rows(r, "cpl")["J1"] == ["J1", "153.9800mm", "-146.1600mm", "Top", "0"]
    # rotated 180 the offset turns with it (an RJ45 body centre 4.44 / 6.61 mm from pin 1)
    pos180 = pos.replace("-90.000000", "180.000000")
    r = cam.build(bom, pos180, cam.read_rotations("match,offset,dx,dy,verified,note\n^PinHeader,0,4.44,6.61,no,\n"))
    assert rows(r, "cpl")["J1"][1:3] == ["158.4300mm", "-139.5500mm"]


def test_pad_centres_replace_origins_and_offsets_add_on_top():
    # kicad-jlcpcb-tools' convention: the soldered-pad centre, then the correction's offset
    bom = HEAD + '"J1","Header","x:PinHeader_1x08","C492416","","",""\n'
    pos = ("Ref,Val,Package,PosX,PosY,Rot,Side\n"
           '"J1","Header","PinHeader_1x08_P2.54mm_Horizontal",162.870000,-146.160000,-90.000000,top\n')
    centres = cam.parse_centres("Ref,X,Y\nJ1,153.980000,-146.160000\n")
    r = cam.build(bom, pos, cam.read_rotations("match,offset,dx,dy,verified,note\n^PinHeader,90,,,no,\n"), centres)
    assert rows(r, "cpl")["J1"] == ["J1", "153.9800mm", "-146.1600mm", "Top", "0"]
    r = cam.build(bom, pos, cam.read_rotations("match,offset,dx,dy,verified,note\n^PinHeader,90,0,1,no,\n"), centres)
    assert rows(r, "cpl")["J1"][1:3] == ["152.9800mm", "-146.1600mm"]


def test_position_offset_refused_on_the_bottom():
    bom = HEAD + '"J1","Header","x:PinHeader_1x08","C492416","","",""\n'
    pos = "Ref,Val,Package,PosX,PosY,Rot,Side\n" '"J1","Header","PinHeader_1x08",1,-1,0,bottom\n'
    with pytest.raises(cam.CamError):
        cam.build(bom, pos, cam.read_rotations("match,offset,dx,dy,verified,note\n^PinHeader,0,1,0,no,\n"))


def test_bottom_rotation_without_correction():
    p = cam.Placement("Q1", "X", 0, 0, 30, True)
    assert cam.jlc_rotation(p, None) == 150
    assert cam.jlc_rotation(cam.Placement("Q1", "X", 0, 0, 30, False), None) == 30


def test_missing_lcsc_fails_but_not_for_dnp():
    bad = BOM.replace('"C19702"', '""')
    with pytest.raises(cam.CamError, match=r"C10 \(10uF\) has no LCSC part number"):
        cam.build(bad, POS, [])
    with pytest.raises(cam.CamError, match="no LCSC part number"):
        cam.build(BOM.replace('"C5446"', '"5446"'), POS, [])
    cam.build(BOM, POS, [])  # R9 has none, but it is DNP


def test_duplicate_designators_fail():
    with pytest.raises(cam.CamError, match="C1 appears 2 times in the schematic"):
        cam.build(BOM + '"C1","1nF","Capacitor_SMD:C_0402_1005Metric","C1588","","",""\n', POS, [])
    with pytest.raises(cam.CamError, match="C1 appears 2 times on the board"):
        cam.build(BOM, POS + '"C1","100nF","C_0402_1005Metric",1,1,0,top\n', [])


def test_bom_and_cpl_must_name_the_same_parts():
    no_u1 = "".join(l for l in POS.splitlines(keepends=True) if not l.startswith('"U1"'))
    with pytest.raises(cam.CamError, match="U1 is in the BOM but not placed"):
        cam.build(BOM, no_u1, [])
    with pytest.raises(cam.CamError, match="J1 is placed but not in the BOM"):
        cam.build(BOM, POS + '"J1","Conn","PinHeader_1x02",1,1,0,top\n', [])
    # a part DNP in the schematic is left out of the CPL even where the board still places it
    fitted_on_board = POS + '"R9","0R","R_0402_1005Metric",1,1,0,top\n'
    assert "R9" not in rows(cam.build(BOM, fitted_on_board, []), "cpl")
    excluded = BOM.replace('"C10","10uF","Capacitor_SMD:C_0603_1608Metric","C19702","","",""',
                           '"C10","10uF","Capacitor_SMD:C_0603_1608Metric","C19702","","Excluded",""')
    with pytest.raises(cam.CamError, match="C10 is placed but not in the BOM"):
        cam.build(excluded, POS, [])


def test_every_error_is_reported_at_once():
    bad = BOM.replace('"C19702"', '""').replace('"C5446"', '""')
    with pytest.raises(cam.CamError) as e:
        cam.build(bad, POS, [])
    assert "C10" in str(e.value) and "U1" in str(e.value)


def test_unknown_side_fails():
    with pytest.raises(cam.CamError, match="unknown side"):
        cam.build(BOM, POS.replace("bottom", "inner"), [])


def test_rotation_table_shipped_parses():
    table = cam.read_rotations(cam.ROTATIONS.read_text(encoding="utf-8"))
    assert table and all(isinstance(r.offset, float) for r in table)
    assert cam.rotation_for("U9", "SOT-23-6", table).offset == -90  # kicad-jlcpcb-tools' default table


def test_output_only_inside_cache(tmp_path):
    with pytest.raises(cam.CamError, match="not inside"):
        cam.safe_out(tmp_path)
    assert cam.safe_out(cam.OUT) == cam.OUT.resolve()


def test_csv_is_crlf(tmp_path):
    p = tmp_path / "x.csv"
    cam.write_csv(p, [["a", "b,c"], ["1", "2"]])
    assert p.read_bytes() == b'a,"b,c"\r\n1,2\r\n'


@pytest.mark.skipif(not os.environ.get("CAM_BOARD"), reason="set CAM_BOARD and CAM_SCH (needs Docker)")
def test_export_real_board():
    r = cam.export(Path(os.environ["CAM_BOARD"]), Path(os.environ["CAM_SCH"]), cam.OUT)
    assert len(r.cpl) > 1 and len(r.bom) > 1
