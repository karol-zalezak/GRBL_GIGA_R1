#!/usr/bin/env python3
"""CoreXY kinematics ($40/$41) tests for Grbl-Giga on the Arduino GIGA R1.

No machine needed. The `motor=` counters in $I count the pulses each driver
actually gets, signed by its direction line, so they show the belt motors'
view of a move, while `?` shows the tool head's (Cartesian) one.

  motors     X move -> both belt motors the same way; Y move -> opposite ways;
             a 45-degree diagonal moves one motor only (by twice the distance)
  closure    a full circle and a jog return the motors to where they started
  feed       F is the speed along the programmed path, as on a Cartesian
             machine (upstream Grbl makes it the motor speed instead)
  limits     $41=0: $110/$120 limit each motor, so a diagonal runs at
             1/sqrt(2) of a Cartesian one; $41=1: they limit the head axes
  exclusive  $40=1 and a slave A ($42=2/3) refuse each other

    python test_corexy.py [--port COM5]
"""
import argparse
import math
import re
import time

from grbl_link import Grbl, say

DIAG_RE = re.compile(rb"\[MSG:\S+ .*motor=(-?\d+),(-?\d+),(-?\d+),(-?\d+) in=")
SPM = 100          # steps/mm on every axis


def motors(g):
    for l in g.ok("$I"):
        m = DIAG_RE.match(l)
        if m:
            return [int(x) for x in m.groups()]
    raise RuntimeError("no diag line in $I")


def sync(g):
    g.ok("G4 P0", timeout=60)


def pos(g):
    sync(g)
    s = g.wait_idle()
    return s[2], s[3], s[4], s[6]


def near(a, b, tol=0.002):
    return abs(a - b) <= tol


def check(cond, what):
    cond = bool(cond)
    say(("  ok    " if cond else "  FAIL  ") + what)
    return cond


def moved(g, line):
    """Runs one line and returns how many pulses each motor got."""
    sync(g)
    m0 = motors(g)
    g.ok(line)
    if line.startswith("$J"):
        g.wait_idle()          # G-code (G4 P0) is locked while jogging
    else:
        sync(g)
    m1 = motors(g)
    return [b - a for a, b in zip(m0, m1)]


def timed(g, line):
    """Seconds from the first Run status to Idle again, polling ? every 10 ms."""
    sync(g)
    g.write(line.encode() + b"\n")
    t_run = None
    t0 = time.monotonic()
    while time.monotonic() - t0 < 30:
        s = g.query(timeout=0.5)
        if s:
            if s[1].startswith("Run") and t_run is None:
                t_run = s[0]
            elif s[1].startswith("Idle") and t_run is not None:
                g.drain(0.1)
                return s[0] - t_run
        time.sleep(0.01)
    raise TimeoutError("move did not finish: %s" % line)


def trapezoid(d, v, a):
    """Duration of a d-mm move at top speed v (mm/s) and acceleration a (mm/s^2)."""
    if d >= v * v / a:
        return d / v + v / a
    return 2 * math.sqrt(d / a)


def t_motors(g):
    say("\nbelt motors ($40=1):")
    ok = True
    d = moved(g, "G1 X10 F3000")
    ok &= check(d == [1000, 1000, 0, 0], "G1 X10: motors A,B = +1000,+1000 %s" % d[:2])
    d = moved(g, "G1 Y10 F3000")
    ok &= check(d == [1000, -1000, 0, 0], "G1 Y10: motors A,B = +1000,-1000 %s" % d[:2])
    d = moved(g, "G1 X0 Y0 F3000")
    ok &= check(d == [-2000, 0, 0, 0], "diagonal back to 0,0: only A, -2000 %s" % d[:2])
    d = moved(g, "G1 X10 Y-10 F3000")
    ok &= check(d == [0, 2000, 0, 0], "diagonal X10 Y-10: only B, +2000 %s" % d[:2])
    x, y, z, a = pos(g)
    ok &= check(near(x, 10) and near(y, -10), "status reports the head at X10 Y-10 (%.3f, %.3f)" % (x, y))
    d = moved(g, "G1 Z5 A5 F3000")
    ok &= check(d == [0, 0, 500, 500], "Z and A stay independent: %s" % d)
    g.ok("G0 X0 Y0 Z0 A0")
    sync(g)
    return ok


def t_closure(g):
    say("\nclosure:")
    ok = True
    m0 = motors(g)
    g.ok("G2 X0 Y0 I7.5 J2.5 F3000")
    g.ok("G3 X0 Y0 I-4 J-6 F3000")
    x, y, z, a = pos(g)
    m1 = motors(g)
    ok &= check(m1 == m0 and near(x, 0) and near(y, 0),
                "two full circles end where they began (motors %s, head %.3f,%.3f)" % (
                    [b - a for a, b in zip(m0, m1)], x, y))
    d = moved(g, "$J=G91 X5 Y5 F3000")
    ok &= check(d[:2] == [1000, 0], "jog X5 Y5 (diagonal): A +1000, B 0 %s" % d[:2])
    d = moved(g, "$J=G91 X-5 Y-5 F3000")
    ok &= check(d[:2] == [-1000, 0], "jog back: A -1000 %s" % d[:2])
    return ok


def t_feed(g):
    say("\nfeed rate = path speed:")
    ok = True
    # 60 mm at F3600 (60 mm/s), accel 1000 mm/s^2 on each motor.
    g.ok("$41=0")
    tx = timed(g, "G1 X60 F3600")
    g.ok("G0 X0")
    td = timed(g, "G1 X42.426 Y42.426 F3600")   # 60 mm along a 45-degree line
    g.ok("G0 X0 Y0")
    sync(g)
    ex = trapezoid(60, 60, 1000)
    # On the diagonal motor A runs at 60*sqrt(2) mm/s and accelerates at
    # a*sqrt(2), within its 100 mm/s and 1000 mm/s^2 only after scaling: the
    # head acceleration becomes 1000/sqrt(2).
    ed = trapezoid(60, 60, 1000 / math.sqrt(2))
    ok &= check(abs(tx - ex) < 0.05, "G1 X60 F3600: %.3f s (expected %.3f)" % (tx, ex))
    # Upstream plans the motor-space path, sqrt(2) longer, at F: 84.85 mm at 60 mm/s.
    ux = trapezoid(60 * math.sqrt(2), 60, 1000 * math.sqrt(2))
    ud = trapezoid(60 * math.sqrt(2), 60, 1000)
    ok &= check(abs(td - ed) < 0.05, "60 mm diagonal F3600: %.3f s (expected %.3f)" % (td, ed))
    say("        (upstream CoreXY would take %.3f s and %.3f s)" % (ux, ud))
    return ok


def t_limits(g):
    say("\n$41 rate limits (G0, $110=6000 -> 100 mm/s, $120=1000):")
    ok = True
    d = 141.421                       # G0 X100 Y100
    g.ok("$41=0")
    t0 = timed(g, "G0 X100 Y100")
    g.ok("G0 X0 Y0")
    sync(g)
    g.ok("$41=1")
    t1 = timed(g, "G0 X100 Y100")
    g.ok("G0 X0 Y0")
    sync(g)
    g.ok("$41=0")
    # $41=0: motor A carries the whole diagonal at sqrt(2) times the path speed.
    e0 = trapezoid(d, 100 / math.sqrt(2), 1000 / math.sqrt(2))
    # $41=1: each head axis may run at 100 mm/s, the path at 141 mm/s.
    e1 = trapezoid(d, 100 * math.sqrt(2), 1000 * math.sqrt(2))
    ok &= check(abs(t0 - e0) < 0.06, "$41=0: %.3f s (expected %.3f)" % (t0, e0))
    ok &= check(abs(t1 - e1) < 0.06, "$41=1: %.3f s (expected %.3f)" % (t1, e1))
    return ok


def t_exclusive(g):
    say("\n$40 and a slave A exclude each other:")
    ok = True
    out = g.cmd("$42=2")
    ok &= check(out[-1] == b"error:5", "$40=1 then $42=2: %s" % out[-1].decode())
    g.ok("$40=0")
    g.ok("$42=2")
    out = g.cmd("$40=1")
    ok &= check(out[-1] == b"error:5", "$42=2 then $40=1: %s" % out[-1].decode())
    g.ok("$42=1")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    a = ap.parse_args()
    g = Grbl(a.port)
    say("port:", g.port)
    results = []
    try:
        g.setup_bench(steps_per_mm=SPM, max_rate=6000, accel=1000)
        # Work = machine coordinates, so positions can be compared directly.
        g.ok("G92.1")
        g.ok("G10 L2 P1 X0 Y0 Z0 A0")
        g.ok("G53 G0 X0 Y0 Z0 A0")
        sync(g)
        g.ok("$40=1")
        results.append(("motors", t_motors(g)))
        results.append(("closure", t_closure(g)))
        results.append(("feed", t_feed(g)))
        results.append(("limits", t_limits(g)))
        results.append(("exclusive", t_exclusive(g)))
    finally:
        try:
            sync(g)
            g.ok("$40=0")
            g.ok("$41=0")
            g.ok("$42=0")
        except Exception as e:
            say("cleanup failed:", e)
        g.close()
    say("\nsummary: " + ", ".join("%s %s" % (n, "ok" if r else "FAIL") for n, r in results))


if __name__ == "__main__":
    main()
