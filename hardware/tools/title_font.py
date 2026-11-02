"""Replaces the "SSH Runner" title on the back silkscreen with outlines of a TrueType font.

    hardware/tools/kicad.sh python3 tools/title_font.py FONT IN.kicad_pcb OUT.kicad_pcb [--bold]

FONT is "Newstroke" (KiCad's built-in stroke font: the board is copied unchanged), "Inter"
or "Orbitron". The font files are not part of the repository: they live in
.cache/kicad/home/.fonts (SIL OFL 1.1, from google/fonts, see THIRD_PARTY.md), and only the
resulting outlines, plain silkscreen polygons, go into the board. So the board does not
depend on the font being installed.
"""
import sys

import pcbnew

TITLE = "SSH Runner"


def main(font, src, dst, bold):
    board = pcbnew.LoadBoard(src)
    if font.lower() != "newstroke":
        titles = [d for d in board.GetDrawings()
                  if isinstance(d, pcbnew.PCB_TEXT) and d.GetText() == TITLE]
        if len(titles) != 1:
            raise SystemExit("expected one %r text, found %d" % (TITLE, len(titles)))
        t = titles[0]
        t.SetUnresolvedFontName(font)
        if not t.ResolveFont(None) or t.GetFontName() != font:
            raise SystemExit("font %r not found (fonts go in .cache/kicad/home/.fonts)" % font)
        t.SetBold(bold)
        outline = pcbnew.SHAPE_POLY_SET()
        t.TransformTextToPolySet(outline, 0, pcbnew.FromMM(0.002), pcbnew.ERROR_INSIDE)
        for i in range(outline.OutlineCount()):      # one polygon per glyph outline, with its holes
            glyph = pcbnew.SHAPE_POLY_SET()
            glyph.AddOutline(outline.Outline(i))
            for h in range(outline.HoleCount(i)):
                glyph.AddHole(outline.Hole(i, h))
            shape = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY)
            shape.SetPolyShape(glyph)
            shape.SetFilled(True)
            shape.SetWidth(0)
            shape.SetLayer(t.GetLayer())
            board.Add(shape)
        board.Remove(t)
        bb = outline.BBox()
        print("%s: %.2f x %.2f mm" % (font, pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())))
    pcbnew.SaveBoard(dst, board)


main(sys.argv[1], sys.argv[2], sys.argv[3], "--bold" in sys.argv)
