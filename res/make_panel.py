#!/usr/bin/env python3
# Draws res/Boohm.svg. VCV Rack can't draw SVG <text>, so every label is turned
# into outline paths with fontTools. Positions are in mm and must match
# src/Boohm.cpp (the numbers in the LAYOUT block are the same in both).
#   pip install fonttools
#   python3 res/make_panel.py
import os
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_BOLD = os.environ.get("PANEL_FONT_BOLD", "/usr/share/fonts/opentype/inter/Inter-Bold.otf")
FONT_MED = os.environ.get("PANEL_FONT_MED", "/usr/share/fonts/opentype/inter/Inter-Medium.otf")

W, H = 132.08, 128.5  # 26 HP

BG = "#211F24"
BG_DARK = "#17161A"
LINE = "#3A3740"
INK = "#F1E9DA"
SOFT = "#A39B8E"
ACCENT = "#E8603C"   # kick red-orange
AMBER = "#F2A541"

# ---- LAYOUT (same numbers in src/Boohm.cpp) ----
COL = [12.5, 33.9, 55.3, 76.7, 98.1, 119.5]
R1_Y, R2_Y, R3_Y = 27.0, 49.0, 69.0
J1_Y, J2_Y, J3_Y = 87.0, 101.5, 116.0
LIST_LX = 6.0
MODEL_Y0, FX_Y0, LIST_DY = 25.0, 55.5, 4.0
SEL_BX = 21.6
MODEL_BY, FX_BY = 18.4, 48.4

CONTROL = 3.2   # control label size (project standard)
JACK = 2.9      # jack label size (project standard)

fonts = {}


def text(s, x, y, size, font=FONT_BOLD, fill=INK, anchor="middle", spacing=0.04):
    """SVG path for string s; (x, y) is the baseline point in mm."""
    if font not in fonts:
        f = TTFont(font)
        fonts[font] = (f, f.getGlyphSet(), f.getBestCmap(), f["head"].unitsPerEm)
    f, gs, cmap, upm = fonts[font]
    scale = size / upm
    track = spacing * upm
    names = [cmap[ord(c)] for c in s]
    width = sum(gs[n].width + track for n in names) - track
    x0 = {"middle": x - width * scale / 2, "end": x - width * scale}.get(anchor, x)
    pen = SVGPathPen(gs)
    cx = 0.0
    for n in names:
        gs[n].draw(TransformPen(pen, (scale, 0, 0, -scale, x0 + cx * scale, y)))
        cx += gs[n].width + track
    return f'<path fill="{fill}" d="{pen.getCommands()}"/>'


parts = []
add = parts.append
add(f'<rect width="{W}" height="{H}" fill="{BG}"/>')

# title with a little pitch-sweep curve after it
add(text("Boohm", 5.0, 11.4, 5.4, FONT_BOLD, INK, anchor="start", spacing=0.05))
sx, sy = 25.5, 7.2
add(f'<path d="M{sx} {sy} C{sx + 1.0} {sy + 3.6} {sx + 3.0} {sy + 4.2} {sx + 9.0} {sy + 4.2}" '
    f'stroke="{ACCENT}" stroke-width="0.6" fill="none" stroke-linecap="round"/>')
add(text("kick voice · after the Ohmforce Bohm", W - 5.0, 11.0, 2.5, FONT_MED, SOFT, anchor="end"))

# selector column: MODEL and FX TYPE lists
add(f'<rect x="2.6" y="14.2" width="{COL[0] + 11.0}" height="63.6" rx="1.4" fill="{BG_DARK}"/>')
add(text("MODEL", 4.0, MODEL_BY + 1.15, CONTROL, anchor="start"))
for i, n in enumerate(["ANLG", "FM-2", "MEMB", "GRIT"]):
    add(text(n, LIST_LX + 2.4, MODEL_Y0 + i * LIST_DY + 0.95, 2.6, FONT_MED, INK, anchor="start"))
add(text("FX TYPE", 4.0, FX_BY + 1.15, CONTROL, anchor="start"))
for i, n in enumerate(["TUBE", "BASS", "SOFT", "HARD", "FOLD", "CRUSH"]):
    add(text(n, LIST_LX + 2.4, FX_Y0 + i * LIST_DY + 0.95, 2.6, FONT_MED, INK, anchor="start"))

# knob rows
r1 = ["PITCH", "CURVE", "LENGTH", "ATTACK", "SUSTAIN"]
r2 = ["COLOR", "TRS DECAY", "TRS TONE", "VELOCITY", "FX"]
for i in range(5):
    add(text(r1[i], COL[i + 1], R1_Y - 7.6, CONTROL))
    add(text(r2[i], COL[i + 1], R2_Y - 7.6, CONTROL))
# scale hints
add(text("C1", COL[1] - 5.9, R1_Y + 7.2, 2.3, FONT_MED, SOFT))
add(text("C2", COL[1] + 6.6, R1_Y + 7.2, 2.3, FONT_MED, SOFT))
add(text("808", COL[2] - 6.6, R1_Y + 7.2, 2.3, FONT_MED, SOFT))
add(text("909", COL[2] + 6.6, R1_Y + 7.2, 2.3, FONT_MED, SOFT))
add(text("dark", COL[3] - 6.8, R2_Y + 7.2, 2.3, FONT_MED, SOFT))
add(text("bright", COL[3] + 7.4, R2_Y + 7.2, 2.3, FONT_MED, SOFT))

# row 3: attenuverters, stereo, random, hit
r3 = {1: "ATV", 2: "STEREO", 3: "RANDOM", 4: "HIT", 5: "ATV"}
for i, n in r3.items():
    add(text(n, COL[i], R3_Y - 6.0, CONTROL, fill=ACCENT if n == "HIT" else INK))
# little arrows tying the ATVs to PITCH and SUSTAIN
for i in (1, 5):
    add(f'<path d="M{COL[i]} {R2_Y + 9.2} v1.6" stroke="{LINE}" stroke-width="0.35"/>')

# CV inputs
add(f'<rect x="{COL[1] - 10.2}" y="79.2" width="{W - 2.6 - (COL[1] - 10.2)}" height="27.6" rx="1.4" fill="{BG_DARK}"/>')
add(f'<rect x="2.6" y="79.2" width="{COL[0] + 7.6}" height="27.6" rx="1.4" fill="{BG_DARK}"/>')
j1 = ["MODEL", "PITCH", "CURVE", "LENGTH", "ATTACK", "SUSTAIN"]
j2 = ["FX TYPE", "COLOR", "TRS DEC", "TRS TONE", "VEL", "FX"]
for i in range(6):
    add(text(j1[i], COL[i], J1_Y - 5.6, JACK, FONT_BOLD, SOFT))
    add(text(j2[i], COL[i], J2_Y - 5.6, JACK, FONT_BOLD, SOFT))

# bottom row: HIT and RANDOM in, outputs on a red-orange strip
add(text("HIT", COL[0], J3_Y - 5.8, JACK, fill=ACCENT))
add(text("RAND", COL[1], J3_Y - 5.8, JACK))
ox = COL[2] - 8.6
add(f'<rect x="{ox}" y="108.0" width="{W - 2.6 - ox}" height="13.4" rx="1.4" fill="{ACCENT}" fill-opacity="0.16"/>')
for i, n in [(2, "ENV"), (3, "ENV INV"), (4, "L"), (5, "R")]:
    add(text(n, COL[i], J3_Y - 5.8, JACK, fill=AMBER if n in ("L", "R") else INK))

svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">\n'
       + "\n".join(p for p in parts if p) + "\n</svg>\n")
open(os.path.join(HERE, "Boohm.svg"), "w").write(svg)
print("ok")

# --- Haer maker logo (res/haer_logo.py) on the bottom rail
import sys as _sys
_sys.path.insert(0, HERE)
import haer_logo
haer_logo.add_to_file(os.path.join(HERE, "Boohm.svg"), x=W / 2, y=126.0, cap=3.0, bg=BG)
