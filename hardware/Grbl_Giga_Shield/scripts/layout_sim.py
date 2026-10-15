#!/usr/bin/env python3
"""layout_sim.py - first, simulated placement of the GIGA peripheral shield.

No schematic yet: blocks of parts with their real footprints' size, placed
against the real geometry of the GIGA R1 and the Display Shield (mech.py),
to see whether the plan of PLAN.md part 5 fits and how big the board gets.
Writes ../docs/layout_sim.svg: the front view (as one looks at the display;
the shield seen through, its parts face away) and a side view of the stack.

    python layout_sim.py
"""
import os

from mech import *  # noqa: F401,F403  (geometry constants)

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "docs", "layout_sim.svg")

# ------------------------------------------------------------------ shield
SH_X0, SH_X1 = -104.0, DISP_W        # 210 mm: right edge with the display's (USB side)
SH_Y0, SH_Y1 = 0.0, DISP_H
SH_R = 3.0
P = 3.81                              # pluggable terminal pitch
P5 = 5.08

# Zones: (polygon, kind). Everything else on the shield is logic.
ZONES = [
    ([(-104, 0), (-0.5, 0), (-0.5, 12.5), (-104, 12.5)], "mixed"),
    ([(-104, 12.5), (-92, 12.5), (-92, 50), (-104, 50)], "field"),
    ([(-104, 50), (3, 50), (3, 60.5), (13.5, 60.5), (13.5, 64.6), (82, 64.6), (82, 56.5), (14, 56.5),
      (14, 24), (106, 24), (106, 80), (-104, 80)], "field"),
]

# Blocks: (x0, y0, w, h, kind, title, sub)
B = []


def blk(x, y, w, h, kind, title, sub=""):
    B.append((x, y, w, h, kind, title, sub))


# --- left part: servos, inputs, power
for i, ax in enumerate("XYZA"):
    blk(-101.0 + i * 24.6, 1.0, 22.7, 9.0, "conn", "SERVO " + ax, "2×6")
blk(-88, 13.5, 18, 6.5, "opto", "OPTO ALM", "IN9–IN12")
blk(-62, 13.5, 11, 6.5, "field", "SON 24 V", "výstup")
blk(-46.5, 13.5, 13.5, 7, "ic", "AM26C31", "STEP")
blk(-31.5, 13.5, 13.5, 7, "ic", "AM26C31", "DIR")
blk(-16.5, 13.5, 13.5, 7, "ic", "AM26C31", "ENABLE")
blk(-104, 21.0, 8.5, 2 * P, "conn", "STOP", "")
blk(-104, 30.0, 8.5, 2 * P, "conn", "DRV", "PWR")
blk(-94, 21.5, 6.5, 6.6, "opto", "S", "")
blk(-94, 30.5, 6.5, 6.6, "opto", "P", "")
for i, (t, s) in enumerate([("IN1–IN4", "koncáky X, Y"), ("IN5–IN8", "koncáky Z, A"),
                            ("IN13–16", "sonda, tlačidlá"), ("IN17–20", "dvere, menič")]):
    blk(-86 + i * 17, 42.0, 15, 15, "opto", t, s)
blk(-19.0, 31.0, 20.3, 31.8, "power", "DC/DC 24→5 V", "izolovaný 2 A")
x = -102.0
for t in ["IN1–IN4", "IN5–IN8", "IN13–IN16", "IN17–IN20"]:
    blk(x, 70.0, 6 * P, 10.0, "conn", t, "+24V, 0V · 6×3,81")
    x += 6 * P + 1.5

# --- over the GIGA: outputs, 0-10 V, 24 V input, extension
blk(10.0, 1.0, 27.0, 9.0, "ext", "EXT / MPG", "2×8, voliteľné")
blk(J3_BODY[0], J3_BODY[1], J3_BODY[2] - J3_BODY[0], J3_BODY[3] - J3_BODY[1], "ext", "", "")
blk(67.5, 1.5, 15.0, 8.0, "ext", "EXT vstupy", "ESD, AM26C32")
blk(86.0, 1.5, 9.0, 8.0, "ui", "RESET", "")
for i, t in enumerate(["OUT1–4", "OUT5–8", "OUT9–12", "BRAKE"]):
    blk(79 - i * 21, 20.5, 17, 7, "opto", t, "opto")
blk(60.0, 30.5, 12.0, 10.5, "field", "TBD62083", "OUT1–8")
blk(43.0, 30.5, 12.0, 10.5, "field", "TBD62083", "OUT9–12")
blk(17.0, 30.0, 17.0, 12.0, "power", "BRAKE 5 A", "MOSFET, dióda")
blk(55.0, 45.0, 10 * P, 8.0, "conn", "OUT1–OUT8", "+24V ×2 · vertikálne")
blk(30.0, 45.0, 6 * P, 8.0, "conn", "OUT9–12", "+24V, 0V")
blk(16.0, 45.0, 2 * P5, 8.0, "conn", "BRAKE", "5,08")
blk(16.0, 63.0, 15.0, 7.0, "field", "0–10 V", "AMC1311+OPA")
blk(18.0, 71.0, 2 * P, 9.0, "conn", "0–10V", "")
blk(30.0, 70.0, 2 * P5, 10.0, "conn", "24V IN", "5,08")
blk(42.5, 67.0, 21.0, 11.0, "power", "ochrana 24 V", "PTC, TVS, P-FET")
blk(66.0, 68.5, 13.0, 8.0, "ui", "LED", "24 V, 5 V")

# ------------------------------------------------------------------ SVG
S = 4.0                                   # px per mm
VX0, VX1 = -110.0, 156.0                  # front view x range (mm)
OX, OY = 30.0, 96.0                       # px offset of the front view
SIDE_Z0, SIDE_Z1 = -11.0, 50.0
SOY = OY + (DISP_H + 22) * S             # side view origin
W = OX * 2 + (VX1 - VX0) * S
H = SOY + (SIDE_Z1 - SIDE_Z0) * S + 230

C = {
    "bg": "#ffffff", "ink": "#1f2937", "dim": "#6b7280", "faint": "#9ca3af",
    "logic": "#e8f0fb", "logic_s": "#1e3a5f", "field": "#fde8d2", "field_s": "#d9822b",
    "mixed": "#efe4f8", "mixed_s": "#8b5cf6",
    "conn": "#374151", "opto": "#0f766e", "ic": "#1d4ed8", "field_b": "#c2410c", "power": "#b91c1c",
    "ext": "#7c3aed", "ui": "#15803d", "giga": "#0d9488", "disp": "#475569", "pin": "#111827",
}
KIND_FILL = {"conn": C["conn"], "opto": C["opto"], "ic": C["ic"], "field": C["field_b"], "power": C["power"],
             "ext": C["ext"], "ui": C["ui"]}

svg = []


def fx(x):
    return OX + (x - VX0) * S


def fy(y):
    return OY + y * S


def sz(z):
    return SOY + (z - SIDE_Z0) * S


def esc(t):
    return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, t, size=11, fill=None, anchor="start", weight="normal", italic=False):
    svg.append('<text x="%.1f" y="%.1f" font-size="%.1f" fill="%s" text-anchor="%s" font-weight="%s"%s>%s</text>'
               % (x, y, size, fill or C["ink"], anchor, weight, ' font-style="italic"' if italic else "", esc(t)))


def rect(x0, y0, x1, y1, fill="none", stroke="none", sw=1.0, dash=None, rx=0, op=1.0):
    svg.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%.1f" fill="%s" stroke="%s" '
               'stroke-width="%.2f"%s opacity="%.2f"/>'
               % (min(x0, x1), min(y0, y1), abs(x1 - x0), abs(y1 - y0), rx, fill, stroke, sw,
                  ' stroke-dasharray="%s"' % dash if dash else "", op))


def poly(pts, fill="none", stroke="none", sw=1.0, dash=None, op=1.0):
    svg.append('<polygon points="%s" fill="%s" stroke="%s" stroke-width="%.2f"%s opacity="%.2f"/>'
               % (" ".join("%.1f,%.1f" % p for p in pts), fill, stroke, sw,
                  ' stroke-dasharray="%s"' % dash if dash else "", op))


def line(x0, y0, x1, y1, stroke, sw=1.0, dash=None):
    svg.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="%.2f"%s/>'
               % (x0, y0, x1, y1, stroke, sw, ' stroke-dasharray="%s"' % dash if dash else ""))


def circle(x, y, r, fill="none", stroke="none", sw=1.0):
    svg.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s" stroke="%s" stroke-width="%.2f"/>'
               % (x, y, r, fill, stroke, sw))


svg.append('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
           'font-family="Segoe UI, Arial, sans-serif">' % (W, H, W, H))
svg.append('<defs><pattern id="hatch" width="6" height="6" patternUnits="userSpaceOnUse" '
           'patternTransform="rotate(45)"><line x1="0" y1="0" x2="0" y2="6" stroke="#dc2626" '
           'stroke-width="1" opacity="0.45"/></pattern></defs>')
rect(0, 0, W, H, fill=C["bg"])

text(OX, 34, "Shield pre GIGA R1 — simulované rozloženie súčiastok (koncept v1, 14. 10. 2026)", 20, weight="bold")
text(OX, 58, "Pohľad spredu (od displeja), mierka v mm. Shield 210 × 80 mm, vpravo zarovno s displejom (USB strana), "
     "104 mm presahuje doľava.", 12.5, fill=C["dim"])
text(OX, 76, "Súčiastky shieldu sú na jeho zadnej strane — kreslené cez dosku. Polohy GIGA, displeja a ich "
     "konektorov sú z Altium a STEP súborov Arduina.", 12.5, fill=C["dim"])

# --- shield and zones
rect(fx(SH_X0), fy(SH_Y0), fx(SH_X1), fy(SH_Y1), fill=C["logic"], stroke=C["logic_s"], sw=2, rx=SH_R * S)
for pts, kind in ZONES:
    poly([(fx(x), fy(y)) for x, y in pts], fill=C[kind], stroke=C[kind + "_s"], sw=1.2, dash="5,3", op=0.9)

# --- display and GIGA outlines (behind the shield)
rect(fx(0), fy(0), fx(DISP_W), fy(DISP_H), stroke=C["disp"], sw=1.6, dash="10,4", rx=DISP_R * S)
rect(fx(TFT_AREA[0]), fy(TFT_AREA[1]), fx(TFT_AREA[2]), fy(TFT_AREA[3]), stroke=C["faint"], sw=1, dash="2,3")
gpts = [giga(u, v) for u, v in GIGA_OUTLINE_UV]
poly([(fx(x), fy(y)) for x, y in gpts], stroke=C["giga"], sw=1.8, dash="7,3")
for (n, u0, v0, u1, v1, h, note) in GIGA_PARTS:
    x0, y0 = giga(u0, v0)
    x1, y1 = giga(u1, v1)
    rect(fx(x0), fy(y0), fx(x1), fy(y1), fill="url(#hatch)" if h > 6 else "none", stroke="#dc2626", sw=0.8, dash="3,2")
    text((fx(x0) + fx(x1)) / 2, fy(max(y0, y1)) - 2, n, 8.5, fill="#b91c1c", anchor="middle")
for u, v in GIGA_HOLES_UV:
    x, y = giga(u, v)
    circle(fx(x), fy(y), 1.6 * S, stroke=C["giga"], sw=0.8)

# --- header pins (the shield's male headers into the GIGA)
for _, u in JDIGITAL:
    x, y = giga(u, JDIGITAL_V)
    rect(fx(x) - 3, fy(y) - 3, fx(x) + 3, fy(y) + 3, fill=C["pin"])
for _, u in JANALOG:
    x, y = giga(u, JANALOG_V)
    rect(fx(x) - 3, fy(y) - 3, fx(x) + 3, fy(y) + 3, fill=C["pin"])
for col, u in enumerate(JSIDE_U):
    for r in range(18):
        x, y = giga(u, JSIDE_V0 + 2.54 * r)
        rect(fx(x) - 3, fy(y) - 3, fx(x) + 3, fy(y) + 3, fill=C["pin"])

# --- blocks
for (x, y, w, h, kind, title, sub) in B:
    rect(fx(x), fy(y), fx(x + w), fy(y + h), fill=KIND_FILL[kind], stroke="#ffffff", sw=1, rx=3, op=0.93)
    cx, cy = fx(x + w / 2), fy(y + h / 2)
    small = (w * S < 70) or (h * S < 30)
    if sub and not (h * S < 26):
        text(cx, cy - 1, title, 10 if small else 11, fill="#ffffff", anchor="middle", weight="bold")
        text(cx, cy + 11, sub, 8.5 if small else 9.5, fill="#e5e7eb", anchor="middle")
    else:
        text(cx, cy + 4, title, 9.5 if small else 11, fill="#ffffff", anchor="middle", weight="bold")

# --- J3 contacts (long pins down into the display's socket)
for xp in J3_COLS_X:
    for yp in J3_ROWS_Y:
        circle(fx(xp), fy(yp), 2.2, fill="#ffffff")

# --- holes for the M3 standoffs (display corners)
for x, y in DISP_HOLES:
    circle(fx(x), fy(y), 1.6 * S, fill="#ffffff", stroke=C["ink"], sw=1.4)
    circle(fx(x), fy(y), 2.75 * S, stroke=C["ink"], sw=0.6)

# --- things outside: USB stick, USB-C cable
x0, y0 = giga(-2.55, 24.0)
x1, y1 = giga(-48.0, 35.3)
rect(fx(x0), fy(y0), fx(x1), fy(y1), fill="#f3f4f6", stroke=C["dim"], sw=1, rx=4)
text((fx(x0) + fx(x1)) / 2, (fy(y0) + fy(y1)) / 2 + 4, "USB kľúč", 10.5, fill=C["dim"], anchor="middle")
x0, y0 = giga(-0.98, 10.6)
x1, y1 = giga(-30.0, 17.1)
rect(fx(x0), fy(y0), fx(x1), fy(y1), fill="#f3f4f6", stroke=C["dim"], sw=1, rx=6)
text((fx(x0) + fx(x1)) / 2, (fy(y0) + fy(y1)) / 2 + 4, "USB-C k PC", 10, fill=C["dim"], anchor="middle")

# --- labels on the geometry
text(fx(53), fy(-2.5), "J3 na displeji: D54–D67 (dlhé piny)", 10.5, fill=C["ext"], anchor="middle", weight="bold")
text(fx(103.5), fy(62.8), "GIGA R1", 10, fill=C["giga"], anchor="end", weight="bold")

# dimensions
yd = fy(SH_Y1) + 30
line(fx(SH_X0), yd, fx(SH_X1), yd, C["dim"], 1)
for xx in (SH_X0, 0, SH_X1):
    line(fx(xx), yd - 6, fx(xx), yd + 6, C["dim"], 1)
text(fx((SH_X0 + 0) / 2), yd - 4, "104 mm (presah doľava)", 11, fill=C["dim"], anchor="middle")
text(fx(DISP_W / 2), yd - 4, "106 mm = šírka displeja", 11, fill=C["dim"], anchor="middle")
xd = fx(SH_X0) - 14
line(xd, fy(0), xd, fy(80), C["dim"], 1)
text(xd - 4, fy(40), "80", 11, fill=C["dim"], anchor="end")

# ------------------------------------------------------------------ side view
text(OX, SOY - 34, "Bočný rez (pohľad zdola, x ako hore, z = smer od displeja dozadu)", 14, weight="bold")
line(fx(VX0 + 4), sz(SIDE_Z0 + 1), fx(VX0 + 4), sz(SIDE_Z1 - 2), C["faint"], 1)
text(fx(VX0 + 6), sz(-8.5), "↑ spredu (displej)", 10, fill=C["dim"])
text(fx(VX0 + 6), sz(48.5), "↓ dozadu (svorky)", 10, fill=C["dim"])


def srect(x0, x1, z0, z1, fill, stroke="none", label=None, lsize=9.5, lfill=None, op=1.0):
    rect(fx(x0), sz(z0), fx(x1), sz(z1), fill=fill, stroke=stroke, sw=0.8, op=op)
    if label:
        text((fx(x0) + fx(x1)) / 2, (sz(z0) + sz(z1)) / 2 + 3.5, label, lsize, fill=lfill or "#ffffff", anchor="middle")


srect(TFT_AREA[0], TFT_AREA[2], Z_TFT_FRONT, -5.0, "#9ca3af", label="sklo TFT", lsize=9)
srect(0, DISP_W, Z_DISP_PCB[0], Z_DISP_PCB[1], "#475569")
srect(J3_BODY[0], J3_BODY[2], 0.05, 3.71, C["ext"], label="J3", lsize=8.5)
for xc in (85.74, 23.27):                      # TSM pins display -> GIGA
    srect(xc - 2.5, xc + 2.5, 0.0, Z_DISP_PARTS, "#64748b")
    for k in (-1.27, 1.27):
        line(fx(xc + k), sz(0), fx(xc + k), sz(Z_TSM_TIP), "#64748b", 1.2)
gx0, _ = giga(GIGA_W, 0)
gx1, _ = giga(0, 0)
srect(gx0, gx1, Z_GIGA[0], Z_GIGA[1], C["giga"], label="GIGA R1", lsize=8.5)
srect(*sorted((giga(-2.55, 0)[0], giga(13.65, 0)[0])), Z_GIGA[1] - 3.9, Z_GIGA[1] + 7.78, "#9ca3af",
      label="USB-A", lsize=8.5)
srect(giga(-2.55, 0)[0], giga(-40, 0)[0], Z_GIGA[1] - 1.0, Z_GIGA[1] + 6.5, "#e5e7eb", stroke=C["dim"],
      label="kľúč", lsize=9, lfill=C["dim"])
for (u0, u1) in [(JSIDE_U[0] - 1.27, JSIDE_U[1] + 1.27), (JDIGITAL[0][1] - 1.27, JDIGITAL[-1][1] + 1.27)]:
    srect(*sorted((giga(u0, 0)[0], giga(u1, 0)[0])), Z_GIGA[1], Z_GIGA[1] + Z_GIGA_HEADER, "#d6c7a1")
srect(*sorted((giga(61.98, 0)[0], giga(67.62, 0)[0])), Z_GIGA[1], Z_GIGA[1] + 8.61, "#dc2626")
srect(SH_X0, SH_X1, Z_SHIELD[0], Z_SHIELD[1], C["logic_s"])
for xc in (DISP_HOLES[0][0], DISP_HOLES[1][0]):      # standoffs
    srect(xc - 2.75, xc + 2.75, 0, Z_SHIELD[0], "#cbd5e1", stroke=C["dim"])
for xp in J3_COLS_X[::3]:                       # long pins into J3
    line(fx(xp), sz(1.5), fx(xp), sz(Z_SHIELD[1] + 2.5), C["ext"], 1.4)
srect(J3_BODY[0], J3_BODY[2], Z_SHIELD[1], Z_SHIELD[1] + 2.5, C["ext"])
zt = Z_SHIELD[1]
srect(-101, -2.7, zt, zt + 9.0, C["conn"], label="servo 2×6", lsize=9)
srect(-102, -5.9, zt + 9.0, zt + 21.0, C["conn"], label="svorky vstupov (zástrčky)", lsize=9, op=0.85)
srect(-19.0, 1.3, zt, zt + 10.2, C["power"], label="DC/DC", lsize=8.5)
srect(16.0, 93.1, zt, zt + 18.0, C["conn"], label="svorky výstupov, vertikálne (so zástrčkou)", lsize=9, op=0.85)

# z dimensions
def mmf(v, nd=1):
    return ("%.*f" % (nd, v)).replace(".", ",")


def zdim(x, z0, z1, label, anchor="start", dx=6):
    line(fx(x), sz(z0), fx(x), sz(z1), C["ink"], 1)
    line(fx(x) - 4, sz(z0), fx(x) + 4, sz(z0), C["ink"], 1)
    line(fx(x) - 4, sz(z1), fx(x) + 4, sz(z1), C["ink"], 1)
    text(fx(x) + (dx if anchor == "start" else -dx), (sz(z0) + sz(z1)) / 2 + 4, label, 10, anchor=anchor)


for zz in (0, Z_GIGA[0], Z_GIGA[1], Z_SHIELD[0]):
    line(fx(-60), sz(zz), fx(0), sz(zz), C["faint"], 0.8, dash="3,3")
zdim(-4, 0, Z_GIGA[0], "≈ %s mm displej–GIGA (zo STEP, zmerať)" % mmf(Z_GIGA[0]), anchor="end")
zdim(-4, Z_GIGA[1], Z_SHIELD[0], "%s mm GIGA–shield (8,5 + 2,5)" % mmf(Z_SHIELD[0] - Z_GIGA[1]), anchor="end")
zdim(-62, 0, Z_SHIELD[0], "stĺpiky M3 ≈ %.0f mm" % STANDOFF_LEN, anchor="end")
text(fx(53), sz(8.6), "kolíky do J3 ≈ %.0f mm" % J3_PIN_BELOW, 10, fill=C["ext"], anchor="middle",
     weight="bold")
text(fx(41), sz(Z_GIGA[1] + 11.2), "ICSP 8,6 mm", 9, fill="#b91c1c", anchor="middle")

# ------------------------------------------------------------------ legend and notes
ly = sz(SIDE_Z1) + 26
items = [(C["logic"], C["logic_s"], "logika (GND GIGA, 3,3/5 V)"), (C["field"], C["field_s"], "izolovaná 24 V strana"),
         (C["mixed"], C["mixed_s"], "konektory serv: diferenčné + 24 V signály")]
xx = OX
for fill, st, t in items:
    rect(xx, ly - 12, xx + 26, ly + 4, fill=fill, stroke=st, dash="5,3")
    text(xx + 32, ly, t, 11.5)
    xx += 300
xx = OX
ly += 26
for kind, t in [("conn", "konektory a svorky"), ("opto", "optočleny"), ("ic", "budiče AM26C31"),
                ("field", "výkonové výstupy"), ("power", "napájanie, brzda"), ("ext", "rozšírenie J3 / EXT"),
                ("ui", "tlačidlo, LED")]:
    rect(xx, ly - 12, xx + 18, ly + 4, fill=KIND_FILL[kind], rx=2)
    text(xx + 24, ly, t, 11.5)
    xx += 140
ly += 20
rect(OX, ly - 11, OX + 18, ly + 3, fill="url(#hatch)", stroke="#dc2626", dash="3,2")
text(OX + 24, ly, "diel na GIGA pod shieldom vyšší ako 6 mm — spodná strana shieldu bez vývodov THT", 11.5)
circle(OX + 560, ly - 4, 6, fill="#ffffff", stroke=C["ink"], sw=1.4)
text(OX + 572, ly, "otvor M3 v rohoch displeja — stĺpiky displej ↔ shield", 11.5)
ly += 20
rect(OX + 6, ly - 7, OX + 12, ly - 1, fill=C["pin"])
text(OX + 24, ly, "kolíky do GIGA: JDIGITAL hore (výstupy D2–D16), JANALOG dole (DAC0, 5 V), JSIDE 2×18 vľavo (kroky, vstupy)", 11.5)

notes = [
    "1. GIGA na displeji (z konektorov J6/J7 v Altium súboroch): USB okraj 0,16 mm od pravého okraja displeja, horný okraj GIGA 10,73 mm pod horným okrajom displeja.",
    "2. Medzera displej–GIGA ≈ 12,6 mm vychádza zo STEP modelu kolíkov (17,9 mm nad displejom); v dokumentácii nie je — treba zmerať. Od nej závisia stĺpiky aj kolíky do J3.",
    "3. J3 (2×10) na displeji: 14 voľných GPIO D54–D67 + 3,3 V + GND. Piny 17/18 sú I2C4 = Wire1, zbernica dotyku a RGB diódy — na shielde nepripájať.",
    "4. Tlačidlá RESET a BOOT na GIGA zakryje shield: RESET je na shielde. Výstupy majú svorky zhora, aby 24 V nemuselo krížiť logické rady konektorov.",
]
ly += 30
for n in notes:
    text(OX, ly, n, 11.5, fill=C["ink"])
    ly += 19

svg.append("</svg>")
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(svg))
print("wrote", os.path.normpath(OUT), "%dx%d px" % (W, H))
