#!/usr/bin/env python3
"""Streaming tests for Grbl-Giga on the Arduino GIGA R1: g-code under load.

Streams short segments with the character-counting protocol while polling
status every 50 ms, exactly as a sender does, and checks that every line is
acknowledged, no frame is malformed, and the final position is exact.

  ramp      segment length / feed combinations from light to heavy, so the
            table shows where the link (or the parser) runs out
  sustained a fixed load for a while, for lockups and counter drift
  hold      feed hold and resume in the middle of a stream

    python test_stream.py [--port COM4] [--sustained 120] [--quick]
"""
import argparse
import time

from grbl_link import Grbl, say
from test_comm import stream
from test_steprate import diag


def raster(seg, count, feed, width=10.0):
    """Back-and-forth raster: `count` segments of `seg` mm along X, stepping
    down in Y every `width` mm. Returns (lines, final_x, final_y)."""
    lines = []
    x = y = 0.0
    dx = seg
    for i in range(count):
        if (dx > 0 and x + dx > width + 1e-9) or (dx < 0 and x + dx < -1e-9):
            dx = -dx
            y += seg
            lines.append("G1 Y%.3f F%d" % (y, feed))
            continue
        x += dx
        lines.append("G1 X%.3f F%d" % (x, feed))
    return [l.encode() for l in lines], x, y


def start_position(g):
    s = g.wait_idle()
    return s[2], s[3]


def check_position(g, start, x, y, spm=250):
    """Compares the machine position travelled since `start` (MPos is never
    reset by G92, so the raster is checked as a displacement). Exact means
    within the half-step rounding of the target."""
    s = g.wait_idle()
    dx, dy = s[2] - start[0], s[3] - start[1]
    tol = 0.6 / spm
    ok = abs(dx - x) < tol and abs(dy - y) < tol
    return ok, (round(dx, 3), round(dy, 3))


def t_ramp(g, quick):
    spm = 250
    g.setup_bench(steps_per_mm=spm, max_rate=30000, accel=2000)
    rows = [
        # seg mm, feed mm/min
        (0.50, 2400), (0.50, 4800), (0.50, 9600), (0.25, 9600), (0.20, 12000), (0.10, 12000), (0.05, 12000),
    ]
    if quick:
        rows = [(0.50, 4800), (0.20, 12000), (0.05, 12000)]
    say("\nramp (50 ms status polling, %d steps/mm):" % spm)
    say("  %6s %6s %8s %8s %11s %6s %6s %8s %6s" % ("seg", "feed", "steps/s", "lines/s", "ok/expected", "errors", "bad", "final", "status"))
    for seg, feed in rows:
        count = 300 if not quick else 150
        lines, fx, fy = raster(seg, count, feed)
        g.ok("G92 X0 Y0 Z0 A0")
        start = start_position(g)
        d0 = diag(g)
        sent, oks, errs, bad, status, dt = stream(g, lines, window=127, poll_ms=50)
        posok, pos = check_position(g, start, fx, fy)
        d1 = diag(g)
        say("  %6.2f %6d %8.0f %8.0f %11s %6d %6d %8s %6d   steps %d pulses %d merged %d drop=%d" % (
            seg, feed, feed / 60.0 * spm, sent / dt, "%d/%d" % (oks, sent), errs, bad,
            "exact" if posok else "OFF %r" % (pos,), status, d1[2] - d0[2], d1[3] - d0[3], d1[4] - d0[4], d1[9]))
        assert oks == sent and errs == 0 and bad == 0 and posok, "ramp row failed"


def t_sustained(g, seconds):
    spm = 250
    g.setup_bench(steps_per_mm=spm, max_rate=30000, accel=2000)
    say("\nsustained: 0.15 mm segments at F5400 (%d steps/s) for %d s with 50 ms polling" % (5400 / 60 * spm, seconds))
    lines, fx, fy = raster(0.15, 10 ** 7, 5400, width=20.0)   # effectively endless

    def gen():
        i = 0
        while True:
            yield lines[i % len(lines)]
            i += 1

    g.ok("G92 X0 Y0 Z0 A0")
    sent, oks, errs, bad, status, dt = stream(g, gen(), window=127, poll_ms=50, duration=seconds)
    s = g.wait_idle()
    say("  %d lines in %.0f s (%.0f lines/s), %d ok, %d errors, %d bad frames, %d status frames"
        % (sent, dt, sent / dt, oks, errs, bad, status))
    info = [l for l in g.ok("$I") if l.startswith(b"[MSG:")]
    say("  diag:", info[0].decode() if info else "-")
    assert oks == sent and errs == 0 and bad == 0


def t_hold(g):
    spm = 250
    g.setup_bench(steps_per_mm=spm, max_rate=30000, accel=2000)
    lines, fx, fy = raster(0.5, 400, 3000)
    g.ok("G92 X0 Y0 Z0 A0")
    start = start_position(g)
    say("\nhold: feed hold 1.5 s into the stream, resume 3 s later")
    events = []

    def on_tick(t):
        # Real-time characters bypass the character-counting window, exactly
        # as a sender's hold button does, so they are injected from the loop
        # rather than from the line stream (which is blocked during the hold).
        if not events and t > 1.5:
            g.write(b"!"); events.append(("!", t))
        elif len(events) == 1 and t > 4.5:
            g.write(b"~"); events.append(("~", t))

    sent, oks, errs, bad, status, dt = stream(g, lines, window=127, poll_ms=50, on_tick=on_tick)
    states = [s[1] for s in g.status[-status:]] if status else []
    seen = []
    for st in states:
        if not seen or seen[-1] != st:
            seen.append(st)
    posok, pos = check_position(g, start, fx, fy)
    say("  %d ok / %d sent, %d errors, %d bad; states seen: %s; final %s"
        % (oks, sent, errs, bad, " -> ".join(seen), "exact" if posok else "OFF %r" % (pos,)))
    assert oks == sent and errs == 0 and bad == 0 and posok
    assert any(s.startswith("Hold") for s in seen), "no Hold state observed"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--sustained", type=int, default=120)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--only", choices=["ramp", "sustained", "hold"])
    a = ap.parse_args()
    g = Grbl(a.port)
    say("port:", g.port)
    try:
        g.reset()
        if a.only in (None, "ramp"):
            t_ramp(g, a.quick)
        if a.only in (None, "hold"):
            t_hold(g)
        if a.only in (None, "sustained") and not a.quick:
            t_sustained(g, a.sustained)
        say("\nALL STREAM TESTS PASSED")
    finally:
        try:
            g.write(b"\x18")
            g.drain(0.5)
            g.setup_bench()
        except Exception:
            pass
        g.close()


if __name__ == "__main__":
    main()
