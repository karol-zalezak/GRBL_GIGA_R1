#!/usr/bin/env python3
"""Handwheel (MPG) tests for Grbl-Giga on the Arduino GIGA R1 (src/mpg.h).

No pendant needed: the firmware's test hooks take the same path as the
pendant's signals.

  $TMPG=axis,mult  pulls the selector pins of axis (0 OFF, 1-4 = X Y Z 4)
                   and of the step (1, 10, 100) down, like its contacts
  $TMPW=n[,rate]   turns the wheel n detents (negative: back) at rate
                   detents/s, by switching the pulls of the A/B pins in
                   quadrature - TIM8 counts the real edges

and $I reports mpg=<state>/<axis>/<mult>/<count>/<inputs>/<led>
(state 0 off, 1 no axis, 2 ready, 3 blocked).

  steps      x1 / x10 / x100 move exactly n x step, $96=2 reverses, A in degrees
  off        $96=0, selector at OFF, check mode: the wheel moves nothing
  follow     the axis follows a steady turn and stops with the wheel
  drop       turning faster than $98 is dropped, not stored up
  switch     another axis or OFF stops the running jog at once
  soft       at a soft limit the axis stops at the limit

    python test_mpg.py [--port COM5] [--only steps,follow,...]
"""
import argparse
import re
import time

from grbl_link import Grbl, say

MPG_RE = re.compile(rb" mpg=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)")
AXES = "XYZA"


def check(cond, what):
    cond = bool(cond)
    say(("  ok    " if cond else "  FAIL  ") + what)
    return cond


def mpg(g):
    for l in g.ok("$I"):
        m = MPG_RE.search(l)
        if m:
            return tuple(int(x) for x in m.groups())
    raise RuntimeError("no mpg= in $I")


def pos(g):
    s = g.query()
    return {"X": s[2], "Y": s[3], "Z": s[4], "A": s[6]}


def wait_idle(g, timeout=10.0):
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < timeout:
        s = g.query()
        if s and s[1].startswith("Idle"):
            return s
        time.sleep(0.02)
    raise TimeoutError("not Idle")


def select(g, axis, mult):
    g.ok("$TMPG=%d,%d" % (axis, mult))
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 1.0:
        m = mpg(g)
        if m[1] == axis and (mult == 0 or m[2] == mult):
            return m
        time.sleep(0.02)
    raise TimeoutError("selector not taken: %s" % (mpg(g),))


def turn(g, detents, rate=200, settle=0.4):
    """Turns the wheel and waits until it has stopped and the machine is Idle."""
    g.ok("$TMPW=%d,%d" % (detents, rate))
    time.sleep(abs(detents) / rate + settle)
    return wait_idle(g)


def near(a, b, tol=0.0005):
    return abs(a - b) <= tol


def t_steps(g):
    say("\nsteps x1 / x10 / x100:")
    ok = True
    select(g, 1, 1)
    m = mpg(g)
    ok &= check(m[0] == 2 and m[5] == 1, "X x1 selected: state ready, LED on (mpg=%s)" % (m,))
    p0 = pos(g)["X"]
    turn(g, 100)
    p1 = pos(g)["X"]
    ok &= check(near(p1 - p0, 0.100), "x1, 100 detents: X %+.4f mm" % (p1 - p0))
    select(g, 1, 10)
    turn(g, -50)
    p2 = pos(g)["X"]
    ok &= check(near(p2 - p1, -0.500), "x10, -50 detents: X %+.4f mm" % (p2 - p1))
    select(g, 1, 100)
    turn(g, 20)
    p3 = pos(g)["X"]
    ok &= check(near(p3 - p2, 2.000), "x100, 20 detents: X %+.4f mm" % (p3 - p2))
    g.ok("$96=2")
    select(g, 2, 10)
    y0 = pos(g)["Y"]
    turn(g, 10)
    y1 = pos(g)["Y"]
    ok &= check(near(y1 - y0, -0.100), "$96=2 (reversed), Y x10, +10 detents: Y %+.4f mm" % (y1 - y0))
    g.ok("$96=1")
    select(g, 4, 100)
    a0 = pos(g)["A"]
    turn(g, 15)
    a1 = pos(g)["A"]
    ok &= check(near(a1 - a0, 1.5), "axis 4 = A (rotary), x100, 15 detents: A %+.4f deg" % (a1 - a0))
    g.ok("$97=0.005")
    select(g, 3, 10)
    z0 = pos(g)["Z"]
    turn(g, 7)
    z1 = pos(g)["Z"]
    ok &= check(near(z1 - z0, 0.350), "$97=0.005, Z x10, 7 detents: Z %+.4f mm" % (z1 - z0))
    g.ok("$97=0.001")
    return ok


def t_off(g):
    say("\nwheel without effect:")
    ok = True
    p0 = pos(g)
    g.ok("$96=0")
    select(g, 1, 100)
    m = mpg(g)
    turn(g, 20)
    ok &= check(m[0] == 0 and m[5] == 0 and pos(g) == p0, "$96=0: state %d, LED %d, nothing moves" % (m[0], m[5]))
    g.ok("$96=1")
    select(g, 0, 100)
    m = mpg(g)
    turn(g, 20)
    ok &= check(m[0] == 1 and m[5] == 0 and pos(g) == p0, "selector at OFF: state %d, LED %d, nothing moves" % (m[0], m[5]))
    select(g, 1, 100)
    g.ok("$C")
    m = mpg(g)
    g.ok("$TMPW=20,200")
    time.sleep(0.4)
    s = g.query()
    leds = set()
    for _ in range(6):
        leds.add(mpg(g)[5])
        time.sleep(0.11)
    ok &= check(m[0] == 3 and s[1].startswith("Check") and pos(g) == p0 and leds == {0, 1},
                "check mode: state %d (blocked), LED blinks %s, nothing moves" % (m[0], sorted(leds)))
    g.write(b"$C\n")
    time.sleep(0.5)
    g.drain(0.3)
    m = mpg(g)
    ok &= check(m[0] == 2, "out of check mode: ready again (state %d)" % m[0])
    return ok


def t_follow(g):
    say("\nfollowing a steady turn:")
    ok = True
    select(g, 1, 100)
    p0 = pos(g)["X"]
    detents, rate = 200, 100           # 20 mm at 10 mm/s = 600 mm/min
    g.ok("$TMPW=%d,%d" % (detents, rate))
    t0 = time.perf_counter()
    samples = []
    while time.perf_counter() - t0 < detents / rate + 1.0:
        s = g.query()
        samples.append((time.perf_counter() - t0, s[2], s[1]))
        if s[1].startswith("Idle") and time.perf_counter() - t0 > 0.5:
            break
        time.sleep(0.02)
    t_end = detents / rate
    mid = [(t, x) for t, x, _ in samples if 0.5 < t < t_end - 0.3]
    speed = (mid[-1][1] - mid[0][1]) / (mid[-1][0] - mid[0][0]) if len(mid) > 2 else 0
    idle_at = next((t for t, x, st in samples if t > 0.5 and st.startswith("Idle")), None)
    p1 = pos(g)["X"]
    ok &= check(abs(speed - 10.0) < 0.6, "axis speed while turning: %.2f mm/s (wheel 10.0)" % speed)
    ok &= check(near(p1 - p0, 20.0), "whole turn arrives: X %+.4f mm" % (p1 - p0))
    ok &= check(idle_at is not None and idle_at - t_end < 0.15,
                "Idle %.0f ms after the wheel stopped" % (((idle_at or 99) - t_end) * 1000))
    return ok


def t_drop(g):
    say("\nturning faster than $98:")
    ok = True
    g.ok("$98=1200")                   # 20 mm/s
    select(g, 1, 100)
    p0 = pos(g)["X"]
    detents, rate = 200, 1000          # 20 mm in 0.2 s = 100 mm/s
    g.ok("$TMPW=%d,%d" % (detents, rate))
    t0 = time.perf_counter()
    idle_at = None
    while time.perf_counter() - t0 < 3.0:
        s = g.query()
        if s[1].startswith("Idle") and time.perf_counter() - t0 > 0.1:
            idle_at = time.perf_counter() - t0
            break
        time.sleep(0.02)
    p1 = pos(g)["X"]
    t_end = detents / rate
    ok &= check(p1 - p0 < 10.0, "only what the axis could follow: X %+.3f mm of 20 turned" % (p1 - p0))
    ok &= check(idle_at is not None and idle_at - t_end < 0.25,
                "Idle %.0f ms after the wheel stopped (no backlog)" % (((idle_at or 99) - t_end) * 1000))
    g.ok("$98=3000")
    return ok


def t_switch(g):
    say("\nselector changed while moving:")
    ok = True
    select(g, 1, 100)
    x0 = pos(g)["X"]
    g.ok("$TMPW=300,100")              # 3 s of 10 mm/s
    time.sleep(1.0)
    g.ok("$TMPG=2,100")                # to Y
    time.sleep(0.15)
    xa = pos(g)["X"]
    time.sleep(0.3)
    xb = pos(g)["X"]
    ok &= check(xa == xb and 6 < xa - x0 < 12, "X stopped at once at %+.3f mm, stays there" % (xa - x0))
    y0 = pos(g)["Y"]
    time.sleep(0.5)
    y1 = pos(g)["Y"]
    ok &= check(y1 - y0 > 3.0, "the rest of the turn moves Y: %+.3f mm in 0.5 s" % (y1 - y0))
    g.ok("$TMPG=0,100")                # to OFF
    time.sleep(0.15)
    ya = pos(g)["Y"]
    time.sleep(0.3)
    yb = pos(g)["Y"]
    ok &= check(ya == yb, "OFF: Y stopped at once")
    time.sleep(1.5)
    wait_idle(g)
    return ok


def t_soft(g):
    say("\nsoft limits:")
    ok = True
    for n, v in [(22, 1), (23, 0), (24, 200), (25, 1000), (26, 50), (27, 1), (43, 0), (130, 10), (131, 10), (132, 10)]:
        g.ok("$%d=%s" % (n, v))
    for ch, ax in [(2, "X"), (4, "Y"), (6, "Z")]:
        g.ok("$TSW=%d,%s+5" % (ch, ax))
    g.reset()
    r = g.cmd("$H", timeout=60)
    g.ok("$TSW=0")
    g.ok("$20=1")
    ok &= check(r[-1] == b"ok", "$H on virtual switches, then $20=1: %s" % r[-1].decode())
    select(g, 1, 100)
    turn(g, 50)                        # +5 mm from -1: beyond 0
    x1 = pos(g)["X"]
    ok &= check(-0.01 < x1 <= 0.0, "towards the plus end: stops at the limit, X = %.4f" % x1)
    turn(g, -150)                      # -15 mm: beyond -10
    x2 = pos(g)["X"]
    ok &= check(-10.0 <= x2 < -9.99, "towards the minus end: stops at the limit, X = %.4f" % x2)
    for n, v in [(20, 0), (22, 0), (130, 100000), (131, 100000), (132, 100000)]:
        g.ok("$%d=%s" % (n, v))
    g.reset()
    g.ok("$X")
    return ok


TESTS = [("steps", t_steps), ("off", t_off), ("follow", t_follow), ("drop", t_drop),
         ("switch", t_switch), ("soft", t_soft)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--only", help="comma separated: " + ",".join(n for n, _ in TESTS))
    a = ap.parse_args()
    only = set(a.only.split(",")) if a.only else None
    g = Grbl(a.port, quiet=True)
    say("port:", g.port)
    results = []
    try:
        # 1000 steps/mm: the x1 step (1 um) is one motor step.
        g.setup_bench(steps_per_mm=1000, max_rate=6000, accel=1000, pulse_us=3)
        g.ok("$96=1")
        g.ok("$97=0.001")
        g.ok("$98=3000")
        g.ok("$42=0")
        for name, fn in TESTS:
            if only and name not in only:
                continue
            try:
                results.append((name, fn(g)))
            except Exception as e:
                say("  FAIL  %s: %r" % (name, e))
                results.append((name, False))
                g.reset()
                g.ok("$TMPG=0,0")
                g.cmd("$X")
    finally:
        try:
            g.ok("$TMPG=0,0")
            g.ok("$TSW=0")
            g.ok("$96=0")
            g.reset()
        except Exception:
            pass
        g.close()
    say("\nsummary: " + ", ".join("%s %s" % (n, "ok" if r else "FAIL") for n, r in results))


if __name__ == "__main__":
    main()
