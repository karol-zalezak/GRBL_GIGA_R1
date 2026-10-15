#!/usr/bin/env python3
"""Step rate tests for Grbl-Giga on the Arduino GIGA R1.

No motors needed: the machine position reported in `?` status frames is the
stepper ISR's own step counter, so sampling it over time measures the step
rate the ISR actually produced. Each row commands a single-axis (or
three-axis) move at a target step rate and fits a line through the position
samples taken during the cruise part of the move.

  accuracy   100 .. 16000 steps/s at 100 steps/mm (all four AMASS levels, and
             below 3.7 kHz the HAL's stretched 16-bit timer periods)
  ceiling    10k .. 300k steps/s, one axis and all four axes together, to find
             where the ISR saturates and to show the MINIMUM_STEP_CYCLES clamp
  pulses     pulse-termination interrupts == stepper interrupts
  scan       input scan coverage while stepping at full rate

    python test_steprate.py [--port COM4] [--quick]
"""
import argparse
import re
import time

from grbl_link import Grbl, say

DIAG_RE = re.compile(rb"\[MSG:\S+ up=(\d+) scan=(\d+) step=(\d+) pulse=(\d+) merged=(\d+) floor=(\d+)(?: spur=\d+)? lat=(\d+)/(\d+) tx=(\d+) drop=(\d+)")
SCAN_HZ = 10000


def diag(g):
    for l in g.ok("$I"):
        m = DIAG_RE.match(l)
        if m:
            return tuple(int(x) for x in m.groups())
    raise RuntimeError("no diag line in $I")


def fit_slope(samples):
    """Least squares slope of x over t for a list of (t, x)."""
    n = len(samples)
    mt = sum(t for t, _ in samples) / n
    mx = sum(x for _, x in samples) / n
    sxy = sum((t - mt) * (x - mx) for t, x in samples)
    sxx = sum((t - mt) ** 2 for t, _ in samples)
    return sxy / sxx if sxx else 0.0


def measure_move(g, cmd, axis=0, settle=0.3, poll=0.04):
    """Runs one move and returns steps/s measured over its cruise portion."""
    g.drain(0.05)
    g.write(cmd.encode() + b"\n")
    # wait for ok, then sample until Idle
    t0 = time.monotonic()
    while True:
        l = g.readline(timeout=2.0)
        if l is None:
            raise TimeoutError("no ok for %s" % cmd)
        if l == b"ok":
            break
        if l.startswith(b"error") or l.startswith(b"ALARM"):
            raise RuntimeError("%s -> %s" % (cmd, l))
    samples = []
    t_start = None
    while True:
        s = g.query(timeout=1.0)
        if s is None:
            continue
        t, state, x, y, z = s[0], s[1], s[2], s[3], s[4]
        pos = (x, y, z)[axis]
        if state.startswith("Run"):
            if t_start is None:
                t_start = t
            samples.append((t, pos))
        elif state.startswith("Idle") and t_start is not None:
            break
        elif state.startswith("Alarm"):
            raise RuntimeError("alarm during %s" % cmd)
        elif t_start is None and time.monotonic() - t0 > 5.0:
            raise TimeoutError("move never started: %s" % cmd)
        time.sleep(poll)
    if len(samples) < 6:
        return None, len(samples), 0.0
    # cruise: drop the first and last `settle` seconds
    t_a, t_b = samples[0][0] + settle, samples[-1][0] - settle
    cruise = [s for s in samples if t_a <= s[0] <= t_b]
    if len(cruise) < 4:
        cruise = samples[len(samples) // 4: -len(samples) // 4 or None]
    return fit_slope(cruise), len(cruise), samples[-1][0] - samples[0][0]


def run_rate(g, rate, spm, axes=1, cruise_s=3.0, accel=3000):
    """Commands `rate` steps/s on one or three axes and returns measured steps/s."""
    mm_s = rate / spm
    feed = mm_s * 60.0
    if axes > 1:
        feed *= axes ** 0.5     # vector feed so that each axis runs at mm_s
    dist = mm_s * (cruise_s + 2 * 0.6) + mm_s * mm_s / accel
    # Alternate direction so the virtual machine stays near its origin.
    run_rate.sign = -getattr(run_rate, "sign", -1)
    dist *= run_rate.sign
    if axes == 4:
        cmd = "G1 X%.3f Y%.3f Z%.3f A%.3f F%.1f" % (dist, dist, dist, dist, feed)
    elif axes == 3:
        cmd = "G1 X%.3f Y%.3f Z%.3f F%.1f" % (dist, dist, dist, feed)
    else:
        cmd = "G1 X%.3f F%.1f" % (dist, feed)
    slope, n, dur = measure_move(g, cmd)
    g.ok("G92 X0 Y0 Z0 A0")
    if slope is None:
        return None, n, dur
    return abs(slope) * spm, n, dur


def t_accuracy(g, quick):
    spm = 100
    g.setup_bench(steps_per_mm=spm, max_rate=60000, accel=3000)
    rates = [100, 250, 500, 1000, 2000, 4000, 8000, 16000] if not quick else [100, 500, 2000, 8000]
    say("\naccuracy (single axis, %d steps/mm):" % spm)
    say("  %10s %10s %6s %8s" % ("commanded", "measured", "ratio", "samples"))
    for r in rates:
        m, n, dur = run_rate(g, r, spm, cruise_s=2.0 if r < 1000 else 3.0)
        say("  %10d %10.0f %6.3f %8d" % (r, m or 0, (m or 0) / r, n))
        if m is None or abs(m / r - 1) > 0.03:
            say("  ** off by more than 3%%")
    # 400 steps/mm variant, as in the R4 verification. Changing steps/mm
    # away from zero leaves Grbl's mm positions stale until a reset (see
    # grbl_link.setup_bench), so re-run the bench setup rather than just $100.
    g.setup_bench(steps_per_mm=400, max_rate=60000, accel=3000)
    m, n, _ = run_rate(g, 4000, 400)
    say("  %10s %10.0f %6.3f %8d   (400 steps/mm)" % ("4000", m or 0, (m or 0) / 4000, n))


def t_ceiling(g, quick):
    spm = 250
    g.setup_bench(steps_per_mm=spm, max_rate=100000, accel=5000, pulse_us=3)
    rates = [10000, 25000, 50000, 100000, 150000, 175000, 200000, 250000, 300000]
    if quick:
        rates = [25000, 100000, 200000, 300000]
    say("\nceiling (%d steps/mm, 3 us pulse):" % spm)
    say("  %10s %12s %12s" % ("commanded", "1 axis", "4 axes"))
    for r in rates:
        d0 = diag(g)
        m1, n1, _ = run_rate(g, r, spm, axes=1, cruise_s=2.5)
        d1 = diag(g)
        m3, n3, _ = run_rate(g, r, spm, axes=4, cruise_s=2.5)
        d3 = diag(g)
        note = ""
        if m1 and m1 < r * 0.97:
            note += " 1-axis short by %.0f%%" % (100 * (1 - m1 / r))
        if m3 and m3 < r * 0.97:
            note += " 4-axis short by %.0f%%" % (100 * (1 - m3 / r))
        pulses1 = (d1[3] - d0[3], d1[2] - d0[2])
        pulses3 = (d3[3] - d1[3], d3[2] - d1[2])
        say("  %10d %12.0f %12.0f  pulses/ticks 1ax %d/%d 4ax %d/%d merged %d floor %d lat %dus%s"
            % (r, m1 or 0, m3 or 0, pulses1[0], pulses1[1], pulses3[0], pulses3[1],
               d3[4] - d0[4], d3[5] - d0[5], d3[6], note))


def t_scan(g, quick):
    spm = 250
    g.setup_bench(steps_per_mm=spm, max_rate=100000, accel=5000, pulse_us=3)
    say("\ninput scan coverage while stepping (4 axes):")
    for r in ([50000, 150000] if quick else [50000, 100000, 150000, 200000]):
        d0 = diag(g)
        t0 = time.monotonic()
        m3, _, dur = run_rate(g, r, spm, axes=4, cruise_s=3.0)
        d1 = diag(g)
        wall = (time.monotonic() - t0) * 1000.0
        up = d1[0] - d0[0]
        scans = d1[1] - d0[1]
        expected = up * SCAN_HZ / 1000.0   # up= is the M7's own clock (cycle counter)
        say("  %6d steps/s x4 (measured %6.0f): %d scans in %d ms (PC %d ms) = %.1f%% of expected"
            % (r, m3 or 0, scans, up, wall, 100.0 * scans / expected if expected else 0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--only", choices=["accuracy", "ceiling", "scan"])
    a = ap.parse_args()
    g = Grbl(a.port)
    say("port:", g.port)
    try:
        g.reset()
        if a.only in (None, "accuracy"):
            t_accuracy(g, a.quick)
        if a.only in (None, "ceiling"):
            t_ceiling(g, a.quick)
        if a.only in (None, "scan"):
            t_scan(g, a.quick)
        say("\nfinal diag:", " ".join(str(x) for x in diag(g)))
    finally:
        try:
            g.setup_bench()
        except Exception:
            pass
        g.close()


if __name__ == "__main__":
    main()
