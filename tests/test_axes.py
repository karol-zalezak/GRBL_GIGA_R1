#!/usr/bin/env python3
"""Fourth axis, outputs and driver alarm tests for Grbl-Giga on the Arduino GIGA R1.

No machine needed: positions come from the stepper ISR's step counters, the
pulses each driver gets (slave ones included, signed by direction) from the
`motor=` counters in $I, and the outputs and inputs from the `out=`/`in=`
masks in $I (channels / functions).

  rotary     $42=0: A in degrees, unaffected by G20, moves with arcs
  linear     $42=1: A in mm, scaled by G20
  slave      $42=2/3: A words refused, the A driver repeats X (or Y) pulses
  outputs    M3/M4/M5 (FWD/REV), M7/M8/M9, M62-M65 P0-P5, $46, $80-$93
  alarm      a servo alarm input ($68, IN9 via $TIN) stops Grbl with ALARM:17,
             and the next reset pulses the servo reset output (OUT10)

    python test_axes.py [--port COM5]
"""
import argparse
import re
import time

from grbl_link import Grbl, say

DIAG_RE = re.compile(rb"\[MSG:\S+ .*motor=(-?\d+),(-?\d+),(-?\d+),(-?\d+) in=(\d+)/(\d+) out=(\d+)/(\d+)")
MX, MY, MZ, MA, IN, INFN, OUT, OUTFN = range(8)


def diag(g):
    """(motor X, Y, Z, A pulse counts, in mask, out mask) from $I."""
    for l in g.ok("$I"):
        m = DIAG_RE.match(l)
        if m:
            return tuple(int(x) for x in m.groups())
    raise RuntimeError("no diag line in $I")


def sync(g):
    """G4 P0 is acknowledged only once every queued motion has finished."""
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


def t_rotary(g):
    say("\nrotary A ($42=0):")
    g.setup_bench(steps_per_mm=100, max_rate=20000, accel=2000)
    # pos() reads machine positions: no G92 offset, or a test before this one
    # that left the machine off zero (under go_machine_zero's 50 mm) shifts
    # every target below by that much.
    g.ok("G92.1")
    g.ok("$42=0")
    g.ok("G90 G21 G0 A90")
    ok = check(near(pos(g)[3], 90.0), "G0 A90 -> A=90")
    g.ok("G20 G1 A45 F2000")
    ok &= check(near(pos(g)[3], 45.0), "G20 G1 A45 -> A=45 (degrees, not scaled)")
    g.ok("G21 G0 A0 X0 Y0")
    # A rides along an arc like the helical axis: a full circle with A 0 -> 180.
    g.ok("G2 X0 Y0 I5 J0 A180 F3000")
    x, y, z, a = pos(g)
    ok &= check(near(x, 0) and near(y, 0) and near(a, 180.0), "G2 full circle with A0->180 ends at A=180 (%.3f)" % a)
    g.ok("G0 A0")
    return ok


def t_linear(g):
    say("\nlinear A ($42=1):")
    sync(g)
    g.ok("$42=1")
    g.ok("G90 G20 G1 A1 F100")
    ok = check(near(pos(g)[3], 25.4), "G20 G1 A1 -> A=25.4 mm")
    g.ok("G21 G0 A0")
    sync(g)
    return ok


def t_slave(g):
    ok = True
    for mode, axis, word in ((2, "X", "X"), (3, "Y", "Y")):
        say("\nslave A ($42=%d, second %s motor):" % (mode, axis))
        g.ok("G0 X0 Y0 Z0 A0")
        sync(g)
        g.ok("$42=%d" % mode)
        out = g.cmd("G1 A10 F1000")
        ok &= check(out[-1] == b"error:20", "A word refused with error:20 (%s)" % out[-1].decode())
        d0 = diag(g)
        g.ok("G1 %s10 F3000" % word)
        x, y, z, a = pos(g)
        d1 = diag(g)
        moved = x if axis == "X" else y
        pulses = d1[MA] - d0[MA]
        ok &= check(near(moved, 10.0) and near(a, 0.0), "%s moved 10, A reports 0" % axis)
        ok &= check(pulses == 1000, "A driver pulses = %s steps: %d (expected 1000)" % (axis, pulses))
        d0 = diag(g)
        g.ok("G1 %s0 Z5 F3000" % word)
        sync(g)
        d1 = diag(g)
        ok &= check(d1[MA] - d0[MA] == -1000, "only %s pulses follow, not Z, and in its direction: %d"
                    % (axis, d1[MA] - d0[MA]))
        g.ok("G0 Z0")
        sync(g)
        g.ok("$42=1")
    return ok


def t_outputs(g):
    say("\noutputs:")
    ok = True
    g.ok("$46=1")
    g.ok("$93=0")
    def out():
        return diag(g)[OUT]
    def outfn():
        return diag(g)[OUTFN]
    ok &= check(out() == 0, "all off after start")
    g.ok("M3 S500")
    ok &= check(out() == 0b00010001, "M3: OUT1 FWD + OUT5 (AUX0 follows spindle): %s" % bin(out()))
    g.ok("M4 S500")
    ok &= check(out() == 0b00010010, "M4: OUT2 REV only (FWD off) + OUT5: %s" % bin(out()))
    g.ok("M5")
    ok &= check(out() == 0, "M5: all off: %s" % bin(out()))
    g.ok("M8")
    g.ok("M7")
    ok &= check(out() == 0b00001100, "M7 M8: OUT3 + OUT4: %s" % bin(out()))
    g.ok("M9")
    g.ok("M64 P1")
    g.ok("M64 P3")
    ok &= check(out() == 0b10100000, "M64 P1, M64 P3: OUT6 + OUT8: %s" % bin(out()))
    g.ok("M65 P1")
    g.ok("G1 X5 F600")
    g.ok("M62 P2")          # waits for the X move, then switches
    ok &= check(out() == 0b11000000, "M65 P1, M62 P2 after motion: OUT7 + OUT8: %s" % bin(out()))
    g.ok("M64 P4")
    ok &= check(out() == 0b100011000000, "M64 P4: AUX4 on OUT12: %s" % bin(out()))
    aux5 = int(g.settings().get(89, 0))
    g.ok("$89=0")           # AUX5 without a channel (shield v4's default is OUT14)
    g.ok("M64 P5")
    ok &= check((outfn() >> 9) & 1 and out() == 0b100011000000,
                "M64 P5: AUX5 on, no channel assigned: fn=%s ch=%s" % (bin(outfn()), bin(out())))
    out_bad = g.cmd("M64 P6")
    ok &= check(out_bad[-1].startswith(b"error:"), "M64 P6 (no such output) refused: %s" % out_bad[-1].decode())
    g.ok("$93=4095")
    ok &= check(out() == 0b100011000000, "$93=4095 inverts pins, logical state unchanged: %s" % bin(out()))
    g.ok("$93=0")
    # Re-assignment: one output, one function.
    r = g.cmd("$84=11")
    ok &= check(r[-1] == b"error:42", "$84=11 (OUT11 has the VFD reset) refused: %s" % r[-1].decode())
    g.ok("M2")
    sync(g)
    ok &= check(out() == 0, "M2 switches everything off: %s" % bin(out()))
    g.ok("$92=0")
    g.ok("$84=11")
    g.ok("M3 S100")
    ok &= check(out() == 0b10000000001, "AUX0 moved to OUT11, M3: OUT1 + OUT11: %s" % bin(out()))
    g.ok("M5")
    g.ok("$84=5")
    g.ok("$92=11")
    g.ok("$89=%d" % aux5)
    g.ok("G0 X0")
    sync(g)
    return ok


def t_alarm(g):
    say("\nservo alarm input:")
    ok = True
    # X's servo alarm on whatever input $68 puts it (shield v4: IN17).
    ch = int(g.settings().get(68, 0)) or 17
    g.ok("$68=%d" % ch)
    g.ok("$55=3000")        # a long reset pulse, so it outlasts the banner wait
    s = g.query()
    ok &= check(s and s[1].startswith("Idle"), "IN%d (servo alarm X) idle: stays Idle" % ch)
    g.write(b"$TIN=%d,1\n" % ch)  # pull it low: reads like the optocoupler conducting
    seen = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 2.0:
        l = g.readline(timeout=0.5)
        if l:
            seen.append(l)
            if l.startswith(b"ALARM:"):
                break
    ok &= check(b"ALARM:17" in seen, "IN%d active -> ALARM:17 (%s)" % (ch, [x.decode() for x in seen]))
    s = g.query()
    ok &= check(s and "M" in s[5], "Pn shows M: %s" % (s[5] if s else None))
    g.reset()               # a critical alarm takes no lines until the reset
    g.ok("$TIN=%d,0" % ch)
    time.sleep(0.05)        # servo alarm debouncing: 2 ms
    d = diag(g)
    ok &= check(d[OUT] & (1 << 9), "after the reset: servo reset pulse on OUT10: %s" % bin(d[OUT]))
    time.sleep(3.0)
    d = diag(g)
    ok &= check(not d[OUT] & (1 << 9), "pulse over after $55 = 3 s: %s" % bin(d[OUT]))
    g.ok("$55=500")
    g.ok("$X")
    s = g.query()
    ok &= check(s and s[1].startswith("Idle"), "after the input released and $X: %s" % (s[1] if s else None))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    a = ap.parse_args()
    g = Grbl(a.port)
    say("port:", g.port)
    results = []
    try:
        results.append(("rotary", t_rotary(g)))
        results.append(("linear", t_linear(g)))
        results.append(("slave", t_slave(g)))
        results.append(("outputs", t_outputs(g)))
        results.append(("alarm", t_alarm(g)))
    finally:
        try:
            g.ok("$42=0")
        except Exception:
            pass
        g.close()
    say("\nsummary: " + ", ".join("%s %s" % (n, "ok" if r else "FAIL") for n, r in results))


if __name__ == "__main__":
    main()
