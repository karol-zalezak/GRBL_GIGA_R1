#!/usr/bin/env python3
"""Universal I/O, limits, homing, central STOP, spindle, collet, brake and
direction-delay tests for Grbl-Giga on the Arduino GIGA R1 (PLAN.md parts 1-2).

No machine needed. Inputs are driven by the firmware's test hooks, which take
the same path as a real optocoupler:

  $TIN=n,1   pulls input channel n (21 = STOP, 22 = driver power) low,
             $TIN=n,0 releases it
  $TSW=n,X+5 channel n reads active while X >= 5 mm (a virtual limit switch)

and everything is checked through $I (in=, out=, en=, brake=, estop=, drv=,
spindle=, dir=, motor=) and the status reports.

  assign     $60-$93: conflicts refused, shared limit input accepted
  limits     ALARM:1 + [MSG:Limit X-], leaving a held switch, error:41,
             one input per axis, two switches on one input
  homing     $H on virtual switches; far switch -> ALARM:20; no switch -> error:45
  estop      STOP during motion: ALARM:18, steps stop, outputs off, $X refused
  spindle    at-speed wait / timeout ALARM:14, loss -> hold or alarm, VFD
             fault -> ALARM:19 + reset pulse
  collet     interlocks ($51, M3 refused while open), M6 pause
  brake      $48/$49/$50 sequence, $59=0
  dirdelay   $56: no step closer than $56 us after a direction change
  drvpwr     $94/$95: drive supply lost in motion, idle, homing and alarm ->
             ALARM:22, brake on, $H/$X refused until it is back for $95

    python test_io.py [--port COM5] [--only limits,estop,...]
"""
import argparse
import re
import time

from grbl_link import Grbl, say

FIELDS_RE = {
    "motor": re.compile(rb" motor=(-?\d+),(-?\d+),(-?\d+),(-?\d+)"),
    "in": re.compile(rb" in=(\d+)/(\d+)"),
    "out": re.compile(rb" out=(\d+)/(\d+)"),
    "en": re.compile(rb" en=(\d+)"),
    "brake": re.compile(rb" brake=(\d+)"),
    "estop": re.compile(rb" estop=(\d+)/(\d+)/(\d+)/(\d+)"),
    "drv": re.compile(rb" drv=(\d+)/(\d+)"),
    "spindle": re.compile(rb" spindle=(\d+)"),
    "dir": re.compile(rb" dir=(\d+)/(\d+)"),
    "merged": re.compile(rb" merged=(\d+)"),
}


def diag(g):
    for l in g.ok("$I"):
        if l.startswith(b"[MSG:") and b" motor=" in l:
            d = {}
            for k, r in FIELDS_RE.items():
                m = r.search(l)
                v = tuple(int(x) for x in m.groups())
                d[k] = v if len(v) > 1 else v[0]
            return d
    raise RuntimeError("no diag line in $I")


def check(cond, what):
    cond = bool(cond)
    say(("  ok    " if cond else "  FAIL  ") + what)
    return cond


def collect(g, until, timeout=3.0):
    """Reads lines until one starts with `until` (bytes) or the timeout ends."""
    seen = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        l = g.readline(timeout=0.2)
        if l:
            seen.append(l)
            if l.startswith(until):
                break
    return seen


def alarm_reset(g, timeout=4.0):
    """ALARM:14 and :19 stop the machine and then reset Grbl (planner and
    parser cleared, position kept): reads up to the unlock prompt after the
    banner. Returns the lines seen."""
    return collect(g, b"[MSG:'$H'|'$X' to unlock]", timeout=timeout)


def state(g):
    s = g.query()
    return s[1] if s else None


def pn(g):
    s = g.query()
    m = re.search(r"\|Pn:([A-Z]+)", s[5]) if s else None
    return m.group(1) if m else ""


def recover(g):
    """Back to Idle after an alarm, test inputs released."""
    g.reset()
    g.ok("$94=0")
    g.ok("$TIN=0,0")
    g.ok("$TSW=0")
    time.sleep(0.15)
    r = g.cmd("$X")
    return r[-1] == b"ok"


def sync(g, timeout=60):
    g.ok("G4 P0", timeout=timeout)


# ---------------------------------------------------------------------------

def t_assign(g):
    say("\nassignment:")
    ok = True
    r = g.cmd("$61=3")
    ok &= check(r[-1] == b"error:42", "$61=3 (X+ onto IN3, which is Y-) refused: %s" % r[-1].decode())
    r = g.cmd("$61=1")
    ok &= check(r[-1] == b"ok", "$61=1 (X+ shares IN1 with X-) accepted: %s" % r[-1].decode())
    r = g.cmd("$62=1")
    ok &= check(r[-1] == b"error:42", "$62=1 (Y- onto X's input) refused: %s" % r[-1].decode())
    g.ok("$61=2")
    r = g.cmd("$60=21")
    ok &= check(r[-1] == b"error:3", "$60=21 (no such channel) refused: %s" % r[-1].decode())
    r = g.cmd("$79=4194304")
    ok &= check(r[-1] == b"error:3", "$79 with bit 22 refused: %s" % r[-1].decode())
    r = g.cmd("$5=1")
    ok &= check(r[-1] == b"error:3", "$5 (gone, now in $79) refused: %s" % r[-1].decode())
    # NC: $79 bit 0 makes the idle IN1 read as active.
    g.ok("$79=1")
    d = diag(g)
    ok &= check(d["in"][0] & 1 and d["in"][1] & 1, "$79=1: idle IN1 reads active, X- function on: %s" % (d["in"],))
    g.ok("$79=0")
    d = diag(g)
    ok &= check(d["in"] == (0, 0), "$79=0: all inputs quiet: %s" % (d["in"],))
    return ok


def limit_alarm(g, cmd, what):
    """Sends cmd (bytes line or None), expects ALARM:1, returns (ok, [MSG:Limit ...] text)."""
    if cmd:
        g.write(cmd + b"\n")
    seen = collect(g, b"[MSG:Limit", timeout=3.0)
    alarm = b"ALARM:1" in seen
    msg = [l.decode() for l in seen if l.startswith(b"[MSG:Limit")]
    return check(alarm, "%s -> ALARM:1 %s" % (what, msg)), (msg[0] if msg else "")


def t_limits(g):
    say("\nlimit switches (hard limits on):")
    ok = True
    g.setup_bench(steps_per_mm=100, max_rate=6000, accel=1000)
    g.ok("$21=1")

    # Two inputs per axis (the default): Grbl knows which end is touched.
    r, msg = limit_alarm(g, b"$TIN=1,1", "X- (IN1) closes")
    ok &= r and check(msg == "[MSG:Limit X-]", "message names the switch: %s" % msg)
    g.reset()
    ok &= check(state(g).startswith("Alarm"), "after the reset, X- still held: Alarm (Check Limits)")
    ok &= check("X" in pn(g), "Pn shows X: %s" % pn(g))
    g.ok("$X")
    r = g.cmd("$J=G91 X-1 F600")
    ok &= check(r[-1] == b"error:41", "jog further into X- refused: %s" % r[-1].decode())
    x0 = g.query()[2]
    g.ok("$J=G91 X2 F600")
    time.sleep(0.05)
    g.wait_idle()
    s = g.query()
    ok &= check(s[1].startswith("Idle") and abs(s[2] - x0 - 2.0) < 0.01, "jog away from X- with the switch held: %s, moved %.3f" % (s[1], s[2] - x0))
    r, msg = limit_alarm(g, b"G91 G1 X-1 F600", "G1 back towards the held X-")
    ok &= r
    recover(g)

    # Two switches on one input: side unknown, edge only (as upstream).
    g.ok("$61=1")
    r, msg = limit_alarm(g, b"$TIN=1,1", "shared X input closes")
    ok &= r and check(msg == "[MSG:Limit X]", "side unknown: %s" % msg)
    g.reset()
    g.ok("$X")
    g.ok("$J=G91 X-1 F600")
    time.sleep(0.05)
    g.wait_idle()
    ok &= check(state(g).startswith("Idle"), "shared input held: moving either way allowed (%s)" % state(g))
    recover(g)
    g.ok("$61=2")

    # One switch only (X+ unassigned): it is at the minus end.
    g.ok("$61=0")
    r, msg = limit_alarm(g, b"$TIN=1,1", "X- (only switch) closes")
    ok &= r and check(msg == "[MSG:Limit X-]", "named: %s" % msg)
    g.reset()
    g.ok("$X")
    r = g.cmd("$J=G91 X-1 F600")
    ok &= check(r[-1] == b"error:41", "jog into it refused: %s" % r[-1].decode())
    g.ok("$J=G91 X1 F600")
    time.sleep(0.05)
    g.wait_idle()
    ok &= check(state(g).startswith("Idle"), "jog away allowed: %s" % state(g))
    recover(g)
    g.ok("$61=2")

    # Plus switch, and a switch hit during motion.
    g.ok("G90 G0 X0")
    sync(g)
    g.ok("$TSW=2,X+3")
    r, msg = limit_alarm(g, b"G1 X10 F1200", "moving into a virtual X+ at 3 mm")
    ok &= r and check(msg == "[MSG:Limit X+]", "named: %s" % msg)
    recover(g)
    g.ok("$21=0")
    return ok


def t_homing(g):
    say("\nhoming on virtual switches:")
    ok = True
    g.setup_bench(steps_per_mm=100, max_rate=6000, accel=1000)
    for n, v in [(22, 1), (23, 0), (24, 200), (25, 1000), (26, 50), (27, 1), (43, 0)]:
        g.ok("$%d=%s" % (n, v))
    # $23=0 homes towards plus: X+ (IN2), Y+ (IN4), Z+ (IN6), 5 mm from here.
    for ch, ax in [(2, "X"), (4, "Y"), (6, "Z")]:
        g.ok("$TSW=%d,%s+5" % (ch, ax))
    t0 = time.monotonic()
    r = g.cmd("$H", timeout=60)
    s = g.query()
    ok &= check(r[-1] == b"ok", "$H finished in %.1f s: %s" % (time.monotonic() - t0, r[-1].decode()))
    ok &= check(s and all(abs(v + 1.0) < 0.011 for v in (s[2], s[3], s[4])),
                "machine position after pull-off: %s" % (s[2:5] if s else None,))
    g.ok("$TSW=0")

    # The far switch closes during homing: wrong $23 or swapped wires.
    g.ok("$TSW=5,Z+3")          # IN5 is Z-, but here it sits at the plus end
    g.ok("$TSW=6,Z+5")
    g.write(b"$H\n")
    seen = collect(g, b"ALARM:", timeout=30)
    ok &= check(b"ALARM:20" in seen, "Z- closes while homing towards Z+: %s" % [l.decode() for l in seen][-2:])
    recover(g)

    # No switch at the end $23 homes to.
    g.ok("$65=0")
    r = g.cmd("$H")
    ok &= check(r[-1] == b"error:45", "$65=0 (no Z+): $H refused: %s" % r[-1].decode())
    g.ok("$65=6")
    g.ok("$22=0")
    return ok


def t_estop(g):
    say("\ncentral STOP:")
    ok = True
    g.setup_bench(steps_per_mm=100, max_rate=6000, accel=1000)
    g.ok("$57=1")
    g.ok("M3 S500")
    g.ok("M8")
    d0 = diag(g)
    ok &= check(d0["out"][0] & 0b101, "spindle and flood on: out=%s" % (d0["out"],))
    g.write(b"G1 X50 F3000\n")
    time.sleep(0.4)
    g.write(b"$TIN=21,1\n")
    seen = collect(g, b"[MSG:E-stop", timeout=2.0)
    ok &= check(b"ALARM:18" in seen and b"[MSG:E-stop]" in seen, "STOP during motion: %s" % [l.decode() for l in seen if not l.startswith(b"<")])
    # Critical alarm: no lines until reset. Status reports still work.
    s = g.query()
    ok &= check(s and s[1].startswith("Alarm") and "E" in (re.search(r"Pn:([A-Z]+)", s[5]) or [None, ""])[1],
                "Alarm with Pn:E: %s" % (s[1] + s[5] if s else None))
    p1 = s[2]
    time.sleep(0.3)
    s = g.query()
    ok &= check(abs(s[2] - p1) < 1e-6, "no step after the trip: X %.3f -> %.3f" % (p1, s[2]))
    g.reset()
    d = diag(g)
    ok &= check(d["estop"][0] == 2 and d["estop"][3] == 1,
                "latched, tripped by its interrupt: estop=%s (trip routine %.1f us)" % (d["estop"], d["estop"][2] / 10))
    ok &= check(d["out"] == (0, 0) and d["en"] == 0 and d["brake"] == 0,
                "outputs, enable and brake off: out=%s en=%d brake=%d" % (d["out"], d["en"], d["brake"]))
    r = g.cmd("$X")
    ok &= check(r[-1] == b"error:40", "$X while STOP is active: %s" % r[-1].decode())
    g.ok("$TIN=21,0")
    time.sleep(0.2)
    r = g.cmd("$X")
    ok &= check(r[-1] == b"error:40", "released but not reset yet: $X still refused: %s" % r[-1].decode())
    g.reset()
    r = g.cmd("$X")
    ok &= check(r[-1] == b"ok", "reset after release, then $X: %s" % r[-1].decode())
    d = diag(g)
    ok &= check(d["estop"][0] == 0, "not latched any more: estop=%s" % (d["estop"],))

    # The scan's path: STOP already active when it gets switched on.
    g.ok("$57=0")
    g.ok("$TIN=21,1")
    g.write(b"$57=1\n")
    seen = collect(g, b"ALARM:", timeout=2.0)
    g.reset()
    d = diag(g)
    ok &= check(b"ALARM:18" in seen and d["estop"][3] == 2, "found by the 10 kHz scan: estop=%s" % (d["estop"],))
    g.ok("$57=0")
    recover(g)
    return ok


def t_spindle(g):
    say("\nspindle at speed / fault:")
    ok = True
    g.setup_bench(steps_per_mm=100, max_rate=6000, accel=1000)
    # The VFD's relays on whatever inputs $77/$78 put them (shield v4: IN11/IN12).
    s = g.settings()
    at, flt = int(s.get(77, 0)) or 11, int(s.get(78, 0)) or 12
    g.ok("$77=%d" % at)
    g.ok("$78=%d" % flt)
    g.ok("$52=1500")
    g.ok("$53=1")
    t0 = time.monotonic()
    g.write(b"M3 S500\n")
    seen = alarm_reset(g)
    ok &= check(b"ALARM:14" in seen, "no at-speed signal: ALARM:14 after %.2f s" % (time.monotonic() - t0))
    d = diag(g)
    ok &= check(d["out"][0] & 0b11 == 0, "spindle outputs off: out=%s" % (d["out"],))
    recover(g)

    g.ok("$TIN=%d,1" % at)
    time.sleep(0.05)
    t0 = time.monotonic()
    g.ok("M3 S500")
    dt = time.monotonic() - t0
    ok &= check(dt < 0.3, "relay already closed: M3 done in %.3f s" % dt)
    t0 = time.monotonic()
    g.ok("S800")
    dt = time.monotonic() - t0
    ok &= check(0.2 < dt < 0.6, "S change: waits out the 250 ms settle: %.3f s" % dt)
    t0 = time.monotonic()
    g.ok("M4")
    dt = time.monotonic() - t0
    ok &= check(0.2 < dt < 0.6, "reversed: the relay still closed from FWD, waits out the settle: %.3f s" % dt)
    ok &= check(diag(g)["spindle"] == 2, "spindle=2 (at speed)")

    g.ok("G91 G1 X30 F600")
    time.sleep(0.6)
    g.ok("$TIN=%d,0" % at)
    g.ok("$TIN=%d,1,800" % at)      # the relay closes again 0.8 s later
    time.sleep(0.5)
    st = state(g)
    ok &= check(st.startswith("Hold"), "$53=1: losing the signal while cutting -> %s" % st)
    time.sleep(0.5)
    g.write(b"~")
    time.sleep(0.3)
    st = state(g)
    ok &= check(st.startswith("Run") or st.startswith("Idle"), "back at speed, cycle start resumes: %s" % st)
    sync(g)

    g.ok("$53=2")
    g.ok("G91 G1 X-30 F600")
    time.sleep(0.6)
    g.write(b"$TIN=%d,0\n" % at)
    seen = alarm_reset(g)
    s = g.query()
    ok &= check(b"ALARM:14" in seen and s[1].startswith("Alarm"), "$53=2: alarm after a controlled stop at X=%.3f" % s[2])
    recover(g)

    # VFD fault while the spindle runs.
    g.ok("$TIN=%d,1" % at)
    g.ok("M3 S500")
    g.write(b"$TIN=%d,1\n" % flt)
    seen = alarm_reset(g)
    d = diag(g)
    ok &= check(b"ALARM:19" in seen and d["out"][0] & 0b11 == 0, "fault: ALARM:19, FWD/REV off: out=%s" % (d["out"],))
    r = g.cmd("$X")
    d = diag(g)
    ok &= check(r[-1] == b"error:43" and d["out"][0] & (1 << 10),
                "$X refused while in fault, VFD reset pulse on OUT11: %s out=%s" % (r[-1].decode(), d["out"]))
    g.ok("$TIN=%d,0" % flt)
    time.sleep(0.1)
    r = g.cmd("$X")
    ok &= check(r[-1] == b"ok", "fault gone: $X: %s" % r[-1].decode())
    g.ok("M5")
    g.ok("$52=0")
    g.ok("$53=2")
    g.ok("$TIN=0,0")
    return ok


def t_resume(g):
    say("\nhold resumed with the spindle stopped in it (the relay):")
    ok = True
    g.setup_bench(steps_per_mm=100, max_rate=6000, accel=1000)
    s = g.settings()
    at = int(s.get(77, 0)) or 11
    g.ok("$77=%d" % at)
    g.ok("$52=3000")
    g.ok("$TIN=%d,1" % at)
    time.sleep(0.05)
    g.ok("M3 S500")
    for close_ms in (2500, None):
        # Grbl reads no line in a hold: the relay is scheduled beforehand. It
        # opens once the spindle has stopped and closes at close_ms (never).
        g.ok("G91 G1 X%d F600" % (30 if close_ms else -30))
        t0 = time.monotonic()
        g.ok("$TIN=%d,0,800" % at)
        if close_ms:
            g.ok("$TIN=%d,1,%d" % (at, close_ms))
        g.write(b"!")
        time.sleep(0.3)
        g.write(b"\x9E")                     # spindle stop override
        while time.monotonic() - t0 < 1.2:
            time.sleep(0.02)
        x0 = g.query()[2]
        n0 = len(g.log)
        g.write(b"~")
        t_move = None
        while time.monotonic() - t0 < 5.0:
            s = g.query()
            if s and abs(s[2] - x0) > 0.005:
                t_move = time.monotonic() - t0
                break
            if s and s[1].startswith("Alarm"):
                break
            time.sleep(0.02)
        if close_ms:
            ok &= check(t_move is not None and abs(t_move - close_ms / 1000.0) < 0.15,
                        "cycle START at 1.2 s, the relay back at 2.5 s: X moves at %s s"
                        % ("%.2f" % t_move if t_move else "-"))
            sync(g)
        else:
            alarm_reset(g)
            s = g.query()
            ok &= check(t_move is None and b"ALARM:14" in g.log[n0:] and abs(s[2] - x0) < 0.005,
                        "the relay never back: ALARM:14 after $52, X stays at %.3f" % s[2])
            recover(g)
    g.ok("M5")
    g.ok("$52=0")
    g.ok("$TIN=0,0")
    return ok


def t_collet(g):
    say("\ncollet and M6:")
    ok = True
    g.setup_bench(steps_per_mm=100, max_rate=6000, accel=1000)
    g.ok("$52=0")
    g.ok("$51=300")
    g.ok("M3 S100")
    r = g.cmd("$COLLET=1")
    ok &= check(r[-1] == b"error:44", "spindle running: open refused: %s" % r[-1].decode())
    g.ok("M5")
    r = g.cmd("$COLLET=1")
    ok &= check(r[-1] == b"error:44", "right after M5 (run-down $51): refused: %s" % r[-1].decode())
    time.sleep(0.4)
    r = g.cmd("$COLLET=1")
    d = diag(g)
    ok &= check(r[-1] == b"ok" and d["out"][0] & (1 << 8), "after the run-down: open, OUT9 on: out=%s" % (d["out"],))
    r = g.cmd("M3 S100")
    ok &= check(r[-1] == b"error:39", "M3 while open: %s" % r[-1].decode())
    g.ok("$COLLET=0")
    g.ok("M3 S100")
    g.ok("M5")
    g.ok("G91 G1 X20 F600")
    time.sleep(0.3)
    r = g.cmd("$COLLET=1")
    ok &= check(r[-1] == b"error:44", "while moving: refused: %s" % r[-1].decode())
    sync(g)

    g.ok("M3 S200")
    g.write(b"T3 M6\n")
    seen = collect(g, b"[MSG:Tool change", timeout=2.0)
    msg = [l.decode() for l in seen if l.startswith(b"[MSG:")]
    time.sleep(0.2)
    st = state(g)
    d = diag(g) if False else None
    ok &= check(msg and "T3" in msg[-1] and st.startswith("Hold"), "M6: %s, state %s" % (msg, st))
    g.write(b"~")
    out = collect(g, b"ok", timeout=2.0)
    ok &= check(b"ok" in out and state(g).startswith("Idle"), "cycle start resumes: %s" % state(g))
    d = diag(g)
    ok &= check(d["out"][0] & 0b11 == 0, "the spindle stayed off after M6: out=%s" % (d["out"],))
    g.ok("G91 G0 X-20")
    sync(g)
    g.ok("G90")
    g.ok("$51=5000")
    return ok


def t_brake(g):
    say("\nbrake and enable sequence:")
    ok = True
    g.setup_bench(steps_per_mm=100, max_rate=6000, accel=1000)
    g.ok("$1=25")
    g.ok("$48=200")
    g.ok("$49=100")
    g.ok("$50=200")
    g.reset()                 # a new $1 counts from the next stop: give it one
    g.ok("$X")
    for mode in (1, 0):
        g.ok("$59=%d" % mode)
        time.sleep(0.6)
        d = diag(g)
        ok &= check(d["en"] == 0 and d["brake"] == 0, "$59=%d idle: drivers off, brake on (en=%d brake=%d)" % (mode, d["en"], d["brake"]))
        m0 = d["motor"][0]
        t0 = time.perf_counter()
        g.ok("G91 G1 X1 F600")
        t_en = t_br = t_step = None
        while time.perf_counter() - t0 < 2.0:
            d = diag(g)
            t = time.perf_counter() - t0
            if t_en is None and d["en"]:
                t_en = t
            if t_br is None and d["brake"]:
                t_br = t
            if t_step is None and d["motor"][0] != m0:
                t_step = t
                break
        if mode == 1:
            ok &= check(t_en is not None and t_br is not None and t_step is not None and
                        t_br - t_en > 0.15 and t_step - t_br > 0.07,
                        "enable %.3f s, brake released %.3f s, first step %.3f s" % (t_en or -1, t_br or -1, t_step or -1))
        else:
            ok &= check(t_br is None and t_step is not None and t_step - (t_en or 0) > 0.25,
                        "drive's own brake: BRAKE stays off, first step %.3f s after enable" % ((t_step or 0) - (t_en or 0)))
        sync(g)
        t0 = time.perf_counter()
        t_br = t_off = None
        while time.perf_counter() - t0 < 1.0:
            d = diag(g)
            t = time.perf_counter() - t0
            if t_br is None and not d["brake"]:
                t_br = t
            if t_off is None and not d["en"]:
                t_off = t
                break
        if mode == 1:
            ok &= check(t_br is not None and t_off is not None and t_off - t_br > 0.15,
                        "after motion: brake on at %.3f s, drivers off at %.3f s" % (t_br or -1, t_off or -1))
        else:
            ok &= check(t_off is not None, "after motion: drivers off at %.3f s" % (t_off or -1))
    g.ok("G90")
    g.ok("$59=1")
    for n in (48, 49, 50):
        g.ok("$%d=0" % n)
    return ok


def t_dirdelay(g):
    say("\ndirection setup delay ($56):")
    ok = True
    res = {}
    for us in (0, 35):
        g.setup_bench(steps_per_mm=1000, max_rate=60000, accel=10000, pulse_us=3)
        g.ok("$11=0.2")          # generous corners: the reversing axis is still fast there
        g.ok("$56=%d" % us)
        g.reset()                # clears the shortest-gap statistic
        g.ok("$X")
        m0 = diag(g)
        p0 = g.query()
        g.ok("G91")
        for i in range(20):
            g.ok("G1 X2 Y2 F30000")
            g.ok("G1 X-2 Y2 F30000")
        sync(g)
        g.ok("G90")
        d = diag(g)
        res[us] = d
        say("    $56=%d: delayed steps %d, shortest dir->step %.1f us, merged +%d"
            % (us, d["dir"][0], d["dir"][1] / 10, d["merged"] - m0["merged"]))
        ok &= check(d["merged"] == m0["merged"], "no merged pulses")
        s = g.query()
        dx, dy = s[2] - p0[2], s[3] - p0[3]
        ok &= check(abs(dx) < 0.0005 and abs(dy - 80) < 0.0005, "travel exact: X %+.4f Y %+.4f" % (dx, dy))
    ok &= check(res[0]["dir"][1] < 350, "without $56 the gap can be short: %.1f us" % (res[0]["dir"][1] / 10))
    ok &= check(res[35]["dir"][0] > 0 and res[35]["dir"][1] >= 350,
                "with $56=35: steps were held back, shortest gap %.1f us" % (res[35]["dir"][1] / 10))
    g.ok("$56=0")
    g.ok("$11=0.010")
    return ok


def t_drvpwr(g):
    say("\ndriver power ($94, $95, D53):")
    ok = True
    g.setup_bench(steps_per_mm=100, max_rate=6000, accel=1000)
    g.ok("$59=1")             # the BRAKE output works the brake: it must engage at the loss
    g.ok("$95=500")
    lines = lambda seen: [l.decode() for l in seen if not l.startswith(b"<")]

    # The bench pin has nothing on it, which reads as "no supply".
    d = diag(g)
    ok &= check(d["drv"][0] == 0 and state(g) == "Idle", "$94=0: no supply on the pin, nothing happens: drv=%s" % (d["drv"],))
    g.ok("$TIN=22,1")
    g.ok("$94=1")
    time.sleep(0.1)
    d = diag(g)
    ok &= check(d["drv"][0] == 3 and state(g) == "Idle", "$94=1 with the supply present: drv=%s, %s" % (d["drv"], state(g)))
    losses = d["drv"][1]

    # Lost during motion.
    g.write(b"G1 X50 F3000\n")
    time.sleep(0.4)
    s0 = g.query()
    g.write(b"$TIN=22,0\n")
    seen = alarm_reset(g)
    ok &= check(s0[1].startswith("Run") and b"ALARM:22" in seen, "lost while running: %s" % lines(seen))
    s1 = g.query()
    time.sleep(0.3)
    s2 = g.query()
    ok &= check(s2[1].startswith("Alarm") and s1[2] == s2[2] and 3 < s2[2] < 49,
                "Alarm, no step after it: X %.3f -> %.3f" % (s1[2], s2[2]))
    d = diag(g)
    ok &= check(d["en"] == 0 and d["brake"] == 0 and d["drv"] == (1, losses + 1),
                "drives off, brake on: en=%d brake=%d drv=%s" % (d["en"], d["brake"], d["drv"]))
    r = g.cmd("$X")
    ok &= check(r[-1] == b"error:50", "$X without supply: %s" % r[-1].decode())
    g.ok("$22=1")
    r = g.cmd("$H")
    g.ok("$22=0")
    ok &= check(r[-1] == b"error:50", "$H without supply: %s" % r[-1].decode())

    # Back: $X waits for $95.
    g.ok("$TIN=22,1")
    time.sleep(0.05)
    d = diag(g)
    r = g.cmd("$X")
    ok &= check(d["drv"][0] == 2 and r[-1] == b"error:50", "back, $95 not over: drv=%s, $X %s" % (d["drv"], r[-1].decode()))
    seen = collect(g, b"[MSG:Driver power on]", timeout=1.5)
    ok &= check(seen and seen[-1] == b"[MSG:Driver power on]", "after $95: %s" % lines(seen))
    r = g.cmd("$X")
    ok &= check(r[-1] == b"ok" and state(g) == "Idle", "$X: %s, %s" % (r[-1].decode(), state(g)))

    # Lost while idle, the drives holding ($1=255), the brake released.
    g.ok("$1=255")
    g.ok("G90 G1 X10 F3000")
    sync(g)
    d = diag(g)
    ok &= check(d["en"] == 1 and d["brake"] == 1, "idle, drives holding: en=%d brake=%d" % (d["en"], d["brake"]))
    g.write(b"$TIN=22,0\n")
    seen = alarm_reset(g)
    d = diag(g)
    ok &= check(b"ALARM:22" in seen and state(g) == "Alarm" and d["en"] == 0 and d["brake"] == 0,
                "lost while idle: %s, en=%d brake=%d" % (lines(seen), d["en"], d["brake"]))
    g.ok("$1=25")

    # Lost again while already in the alarm: reported, no reset needed.
    g.ok("$TIN=22,1")
    collect(g, b"[MSG:Driver power on]", timeout=1.5)
    g.write(b"$TIN=22,0\n")
    seen = collect(g, b"ALARM:", timeout=1.0)
    more = collect(g, b"Grbl", timeout=0.5)
    ok &= check(b"ALARM:22" in seen and not any(l.startswith(b"Grbl") for l in more),
                "lost while in the alarm: %s, no reset" % lines(seen + more))

    # Lost during homing: ALARM:22, not ALARM:6 (homing aborted by reset).
    g.ok("$TIN=22,1")
    collect(g, b"[MSG:Driver power on]", timeout=1.5)
    g.ok("$X")
    for n, v in [(22, 1), (23, 0), (24, 200), (25, 1000), (26, 50), (27, 1), (43, 0)]:
        g.ok("$%d=%s" % (n, v))
    for ch, ax in [(2, "X"), (4, "Y"), (6, "Z")]:
        g.ok("$TSW=%d,%s+40" % (ch, ax))
    g.ok("$TIN=22,0,600")      # Grbl reads no line while homing
    g.write(b"$H\n")
    seen = collect(g, b"ALARM:", timeout=10)
    ok &= check(b"ALARM:22" in seen, "lost while homing: %s" % lines(seen)[-2:])
    alarm_reset(g, timeout=1.0)
    g.ok("$TSW=0")
    g.ok("$22=0")

    # Switched on while the supply is missing: the scan finds it.
    g.ok("$94=0")
    g.ok("$X")
    g.write(b"$94=1\n")
    seen = alarm_reset(g)
    ok &= check(b"ALARM:22" in seen and state(g) == "Alarm", "$94=1 without supply: %s" % lines(seen))

    # $79 bit 21: the input inverted - an open contact means "supply present".
    g.ok("$79=2097152")
    seen = collect(g, b"[MSG:Driver power on]", timeout=1.5)
    r = g.cmd("$X")
    ok &= check(seen and seen[-1] == b"[MSG:Driver power on]" and r[-1] == b"ok",
                "$79 bit 21 with the pin open: %s, $X %s" % (lines(seen), r[-1].decode()))
    g.ok("$79=0")
    alarm_reset(g)

    # Not watched: the same missing supply is ignored again.
    g.ok("$94=0")
    r = g.cmd("$X")
    g.ok("G90 G1 X0 F3000")
    sync(g)
    ok &= check(r[-1] == b"ok" and state(g) == "Idle", "$94=0: $X %s, moves again (%s)" % (r[-1].decode(), state(g)))
    g.ok("$95=1000")
    g.ok("$TIN=0,0")
    return ok


TESTS = [("assign", t_assign), ("limits", t_limits), ("homing", t_homing), ("estop", t_estop),
         ("spindle", t_spindle), ("resume", t_resume), ("collet", t_collet), ("brake", t_brake), ("dirdelay", t_dirdelay),
         ("drvpwr", t_drvpwr)]


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
        g.reset()
        g.ok("$TIN=0,0")
        g.ok("$TSW=0")
        for name, fn in TESTS:
            if only and name not in only:
                continue
            try:
                results.append((name, fn(g)))
            except Exception as e:
                say("  FAIL  %s: %r" % (name, e))
                results.append((name, False))
                try:
                    recover(g)
                except Exception:
                    pass
    finally:
        try:
            g.reset()
            g.ok("$TIN=0,0")
            g.ok("$TSW=0")
            g.ok("$57=0")
            g.ok("$94=0")
            g.ok("$79=0")
        except Exception:
            pass
        g.close()
    say("\nsummary: " + ", ".join("%s %s" % (n, "ok" if r else "FAIL") for n, r in results))


if __name__ == "__main__":
    main()
