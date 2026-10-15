#!/usr/bin/env python3
"""Quick look at interrupt durations while stepping (development aid).

Runs a 1-axis and a 4-axis move at a given step rate and prints the $I
diagnostics: merged pulses and the longest scan / stepper interrupt (isr=).

    python probe_isr.py [--rate 200000]
"""
import argparse
import re

from grbl_link import Grbl

ISR_RE = re.compile(rb"merged=(\d+).* isr=(\d+)/(\d+)")


def diag(g):
    for l in g.ok("$I"):
        m = ISR_RE.search(l)
        if m:
            return tuple(int(x) for x in m.groups())
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--rate", type=int, default=200000)
    a = ap.parse_args()
    g = Grbl(a.port, quiet=True)
    spm = 1000
    g.setup_bench(steps_per_mm=spm, max_rate=100000, accel=5000, pulse_us=3)
    feed = a.rate / spm * 60
    base = diag(g)
    print("idle: merged=%d isr scan=%.1f us stepper=%.1f us" % (base[0], base[1] / 10, base[2] / 10))
    for axes in ("X", "XYZA"):
        words = " ".join("%s%d" % (c, 300) for c in axes)
        g.ok("G91 G1 %s F%d" % (words, feed * (len(axes) ** 0.5)))
        g.ok("G4 P0", timeout=60)
        d = diag(g)
        print("%-4s %d steps/s: merged +%d, isr scan=%.1f us stepper=%.1f us"
              % (axes, a.rate, d[0] - base[0], d[1] / 10, d[2] / 10))
        base = d
    g.ok("G90")
    g.close()


if __name__ == "__main__":
    main()
