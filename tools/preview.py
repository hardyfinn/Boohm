#!/usr/bin/env python3
# Mock-up picture of the panel with knobs, jacks and lights drawn in, so the
# layout can be checked without Rack.  python3 tools/preview.py out.png
import os, sys, math, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
svg = open(os.path.join(HERE, "..", "res", "Boohm.svg")).read()
L = {}
src = open(os.path.join(HERE, "..", "res", "make_panel.py")).read()
exec(src.split("# ---- LAYOUT")[1].split("CONTROL =")[0].split("\n", 1)[1], L)
C = L["COL"]
g = []
def jack(x, y):
    g.append(f'<circle cx="{x}" cy="{y}" r="4.1" fill="#C9CCD1"/><circle cx="{x}" cy="{y}" r="3.1" fill="#7C8088"/><circle cx="{x}" cy="{y}" r="1.9" fill="#0B0B0C"/>')
def knob(x, y, a, r=4.8):
    g.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="#26272B" stroke="#3A3C42" stroke-width="0.5"/>')
    ang = math.radians(-135 + 270 * a)
    g.append(f'<line x1="{x}" y1="{y}" x2="{x + (r-0.8)*math.sin(ang):.2f}" y2="{y - (r-0.8)*math.cos(ang):.2f}" stroke="#EEE" stroke-width="0.7" stroke-linecap="round"/>')
def light(x, y, b, col="#FFD23C"):
    g.append(f'<circle cx="{x}" cy="{y}" r="0.9" fill="#3a3320"/><circle cx="{x}" cy="{y}" r="0.9" fill="{col}" fill-opacity="{b}"/>')
def button(x, y, r=2.6, col="#4a4126"):
    g.append(f'<circle cx="{x}" cy="{y}" r="{r+1.0}" fill="#2B2D33"/><circle cx="{x}" cy="{y}" r="{r}" fill="{col}"/>')
v1 = [0.25, 0.35, 0.55, 0.5, 0.8]; v2 = [0.0, 0.3, 0.5, 1.0, 0.2]
for i in range(5):
    knob(C[i + 1], L["R1_Y"], v1[i]); knob(C[i + 1], L["R2_Y"], v2[i])
knob(C[1], L["R3_Y"], 0.5, 3.2); knob(C[5], L["R3_Y"], 1.0, 3.2); knob(C[2], L["R3_Y"], 0.3, 3.8)
button(C[3], L["R3_Y"], 3.0); button(C[4], L["R3_Y"], 3.0, "#7a2a20")
button(L["SEL_BX"], L["MODEL_BY"], 1.6, "#55575e"); button(L["SEL_BX"], L["FX_BY"], 1.6, "#55575e")
for i in range(4): light(L["LIST_LX"], L["MODEL_Y0"] + i * L["LIST_DY"], 1.0 if i == 0 else 0)
for i in range(6): light(L["LIST_LX"], L["FX_Y0"] + i * L["LIST_DY"], 1.0 if i == 0 else 0)
for i in range(6):
    jack(C[i], L["J1_Y"]); jack(C[i], L["J2_Y"]); jack(C[i], L["J3_Y"])
W = L["W"] if "W" in L else 132.08
for x in (7.6, 132.08 - 7.6 - 5.08):
    for y in (2.5, 126.0): g.append(f'<circle cx="{x}" cy="{y}" r="1.6" fill="#8A8D93"/>')
svg = svg.replace("</svg>", "\n".join(g) + "\n</svg>")
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "preview.png")
tmp = out + ".svg"; open(tmp, "w").write(svg)
pw, ph = 660, 643
open(tmp + ".html", "w").write(f'<html><body style="margin:0;background:#111"><img src="{os.path.basename(tmp)}" style="width:{pw}px;height:{ph}px"></body></html>')
js = f"""const {{chromium}} = require('playwright');
(async () => {{ const b = await chromium.launch(); const p = await b.newPage({{viewport: {{width: {pw}, height: {ph}}}, deviceScaleFactor: 2}});
await p.goto('file://{tmp}.html'); await p.waitForTimeout(300);
await p.screenshot({{path: '{out}'}}); await b.close(); }})();"""
open(out + ".js", "w").write(js)
subprocess.run(["node", out + ".js"], check=True, cwd="/tmp")
os.remove(out + ".js"); os.remove(tmp); os.remove(tmp + ".html")
print("wrote", out)
