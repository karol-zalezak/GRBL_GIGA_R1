#!/usr/bin/env python3
"""Step pulses of one axis on the Tektronix MDO3034 (CH1 on the STEP pin),
compared with the firmware's settings.

  1. A whole G0 move recorded (10M points): every step's rate (ramp up, the
     cruise, ramp down), every pulse's width, the step count.
  2. During the cruise, the scope's own measurements: pulse width, rise and
     fall time, levels, frequency - plus a screenshot.

    python scope_step.py [--axis X] [--dist 50] [--dist2 300] [--tag 200k] [--out ../docs/scope]

The firmware caps the step rate: the period is at least 5 us (200 kHz,
MINIMUM_STEP_CYCLES) and at least $0 + 2 us (STEP_PERIOD_MARGIN_US); the
expected rate takes both into account.

Needs D:\\ClaudeCode\\Osciloskop_Tektronix_MDO3034 (tek_mdo.py, pyvisa).
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

AXES = "XYZA"
THRESH = 1.65                        # V, half of 3.3 V


def edges(y, thresh):
    hi = y > thresh
    d = np.diff(hi.astype(np.int8))
    return np.nonzero(d == 1)[0] + 1, np.nonzero(d == -1)[0] + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--axis", default="X")
    ap.add_argument("--dist", type=float, default=50.0, help="mm, the move recorded whole")
    ap.add_argument("--dist2", type=float, default=0.0, help="mm, the move for the cruise measurements (default: dist)")
    ap.add_argument("--tag", default="", help="added to the file names")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "docs", "scope"))
    a = ap.parse_args()
    ax = AXES.index(a.axis.upper())
    os.makedirs(a.out, exist_ok=True)

    g = Grbl(quiet=True)
    s = g.settings()
    pulse_us = s[0]
    spm = s[100 + ax]
    vmax = s[110 + ax]               # mm/min
    acc = s[120 + ax]                # mm/s^2
    f_cmd = spm * vmax / 60.0                       # what $100 and $110 ask for
    f_cap = 1e6 / max(5.0, pulse_us + 2.0)          # the firmware's ceiling
    f_set = min(f_cmd, f_cap)
    v = f_set / spm                                 # mm/s
    t_ramp = v / acc
    n_steps = round(a.dist * spm)
    t_move = 2 * t_ramp + max(0.0, a.dist - v * v / acc) / v
    dist2 = a.dist2 or a.dist
    tag = ("_" + a.tag) if a.tag else ""
    print("firmware: $0=%g us  $%d=%g steps/mm  $%d=%g mm/min  $%d=%g mm/s^2" %
          (pulse_us, 100 + ax, spm, 110 + ax, vmax, 120 + ax, acc))
    print("expected: %.1f Hz asked, ceiling %.1f Hz -> %.1f Hz (period %.2f us), ramp %.3f s, %d steps, move %.2f s" %
          (f_cmd, f_cap, f_set, 1e6 / f_set, t_ramp, n_steps, t_move))

    g.ok("$X") if g.query()[1].startswith("Alarm") else None
    g.ok("G90 G21")
    start = g.query()[2 + ax] if ax < 3 else g.query()[6]

    with MDO3000() as sc:
        print("scope:", sc.idn()["model"])
        for ch in (2, 3, 4):
            sc.channel_on(ch, False)
        sc.configure_channel(1, scale=1.0, position=-3.0, offset=0.0, coupling="DC")
        sc.configure_acquisition("SAMPLE")
        sc.write("HORIZONTAL:DELAY:MODE OFF")         # so that the trigger position applies
        sc.configure_trigger("CH1", level=THRESH, slope="RISE", mode="NORMAL")

        # ---- 1: the whole move --------------------------------------------
        need = (t_move + 1.0) / 10                         # s/div, 1-2-4 steps
        div = next(d for d in (0.1, 0.2, 0.4, 1.0, 2.0, 4.0, 10.0) if d >= need)
        sc.configure_horizontal(scale=div, record_length=10_000_000, position_pct=5)
        sc.wait_opc()
        sc.write("ACQUIRE:STOPAFTER SEQUENCE")
        sc.write("ACQUIRE:STATE RUN")
        time.sleep(1.0)                                   # armed
        target = start + a.dist
        g.ok("G0 %s%.3f" % (a.axis.upper(), target))
        g.ok("G4 P0", timeout=t_move + 30)
        if not sc._wait_acq_done(div * 10 + 10):
            raise SystemExit("scope: no acquisition (is CH1 on the STEP pin?)")
        w = sc.get_waveform("CH1")
        dt = w.info["XINCR"]
        r, f = edges(w.y, THRESH)
        f = f[f > r[0]]
        n = min(len(r), len(f))
        widths = (f[:n] - r[:n]) * dt * 1e6
        per = np.diff(r) * dt
        freq = 1.0 / per
        tr = r[1:] * dt
        t0 = tr - tr[0]
        cruise = (t0 > t_ramp + 0.1) & (t0 < (r[-1] - r[0]) * dt - t_ramp - 0.1)
        f_cruise = cruise.sum() / (tr[cruise][-1] - tr[cruise][0])      # steps / time
        per_c = per[cruise] * 1e6
        imax = int(np.argmax(freq))
        # acceleration from the first half of the ramp up (linear in time)
        up = (tr - tr[0]) < 0.8 * t_ramp
        slope = np.polyfit(tr[up], freq[up], 1)[0]
        print("\n1) whole move, %.2f us per sample:" % (dt * 1e6))
        print("   steps counted %d (expected %d)" % (len(r), n_steps))
        print("   cruise %.1f Hz = %.1f mm/min (set %.1f Hz = %g mm/min); periods %.0f..%.0f us" %
              (f_cruise, f_cruise * 60 / spm, f_set, vmax, per_c.min(), per_c.max()))
        print("   highest single step rate %.1f Hz at %.3f s (one short period on a ramp)" %
              (freq[imax], t0[imax]))
        print("   pulse width min %.2f / median %.2f / max %.2f us (set %g us; +-%.2f us resolution)" %
              (widths.min(), np.median(widths), widths.max(), pulse_us, dt * 1e6))
        print("   ramp: %.0f Hz/s = %.2f mm/s^2 (set %g)" % (slope, slope / spm, acc))
        print("   move time %.3f s (expected %.3f)" % ((r[-1] - r[0]) * dt, t_move))
        np.savez_compressed(os.path.join(a.out, "step_%s%s_move.npz" % (a.axis.lower(), tag)),
                            t_rise=r * dt, t_fall=f[:n] * dt)
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
            fig, axs = plt.subplots(2, 1, figsize=(10, 6), sharex=True)
            axs[0].plot(tr - tr[0], freq, lw=0.8)
            axs[0].axhline(f_set, color="r", ls="--", lw=0.8, label="set %.1f Hz" % f_set)
            axs[0].set_ylabel("step rate [Hz]")
            axs[0].legend()
            axs[1].plot(r[:n] * dt - r[0] * dt, widths, ".", ms=1)
            axs[1].axhline(pulse_us, color="r", ls="--", lw=0.8, label="$0 = %g us" % pulse_us)
            axs[1].set_ylabel("pulse width [us]")
            axs[1].set_xlabel("time [s]")
            axs[1].legend()
            fig.suptitle("STEP %s, G0 %g mm, $0=%g us, asked %.0f Hz - Tektronix MDO3034 CH1" %
                         (a.axis.upper(), a.dist, pulse_us, f_cmd))
            fig.tight_layout()
            fig.savefig(os.path.join(a.out, "step_%s%s_move.png" % (a.axis.lower(), tag)), dpi=110)
        except ImportError:
            pass

        # ---- 2: during the cruise, the scope's measurements -----------------
        g.wait_idle(timeout=30)
        g.write(("G0 %s%.3f\n" % (a.axis.upper(), start + a.dist - dist2)).encode())   # back, cruising
        time.sleep(t_ramp + 0.3)
        res = {}
        sc.configure_horizontal(scale=4e-6, record_length=100_000, position_pct=20)
        sc.wait_opc()
        sc.single(timeout_s=3, force=False)
        for t in ("PWIDTH", "RISE", "FALL", "HIGH", "LOW", "PK2PK", "POVERSHOOT", "NOVERSHOOT"):
            m = sc.measure("CH1", t)
            res[t] = m.value
        sc.configure_horizontal(scale=2e-3, record_length=100_000, position_pct=10)
        sc.wait_opc()
        sc.single(timeout_s=3, force=False)
        for t in ("FREQUENCY", "PERIOD", "PDUTY"):
            res[t] = sc.measure("CH1", t).value
        w2 = sc.get_waveform("CH1")                       # the mean over the whole window
        r2, _ = edges(w2.y, THRESH)
        res["MEAN_F"] = (len(r2) - 1) / ((r2[-1] - r2[0]) * w2.info["XINCR"]) if len(r2) > 2 else None
        res["N"] = len(r2)
        g.wait_idle(timeout=t_move + 60)
        # the screenshots after the move: they take a while
        sc.configure_horizontal(scale=4e-6, record_length=100_000, position_pct=20)
        g.write(("G0 %s%.3f\n" % (a.axis.upper(), start + a.dist)).encode())
        time.sleep(t_ramp + 0.3)
        sc.single(timeout_s=3, force=False)
        sc.screenshot(os.path.join(a.out, "step_%s%s_pulse.png" % (a.axis.lower(), tag)))
        g.wait_idle(timeout=t_move + 60)
        g.ok("G0 %s%.3f" % (a.axis.upper(), start))
        g.wait_idle(timeout=t_move + 60)
        sc.configure_horizontal(scale=200e-6, record_length=100_000, position_pct=10)
        sc.configure_trigger("CH1", level=THRESH, slope="RISE", mode="AUTO")
        sc.run()
    print("\n2) cruise, the scope's measurements:")
    print("   pulse width %.3f us (set %g us)" % (res["PWIDTH"] * 1e6, pulse_us))
    print("   rise %.2f ns, fall %.2f ns, high %.3f V, low %.3f V, pk-pk %.3f V, overshoot +%.0f/-%.0f %%" %
          (res["RISE"] * 1e9, res["FALL"] * 1e9, res["HIGH"], res["LOW"], res["PK2PK"],
           res["POVERSHOOT"] or 0, res["NOVERSHOOT"] or 0))
    print("   frequency %.2f Hz, period %.3f us, duty %.2f %% (first cycle; expected %.2f Hz)" %
          (res["FREQUENCY"], res["PERIOD"] * 1e6, res["PDUTY"], f_set))
    if res["MEAN_F"]:
        print("   mean over %d pulses in 20 ms: %.1f Hz (expected %.1f Hz, %+.3f %%)" %
              (res["N"], res["MEAN_F"], f_set, (res["MEAN_F"] / f_set - 1) * 100))
    print("   low time between pulses %.3f us" % (res["PERIOD"] * 1e6 - res["PWIDTH"] * 1e6))
    print("\nfiles in", os.path.abspath(a.out))


if __name__ == "__main__":
    main()
