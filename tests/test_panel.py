#!/usr/bin/env python3
"""Shield v4 additions: OUT7 (D6) and OUT14 (A2), the PWM on OUT7 ($38) and the operator
panel (src/panel.h) - for Grbl-Giga on the Arduino GIGA R1.

No panel needed: the test hooks take the same path as the panel's pins.

  $TPNL=mask[,ms]  presses the panel buttons (bit 0 START, 1 HOLD, 2 STOP) as
                   if their pins were low, ms from now if given; $TPNL=0
                   releases them
  $TPOT=n,value    potentiometer n (1 = A0, 2 = A1) reads value (0-4095);
                   $TPOT=n,-1 hands it back to its pin

and $I reports pnl=<buttons>/<pot A0>/<pot A1>, pwm=<duty 0.1 %, 65535 off>,
dac=<0-4095>, d6=<level of D6>, out=<channels>/<functions>.

  outputs   OUT7 (D6) and OUT14 (A2) as outputs (AUX4/AUX5, M64/M65), $93 range
  pwm       $38 0/1/2: PWM or plain OUT7, the polarity, the DAC either way,
            OUT7 reserved while PWM, laser mode needs PWM
  buttons   START / HOLD like the inputs, STOP: controlled stop (position
            kept, no alarm, spindle off), standing STOP, $39=0
  pots      $44/$45: feed / spindle / rapid override from the reading, the
            middle band, turning takes over, the PC can change it in between,
            set again after a reset

    python test_panel.py [--port COM5] [--only outputs,pwm,...]
"""
import argparse
import re
import time

from grbl_link import Grbl, say

DIAG = {
    "out": re.compile(rb" out=(\d+)/(\d+)"),
    "pwm": re.compile(rb" pwm=(\d+)"),
    "dac": re.compile(rb" dac=(\d+)"),
    "d6": re.compile(rb" d6=(\d+)"),
    "pnl": re.compile(rb" pnl=(\d+)/(\d+)/(\d+)"),
}
OV_RE = re.compile(r"Ov:(\d+),(\d+),(\d+)")


def check(cond, what):
    cond = bool(cond)
    say(("  ok    " if cond else "  FAIL  ") + what)
    return cond


def diag(g):
    for l in g.ok("$I"):
        if b"[MSG:" in l and b" pnl=" in l:
            d = {}
            for k, r in DIAG.items():
                m = r.search(l)
                d[k] = tuple(int(x) for x in m.groups()) if len(m.groups()) > 1 else int(m.group(1))
            return d
    raise RuntimeError("no diagnostics line in $I")


def state(g):
    s = g.query()
    return s[1] if s else ""


def overrides(g, timeout=1.5):
    """(feed, rapid, spindle) from the next status report that carries Ov:."""
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < timeout:
        s = g.query()
        if s:
            m = OV_RE.search(s[5])
            if m:
                return tuple(int(x) for x in m.groups())
        time.sleep(0.02)
    return None


def wait_ov(g, idx, value, timeout=2.0):
    t0 = time.perf_counter()
    ov = None
    while time.perf_counter() - t0 < timeout:
        ov = overrides(g)
        if ov and ov[idx] == value:
            return ov
        time.sleep(0.03)
    return ov


def diag_settled(g):
    """$I after the PWM's preloaded compare value has taken effect (1 kHz)."""
    time.sleep(0.05)
    return diag(g)


def lines_since(g, n0):
    return [l.decode(errors="replace") for l in g.log[n0:]]


# ---------------------------------------------------------------------------

def t_outputs(g):
    say("\nOUT7 (D6) / OUT14 (A2):")
    ok = True
    g.ok("$38=0")
    g.ok("$88=7")                  # AUX4 on OUT7, the free D6 (shield v4 has it on OUT13)
    g.ok("$89=14")                 # AUX5 on OUT14
    g.ok("M64 P4")
    d = diag(g)
    ok &= check(d["out"][0] & (1 << 6) and d["d6"] == 1, "M64 P4 (AUX4 = OUT7): out=%s d6=%d" % (d["out"], d["d6"]))
    g.ok("M65 P4")
    d = diag(g)
    ok &= check(not d["out"][0] & (1 << 6) and d["d6"] == 0, "M65 P4: OUT7 off, d6=%d" % d["d6"])
    g.ok("M64 P5")
    d = diag(g)
    ok &= check(d["out"][0] & (1 << 13), "M64 P5 (AUX5 = OUT14): out=%s" % (d["out"],))
    g.ok("M65 P5")
    d = diag(g)
    ok &= check(not d["out"][0] & (1 << 13), "M65 P5: OUT14 off: out=%s" % (d["out"],))
    r = g.cmd("$88=15")
    ok &= check(r[-1] == b"error:3", "$88=15 (no OUT15): %s" % r[-1].decode())
    r = g.cmd("$93=16384")
    ok &= check(r[-1] == b"error:3", "$93 bit 14 (no OUT15): %s" % r[-1].decode())
    g.ok("$93=64")                 # OUT7 inverted
    d = diag(g)
    ok &= check(d["d6"] == 1 and not d["out"][0] & (1 << 6), "$93=64: OUT7 off but inverted at the pin, d6=%d" % d["d6"])
    g.ok("$93=0")
    g.ok("$88=13")
    return ok


def t_pwm(g):
    say("\nPWM on OUT7 ($38):")
    ok = True
    g.ok("$38=0")
    g.ok("$88=7")
    r = g.cmd("$38=1")
    ok &= check(r[-1] == b"error:42", "$38=1 with AUX4 on OUT7: %s" % r[-1].decode())
    g.ok("$88=13")
    g.ok("$38=1")
    r = g.cmd("$88=7")
    ok &= check(r[-1] == b"error:42", "AUX4 onto OUT7 while it is the PWM: %s" % r[-1].decode())
    g.ok("$30=1000")
    g.ok("$31=0")
    g.ok("M3 S500")
    d = diag(g)
    ok &= check(495 <= d["pwm"] <= 505 and 2000 <= d["dac"] <= 2100,
                "$38=1, M3 S500: pwm %d per mille, dac %d" % (d["pwm"], d["dac"]))
    g.ok("S0")
    d0 = diag_settled(g)
    g.ok("S1000")
    d1 = diag_settled(g)
    ok &= check(d0["d6"] == 0 and d1["d6"] == 1, "not inverted: S0 -> D6 low (%d), S1000 -> D6 high (%d)" % (d0["d6"], d1["d6"]))
    g.ok("$38=2")
    g.ok("S0")
    d0 = diag_settled(g)
    g.ok("S1000")
    d1 = diag_settled(g)
    ok &= check(d0["d6"] == 1 and d1["d6"] == 0, "$38=2 inverted: S0 -> D6 high (%d), S1000 -> D6 low (%d)" % (d0["d6"], d1["d6"]))
    g.ok("$38=0")
    g.ok("S500")
    d = diag(g)
    ok &= check(d["pwm"] == 65535 and 2000 <= d["dac"] <= 2100 and d["d6"] == 0,
                "$38=0: PWM off (pwm=%d), the DAC still at half (dac %d), D6 an output (%d)" % (d["pwm"], d["dac"], d["d6"]))
    g.ok("M5")
    r = g.cmd("$32=1")
    ok &= check(r[-1] == b"error:17", "laser mode without PWM: %s" % r[-1].decode())
    g.ok("$38=1")
    g.ok("$32=1")
    r = g.cmd("$38=0")
    ok &= check(r[-1] == b"error:17", "$38=0 in laser mode: %s" % r[-1].decode())
    g.ok("$32=0")
    g.ok("$38=0")
    r = g.cmd("$38=3")
    ok &= check(r[-1] == b"error:3", "$38=3: %s" % r[-1].decode())
    return ok


def t_buttons(g):
    say("\npanel buttons:")
    ok = True
    g.ok("$39=1")
    g.ok("$TPNL=3")
    d = diag(g)
    s = g.query()
    ok &= check(d["pnl"][0] == 3 and "H" in s[5] and "S" in s[5], "START + HOLD pressed: pnl=%d, %s" % (d["pnl"][0], s[5]))
    g.ok("$TPNL=0")
    time.sleep(0.05)

    g.ok("G92 X0")
    g.ok("G1 X50 F600")            # 5 s
    time.sleep(0.5)
    g.ok("$TPNL=2,10")             # HOLD ...
    g.ok("$TPNL=0,300")
    g.ok("$TPNL=1,1300")           # ... START a second later
    g.ok("$TPNL=0,1600")
    time.sleep(0.8)
    st1 = state(g)
    x1 = g.query()[2]
    time.sleep(0.4)
    x2 = g.query()[2]
    time.sleep(0.9)
    st2 = state(g)
    ok &= check(st1.startswith("Hold") and abs(x2 - x1) < 0.001, "HOLD: %s, standing at X=%.3f" % (st1, x2))
    ok &= check(st2.startswith("Run"), "START: %s" % st2)

    time.sleep(0.5)
    g.ok("M3 S500")                # only parsed once the move is done: wait
    g.wait_idle()
    g.ok("G1 X0 F600")
    time.sleep(1.0)
    n0 = len(g.log)
    g.ok("$TPNL=4,10")             # STOP
    g.ok("$TPNL=0,300")
    t0 = time.perf_counter()
    states = set()
    while time.perf_counter() - t0 < 3.0:
        s = g.query()
        if s:
            states.add(s[1].split(":")[0])
            if s[1].startswith("Idle") and "Hold" in states:
                break
        time.sleep(0.03)
    g.drain(0.3)
    log = lines_since(g, n0)
    s = g.query()
    d = diag(g)
    ok &= check("[MSG:Panel STOP]" in log and any(l.startswith("Grbl ") for l in log),
                "STOP while moving: [MSG:Panel STOP], then Grbl's reset")
    ok &= check("Hold" in states and s[1].startswith("Idle") and not any(l.startswith("ALARM") for l in log),
                "  hold first, then Idle without an alarm (states %s)" % sorted(states))
    ok &= check(5.0 < s[2] < 45.0 and d["out"][0] & 1 == 0,
                "  stopped at X=%.3f (position kept), spindle off (out=%s)" % (s[2], d["out"]))
    x_stop = s[2]
    g.ok("G1 X0 F3000")
    g.wait_idle()
    ok &= check(abs(g.query()[2]) < 0.001, "  the position was right: back at X=0 from X=%.3f" % x_stop)

    g.ok("M3 S500")
    n0 = len(g.log)
    g.ok("$TPNL=4")
    time.sleep(0.2)
    g.ok("$TPNL=0")
    g.drain(0.3)
    d = diag(g)
    ok &= check(d["out"][0] & 1 == 0 and state(g).startswith("Idle") and "[MSG:Panel STOP]" in lines_since(g, n0),
                "STOP standing with the spindle on: spindle off, Idle")

    g.ok("$39=0")
    g.ok("$TPNL=3")
    d = diag(g)
    s = g.query()
    ok &= check(d["pnl"][0] == 0 and "H" not in s[5], "$39=0: buttons ignored (pnl=%d)" % d["pnl"][0])
    g.ok("$TPNL=0")
    g.ok("$39=1")
    return ok


def t_pots(g):
    say("\npanel potentiometers:")
    ok = True
    g.ok("$TPOT=1,2048")
    g.ok("$TPOT=2,2048")
    g.ok("$44=1")
    g.ok("$45=2")
    ov = wait_ov(g, 0, 100)
    ok &= check(ov and ov[0] == 100 and ov[2] == 100, "both in the middle: feed / spindle 100 %%: Ov %s" % (ov,))
    for raw, want in [(0, 10), (4095, 200), (1024, 57), (2000, 100), (2100, 100)]:
        g.ok("$TPOT=1,%d" % raw)
        ov = wait_ov(g, 0, want)
        ok &= check(ov and ov[0] == want, "A0 = %4d -> feed %d %%: Ov %s" % (raw, want, ov))
    g.ok("$TPOT=2,3000")
    ov = wait_ov(g, 2, 144)
    ok &= check(ov and ov[2] == 144, "A1 = 3000 -> spindle 144 %%: Ov %s" % (ov,))
    time.sleep(0.5)                # the knob rests: it lets go of the override
    g.write(b"\x9A")               # the PC: spindle +10 %
    time.sleep(0.6)
    ov = overrides(g)
    ok &= check(ov and ov[2] == 154, "the PC changes it to 154 %%, the knob not turned: stays (Ov %s)" % (ov,))
    g.ok("$TPOT=2,3030")           # less than the dead band
    time.sleep(0.4)
    ov = overrides(g)
    ok &= check(ov and ov[2] == 154, "turned by less than the dead band: still 154 (Ov %s)" % (ov,))
    g.ok("$TPOT=2,3100")
    ov = wait_ov(g, 2, 149)
    ok &= check(ov and ov[2] == 149, "turned: the knob takes over, 149 %% (Ov %s)" % (ov,))
    time.sleep(0.3)
    d = diag(g)
    ok &= check(abs(d["pnl"][2] - 3100) <= 2, "$I pnl= shows the (smoothed) reading: %s" % (d["pnl"],))
    g.ok("$TPOT=1,0")
    wait_ov(g, 0, 10)
    g.reset()
    ov = wait_ov(g, 0, 10)
    ok &= check(ov and ov[0] == 10 and ov[2] == 149, "after a reset the knobs set them again: Ov %s" % (ov,))
    g.ok("$44=3")
    for raw, want in [(0, 25), (2048, 50), (4095, 100)]:
        g.ok("$TPOT=1,%d" % raw)
        ov = wait_ov(g, 1, want)
        ok &= check(ov and ov[1] == want, "$44=3 rapid: A0 = %4d -> %d %%: Ov %s" % (raw, want, ov))
    r = g.cmd("$44=4")
    ok &= check(r[-1] == b"error:3", "$44=4: %s" % r[-1].decode())
    g.ok("$44=0")
    g.ok("$45=0")
    g.write(b"\x90")               # feed override back to 100 %
    g.write(b"\x99")               # spindle override back to 100 %
    g.ok("$TPOT=1,-1")
    g.ok("$TPOT=2,-1")
    d = diag(g)
    ok &= check(d["pnl"][1:] == (0, 0), "$44=$45=0: off (pnl=%s)" % (d["pnl"],))
    return ok


TESTS = [("outputs", t_outputs), ("pwm", t_pwm), ("buttons", t_buttons), ("pots", t_pots)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--only")
    a = ap.parse_args()
    g = Grbl(a.port)
    say("port: %s" % g.port)
    keep = g.settings()
    only = a.only.split(",") if a.only else [n for n, _ in TESTS]
    results = {}
    try:
        g.setup_bench()
        for name, fn in TESTS:
            if name not in only:
                continue
            g.reset()
            if state(g).startswith("Alarm"):
                g.cmd("$X")
            results[name] = fn(g)
            g.cmd("M5")
        say("\n" + ", ".join("%s %s" % (n, "ok" if r else "FAIL") for n, r in results.items()))
        return 0 if all(results.values()) else 1
    finally:
        g.reset()
        g.cmd("$X")
        g.cmd("$TPNL=0")
        g.cmd("$TPOT=1,-1")
        g.cmd("$TPOT=2,-1")
        for n in (32, 38, 39, 44, 45, 88, 89, 93, 30, 31):
            if n in keep:
                r = g.cmd("$%d=%g" % (n, keep[n]))
                if r[-1] != b"ok":
                    say("  could not restore $%d=%g: %s" % (n, keep[n], r[-1].decode()))
        g.close()


if __name__ == "__main__":
    raise SystemExit(main())
