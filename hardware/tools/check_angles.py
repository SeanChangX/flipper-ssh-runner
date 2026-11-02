"""Fails when the copper has a track that is not at a multiple of 45 degrees, an acute angle
between two tracks of one net that meet on one layer (acid traps, a look nobody routes by hand),
or an unmitred 90-degree corner: two tracks of one net meeting end to end at a right angle
(every bend is a 45-degree mitre; T-junctions, where three or more tracks meet, are not bends).

    hardware/tools/kicad.sh python3 tools/check_angles.py [board]      # exit 1 on any finding

Track ends inside a pad or a via are not junctions (their copper covers the corner): a stub may
enter one at any angle, but the stub itself must still be at a multiple of 45 degrees.
"""
import math
import sys
from collections import defaultdict

import pcbnew

BOARD = sys.argv[1] if len(sys.argv) > 1 else "/project/hardware/ssh-runner.kicad_pcb"
ANGLE_TOL = 0.5          # degrees
ACUTE = 90.0 - ANGLE_TOL  # interior angles below this are reported
SHORT = 0.02             # mm: shorter segments carry no direction worth checking

mm = pcbnew.ToMM


def main():
    board = pcbnew.LoadBoard(BOARD)
    tracks = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
    findings = []
    ends = defaultdict(list)      # (layer, net, x, y) -> [(direction away from the point, track)]
    for t in tracks:
        a, b = t.GetStart(), t.GetEnd()
        ax, ay, bx, by = mm(a.x), mm(a.y), mm(b.x), mm(b.y)
        length = math.hypot(bx - ax, by - ay)
        if length < SHORT:
            continue
        ang = math.degrees(math.atan2(by - ay, bx - ax)) % 45.0
        if min(ang, 45.0 - ang) > ANGLE_TOL:
            findings.append("off-45   %-22s %-6s (%.2f, %.2f) -> (%.2f, %.2f)" %
                            (t.GetNetname(), board.GetLayerName(t.GetLayer()), ax, ay, bx, by))
        key = t.GetLayer(), t.GetNetCode()
        ends[key + (round(ax, 3), round(ay, 3))].append(math.atan2(by - ay, bx - ax))
        ends[key + (round(bx, 3), round(by, 3))].append(math.atan2(ay - by, ax - bx))
    pads = [p for f in board.GetFootprints() for p in f.Pads()]
    vias = [v for v in board.GetTracks() if v.GetClass() == "PCB_VIA"]
    for (layer, net, x, y), dirs in ends.items():
        if len(dirs) < 2:
            continue
        here = pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))
        if any(p.GetNetCode() == net and p.IsOnLayer(layer) and p.HitTest(here) for p in pads):
            continue
        if any(v.GetNetCode() == net and v.HitTest(here) for v in vias):
            continue
        dirs = sorted(d % (2 * math.pi) for d in dirs)
        gaps = [(dirs[(i + 1) % len(dirs)] - dirs[i]) % (2 * math.pi) for i in range(len(dirs))]
        worst = math.degrees(min(g for g in gaps if g > 1e-6) if any(g > 1e-6 for g in gaps) else 0)
        if worst < ACUTE:
            findings.append("acute    %-22s %-6s (%.2f, %.2f) %.0f deg" %
                            (board.GetNetInfo().GetNetItem(net).GetNetname(), board.GetLayerName(layer), x, y, worst))
        if len(dirs) == 2 and abs(worst - 90.0) < ANGLE_TOL:
            findings.append("corner90 %-22s %-6s (%.2f, %.2f) unmitred right-angle bend" %
                            (board.GetNetInfo().GetNetItem(net).GetNetname(), board.GetLayerName(layer), x, y))
        if len(dirs) != len(set(round(d, 4) for d in dirs)):
            findings.append("doubled  %-22s %-6s (%.2f, %.2f) two tracks leave in the same direction" %
                            (board.GetNetInfo().GetNetItem(net).GetNetname(), board.GetLayerName(layer), x, y))
    for f in findings:
        print(f)
    print("%d tracks, %d findings" % (len(tracks), len(findings)))
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
