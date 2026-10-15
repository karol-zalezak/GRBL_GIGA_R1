#!/usr/bin/env python3
"""Direction change against the step pulses on the Tektronix MDO3034: CH1 on
STEP X (D25), CH2 on DIR X (D27).

A zigzag (G91 G1 X2 Y2 / X-2 Y2) with sharp corners: X reverses while the
machine is still moving fast, which is where a drive's direction setup time
matters. For every direction change in a recorded run:

  setup  DIR edge -> the next STEP rising edge
  hold   the last STEP falling edge -> the DIR edge
  and whether DIR ever changed while a step pulse was high.

Run for $56=0 and $56=35, each with a close-up screenshot of one change.
The settings it changes for the run ($11, $56, the X/Y acceleration) are put
back afterwards.

    python scope_dir.py [--delays 0,35] [--corners 20] [--out ../docs/scope]
"""
import argparse
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, r"D:\ClaudeCode\Osciloskop_Tektronix_MDO3034")
from tek_mdo import MDO3000          # noqa: E402
from grbl_link import Grbl           # noqa: E402

THRESH = 1.65


def edges(y, thresh=THRESH):
    hi = y > thresh
    d = np.diff(hi.astype(np.int8))
    return np.nonzero(d == 1)[0] + 1, np.nonzero(d == -1)[0] + 1


def zigzag(g, corners, feed):
    g.ok("G91")
    for _ in range(corners // 2):
        g.ok("G1 X2 Y2 F%d" % feed)
        g.ok("G1 X-2 Y2 F%d" % feed)
    g.ok("G90")


def analyse(w1, w2):
    dt = w1.info["XINCR"]
    sr, sf = edges(w1.y)
    dr, dfall = edges(w2.y)
    de = np.sort(np.r_[dr, dfall])
    rows = []
    for e in de:
        i = np.searchsorted(sr, e)
        j = np.searchsorted(sf, e) - 1
        if i >= len(sr):
            continue
        setup = (sr[i] - e) * dt
        hold = (e - sf[j]) * dt if j >= 0 else np.nan          # no step before it in the window
        high = (i >= 1) and (j >= 0) and sr[i - 1] > sf[j]     # a pulse started after the last fall: still high
        rate = 1.0 / ((sr[i + 1] - sr[i]) * dt) if i + 1 < len(sr) else 0.0
        rows.append((setup, hold, high, rate))
    return np.array(rows), dt, len(sr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--delays", default="0,35", help="$56 values to run, us")
    ap.add_argument("--corners", type=int, default=20)
    ap.add_argument("--out", default=os.path.join(HERE, "..", "docs", "scope"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    g = Grbl(quiet=True)
    s0 = g.settings()
    keep = {n: s0[n] for n in (11, 56, 120, 121)}
    feed = int(min(s0[110], s0[111]))
    print("firmware: $0=%g us  $100=%g  $110=%g mm/min  $11=%g  $56=%g" %
          (s0[0], s0[100], s0[110], s0[11], s0[56]))
    results = {}
    try:
        g.ok("$11=0.2")                          # generous corners: X still fast as it reverses
        g.ok("$120=10000")
        g.ok("$121=10000")
        with MDO3000() as sc:
            for ch in (3, 4):
                sc.channel_on(ch, False)
            sc.configure_channel(1, scale=1.0, position=-3.6, offset=0.0, coupling="DC")
            sc.configure_channel(2, scale=1.0, position=0.2, offset=0.0, coupling="DC")
            sc.configure_acquisition("SAMPLE")
            sc.write("HORIZONTAL:DELAY:MODE OFF")
            sc.configure_trigger("CH2", level=THRESH, slope="EITHER", mode="NORMAL")
            sc.write("TRIGGER:A:LEVEL:CH1 %g" % THRESH)
            for us in [int(x) for x in a.delays.split(",")]:
                g.ok("$56=%d" % us)
                # -- the whole zigzag, 10M points per channel
                sc.configure_horizontal(scale=0.2, record_length=10_000_000, position_pct=5)
                sc.wait_opc()
                sc.write("ACQUIRE:STOPAFTER SEQUENCE")
                sc.write("ACQUIRE:STATE RUN")
                time.sleep(1.0)
                zigzag(g, a.corners, feed)
                g.wait_idle(timeout=60)
                if not sc._wait_acq_done(15):
                    raise SystemExit("scope: no acquisition (DIR on CH2?)")
                w1 = sc.get_waveform("CH1")
                w2 = sc.get_waveform("CH2")
                rows, dt, nsteps = analyse(w1, w2)
                corner = rows[rows[:, 3] > 1000]  # changes with the machine moving on
                results[us] = (rows, corner, dt, nsteps)
                # -- a close-up of one change at a corner
                span = 10e-6 if us == 0 else 20e-6
                sc.configure_horizontal(scale=span, record_length=100_000, position_pct=50)
                sc.wait_opc()
                g.ok("G91")
                for _ in range(5):
                    g.write(("G1 X2 Y2 F%d\nG1 X-2 Y2 F%d\n" % (feed, feed)).encode())
                time.sleep(0.08)                 # past the first change, which starts from rest
                sc.single(timeout_s=3, force=False)
                c1 = sc.get_waveform("CH1")
                c2 = sc.get_waveform("CH2")
                g.wait_idle(timeout=30)
                g.ok("G90")
                sc.screenshot(os.path.join(a.out, "dir_x_56-%d.png" % us))
                cr, cdt, _ = analyse(c1, c2)
                results[us] += (cr, cdt)
            sc.configure_trigger("CH1", level=THRESH, slope="RISE", mode="AUTO")
            sc.configure_horizontal(scale=200e-6, record_length=100_000, position_pct=10)
            sc.run()
    finally:
        for n, v in keep.items():
            g.ok("$%d=%g" % (n, v))
    for us, (rows, corner, dt, nsteps, cr, cdt) in results.items():
        print("\n$56=%d us  (%d steps, %d direction changes recorded, %.1f ns per sample)" %
              (us, nsteps, len(rows), dt * 1e9))
        if len(corner):
            print("   at the corners (%d): setup DIR->STEP min %.2f / median %.2f us, X at %.0f..%.0f kHz" %
                  (len(corner), corner[:, 0].min() * 1e6, np.median(corner[:, 0]) * 1e6,
                   corner[:, 3].min() / 1e3, corner[:, 3].max() / 1e3))
            print("   hold STEP fall->DIR min %.2f us; DIR changed while STEP high: %d" %
                  (np.nanmin(corner[:, 1]) * 1e6, int(corner[:, 2].sum())))
        if len(cr):
            print("   close-up: DIR -> STEP %.1f ns, last STEP fall -> DIR %s (%.2f ns per sample)" %
                  (cr[0, 0] * 1e9, "-" if np.isnan(cr[0, 1]) else "%.2f us" % (cr[0, 1] * 1e6), cdt * 1e9))
    print("\nsettings put back:", ", ".join("$%d=%g" % kv for kv in keep.items()))


if __name__ == "__main__":
    main()
