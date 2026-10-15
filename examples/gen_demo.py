import math, io

OUT = "D:/ClaudeCode/Grbl_Uno_R4_Wifi/examples/demo_complex.nc"
ZS, ZW = 5.0, 0.5          # safe height / working height
L = []                      # output lines
pos = [0.0, 0.0, 0.0]       # tracked position, for the length estimate
dist = 0.0

def emit(s): L.append(s)
def sec(t):
    emit(""); emit("(%s)" % ("-"*54)); emit("( %s )" % t); emit("(%s)" % ("-"*54))
def f(v):
    s = "%.3f" % v
    return s.rstrip("0").rstrip(".") if "." in s else s

def move(code, x=None, y=None, z=None, feed=None, extra=""):
    global dist
    t = [x if x is not None else pos[0],
         y if y is not None else pos[1],
         z if z is not None else pos[2]]
    w = code
    if x is not None: w += "X%s" % f(x)
    if y is not None: w += "Y%s" % f(y)
    if z is not None: w += "Z%s" % f(z)
    w += extra
    if feed is not None: w += "F%s" % f(feed)
    emit(w)
    dist += math.dist(pos, t); pos[:] = t

def arc(cw, x, y, i, j, z=None, feed=None):
    """Explicit end point on every arc: Grbl rejects an arc block with no axis
    word, and an end point equal to the start is how a full circle is written."""
    global dist
    cx, cy = pos[0] + i, pos[1] + j
    r = math.hypot(i, j)
    a0 = math.atan2(pos[1] - cy, pos[0] - cx)
    a1 = math.atan2(y - cy, x - cx)
    d = a1 - a0
    if cw:
        while d >= -1e-9: d -= 2*math.pi
    else:
        while d <= 1e-9: d += 2*math.pi
    dist += abs(d) * r
    w = "%sX%sY%s" % ("G2" if cw else "G3", f(x), f(y))
    if z is not None: w += "Z%s" % f(z)
    w += "I%sJ%s" % (f(i), f(j))
    if feed is not None: w += "F%s" % f(feed)
    emit(w)
    pos[0], pos[1] = x, y
    if z is not None: pos[2] = z

def goto(x, y, feed=None):
    move("G0" if feed is None else "G1", x=x, y=y, feed=feed)
def plunge():  move("G1", z=ZW, feed=300)
def retract(): move("G0", z=ZS)
def travel(x, y):
    retract(); goto(x, y)

# ---------------------------------------------------------------- preamble
emit("( Grbl-R4 demo program - Arduino UNO R4 WiFi / Renesas RA4M1 )")
emit("( )")
emit("( Envelope: X 0..98  Y 0..98  Z 0..5 mm, relative to wherever the )")
emit("( machine is standing when the program starts. Nothing ever goes )")
emit("( below Z0, so with the tool clear of the work this is a dry run. )")
emit("( )")
emit("( Before starting: jog to the front-left of the area you want used, )")
emit("( then zero X, Y and Z in UGS. The G10 L20 below zeroes them again )")
emit("( in case you did not - it does NOT need homing. It sets the G54 )")
emit("( offset, which survives a stop/reset; a G92 would not, and the )")
emit("( shown position would jump back when the program is stopped. )")
emit("( )")
emit("( Exercises: G0/G1, G2/G3 arcs and full circles, helical arcs, )")
emit("( G17/G18/G19 planes, G90/G91, G4 dwell, feed changes, and a long )")
emit("( fine-segment raster that keeps the planner and the link busy. )")
emit("(RUNTIME)")
emit("")
emit("G21 (mm)")
emit("G90 (absolute)")
emit("G17 (XY plane)")
emit("G94 (units per minute)")
emit("G54 (work coordinate system 1)")
emit("G10L20P1X0Y0Z0 (treat the current position as the origin)")
emit("G0Z%s" % f(ZS))
emit("")
emit("( Spindle and coolant are left commented out on purpose: this file )")
emit("( does not know what you have wired to those outputs. Uncomment if )")
emit("( you want to exercise the PWM output on D11 and the relays. )")
emit("(M3 S1000)")
emit("(M8)")
emit("G4P0.5 (dwell half a second)")

# ---------------------------------------------------------------- 1. frame
sec("1/8  Frame - straight moves, four right angles")
travel(2, 2); plunge()
move("G1", x=98, feed=900)
move("G1", y=98)
move("G1", x=2)
move("G1", y=2)

# ---------------------------------------------------------------- 2. circles
sec("2/8  Concentric full circles - G2 and G3, zero-travel arcs")
cx, cy = 22.0, 55.0
for k, r in enumerate((4.0, 8.0, 12.0)):
    travel(cx - r, cy); plunge()
    arc(k % 2 == 0, cx - r, cy, r, 0.0, feed=700)

# ---------------------------------------------------------------- 3. rounded rect
sec("3/8  Rounded rectangle - arcs blended into straight moves")
x0, y0, x1, y1, r = 46.0, 43.0, 78.0, 67.0, 6.0
travel(x0 + r, y0); plunge()
move("G1", x=x1 - r, feed=800)
arc(False, x1, y0 + r, 0.0, r)
move("G1", y=y1 - r)
arc(False, x1 - r, y1, -r, 0.0)
move("G1", x=x0 + r)
arc(False, x0, y1 - r, 0.0, -r)
move("G1", y=y0 + r)
arc(False, x0 + r, y0, r, 0.0)

# ---------------------------------------------------------------- 4. helix
sec("4/8  Helical arcs - Z interpolated along a full circle")
cx, cy, r = 88.0, 55.0, 8.0
travel(cx - r, cy); move("G1", z=1.5, feed=200)
z = 1.5
for _ in range(3):
    z -= 0.3
    arc(False, cx - r, cy, r, 0.0, z=z, feed=500)

# ---------------------------------------------------------------- 5. spiral
sec("5/8  Archimedean spiral - 720 short segments, continuous curvature")
cx, cy, r0, r1, turns, steps = 22.0, 22.0, 2.0, 16.0, 6, 720
travel(cx + r0, cy); plunge()
emit("F1500")
for n in range(1, steps + 1):
    t = n / steps
    a = t * turns * 2 * math.pi
    rr = r0 + (r1 - r0) * t
    move("G1", x=cx + rr*math.cos(a), y=cy + rr*math.sin(a))

# ---------------------------------------------------------------- 6. star
sec("6/8  Star polygon - sharp reversals, worst case for the planner")
cx, cy = 62.0, 22.0
for R, fd in ((15.0, 600), (10.0, 1200), (5.0, 1800)):
    pts = [(cx + R*math.cos(math.radians(90 + k*144)),
            cy + R*math.sin(math.radians(90 + k*144))) for k in range(5)]
    travel(*pts[0]); plunge()
    for p in pts[1:] + [pts[0]]:
        move("G1", x=p[0], y=p[1], feed=fd)

# ---------------------------------------------------------------- 7. planes + G91
sec("7/8  G18 / G19 arc planes and incremental mode")
travel(90, 20); move("G1", z=3.0, feed=200)
emit("G18 (XZ plane)")
emit("G2X94Z3I2K0F400 (semicircle in XZ)")
pos[0], pos[2] = 94.0, 3.0; dist += math.pi*2
emit("G19 (YZ plane)")
emit("G3Y24Z3J2K0F400 (semicircle in YZ)")
pos[1], pos[2] = 24.0, 3.0; dist += math.pi*2
emit("G17 (back to XY)")
emit("G0Z%s" % f(ZS)); pos[2] = ZS
emit("")
emit("G91 (incremental)")
emit("G0X-6Y-6"); pos[0] -= 6; pos[1] -= 6; 
emit("G1Z-4.5F300"); pos[2] = ZW; dist += 4.5
for dx, dy in ((8,0),(0,8),(-8,0),(0,-8)):
    emit("G1X%sY%sF700" % (f(dx), f(dy)))
    pos[0] += dx; pos[1] += dy; dist += math.hypot(dx, dy)
emit("G0Z4.5"); pos[2] = ZS; dist += 4.5
emit("G90 (back to absolute)")

# ---------------------------------------------------------------- 8. raster
sec("8/8  Fine raster - 0.5 mm segments, the long haul")
travel(6, 74); plunge()
emit("F2000")
passes, x_a, x_b, step = 10, 6.0, 94.0, 0.5
n = int((x_b - x_a) / step)
for p in range(passes):
    base = 74.0 + p * 2.2
    xs = [x_a + i*step for i in range(n + 1)]
    if p % 2: xs.reverse()
    for x in xs:
        move("G1", x=x, y=base + 1.0*math.sin((x - x_a) / 6.0))
    if p < passes - 1:
        move("G1", y=base + 2.2)

# ---------------------------------------------------------------- epilogue
sec("Done - park and stop")
retract()
emit("(M9)")
emit("(M5)")
emit("G0X0Y0")
emit("M30 (program end)")

# runtime estimate: path length at a representative 900 mm/min, plus overhead
mins = dist / 900.0
body = "\n".join(L)
body = body.replace("(RUNTIME)",
    "( Path length about %.0f mm; expect roughly %d-%d minutes at the feeds )\n"
    "( below, clamped by your $110-$112 max rates. %d lines. )"
    % (dist, max(1, int(mins*0.8)), int(mins*1.6) + 1, len(L)))
io.open(OUT, "w", encoding="ascii", newline="\r\n").write(body + "\r\n")
print("wrote %s" % OUT)
print("lines: %d   path length: %.0f mm" % (len(L), dist))
