#!/usr/bin/env python3
"""A real Delta drive on the RS485 port (D17-D19, $33=1) - no motor needed.

test_vfd.py checks the firmware against a simulated drive; this one runs the
drive that is wired to the port and reports what it does. Spindle commands
only, no axis moves - except with --motion.

Before: the drive set for RS485 (README; VFD-E 02-00=3, 02-01=3, 09-00 = $34,
09-01 = $35, 09-04=3 for $36=1 8N2), $33=1 and $34-$36 matching. THE DRIVE
RUNS: no motor on it, or a motor that may turn freely.

  link      Idle polling for 3 s: link OK, remote=3, no failed transaction
  params    01-00 (the test uses $37 = 01-00) and the drive's 09-xx
  run       M3 at half speed: set point, ramp up, at speed; override +10 %
  speed     S at a quarter: the drive follows down
  reverse   M4: through zero to REV, at speed
  wait      $52: M3 at full speed, ok only once the drive is at speed
  stop      M5: the output ramps down to 0
  touch     the OVR page while idle ($TOUCH): START = M3 at half the range
            (S0), waits for the speed ($52); STOP = M5, also while it waits
  -- with --manual, you act on the drive while the script waits:
  keypad    STOP on the drive's keypad while it runs
  cable     the RS485 cable pulled while it runs, then plugged back
  -- with --motion, X moves 10 mm and back (G91, F60):
  resume    the move resumes only with the drive at speed ($52): after a
            feed hold with the spindle stopped in it (0x9E) and the cycle
            START; the same with the spindle started again (0x9E) first;
            after the safety door ($76, through $TIN); and with $52=0 after
            $54. Wants ramps long enough to see (01-09/01-10 several seconds).

$30, $31, $37, $52 and $54 are set for the test and put back afterwards.

    python test_vfd_drive.py [--port COM5] [--fmax 60] [--manual] [--motion] [--yes]
"""
import argparse
import time

from grbl_link import Grbl, say
from test_vfd import check, hz, state

RPM_MAX = 24000
RAMP_TIMEOUT = 60.0


def report(g):
    """$VFD as a dict. ALARM lines that arrive meanwhile only go to g.log."""
    g.write(b"$VFD\n")
    d = None
    t0 = time.monotonic()
    while time.monotonic() - t0 < 3.0:
        l = g.readline(timeout=1.0)
        if l is None:
            break
        if l.startswith(b"[VFD:") and b" link=" in l:
            d = dict(kv.split("=", 1) for kv in l[5:-1].decode().split()[1:])
        elif l == b"ok" and d:
            return d
        elif l.startswith(b"error:"):
            raise RuntimeError("$VFD -> %s" % l.decode())
    raise RuntimeError("no [VFD:] report")


def read_regs(g, reg, n=1):
    """$VFD=R: the register values, or None with the reason printed."""
    out = g.cmd("$VFD=R%04X,%d" % (reg, n))
    for l in out:
        if l.startswith(b"[VFD:R"):
            return [int(x, 16) for x in l.split(b"=", 1)[1][:-1].split(b",")]
    say("  %04XH not readable: %s" % (reg, b" ".join(out).decode()))
    return None


def follow(g, pred, timeout=RAMP_TIMEOUT):
    """Polls $VFD until pred(v): (last report, seconds, [(t, output Hz)])."""
    t0 = time.perf_counter()
    samples = []
    while True:
        v = report(g)
        t = time.perf_counter() - t0
        samples.append((t, hz(v["drive"])[2]))
        if pred(v) or t > timeout:
            return v, t, samples
        time.sleep(0.1)


def rate(samples):
    """Hz/s while the output was moving."""
    f0, f1 = samples[0][1], samples[-1][1]
    moving = [s for s in samples if s[1] != f0]
    if not moving or f1 == f0:
        return 0.0
    t_start = samples[samples.index(moving[0]) - 1][0]
    t_end = next(t for t, f in samples if f == f1)
    return abs(f1 - f0) / max(t_end - t_start, 0.1)


def near(a, b):
    return abs(a - b) < 0.011


def alarms(g, n0):
    return [l.decode() for l in g.log[n0:] if l.startswith(b"ALARM:")]


class Ctx:
    def __init__(self, g, fmax):
        self.g, self.fmax = g, fmax

    def f(self, rpm):
        return round(rpm / RPM_MAX * self.fmax, 2)


def spin(c, line, direction, rpm):
    """Sends line, follows the drive to at speed, checks set point and direction."""
    g = c.g
    f = c.f(rpm)
    n0 = len(g.log)
    g.ok(line)
    v, dt, s = follow(g, lambda v: v["at"] == "1" or v["trouble"] != "0")
    d = hz(v["drive"])
    ok = check(v["want"] == "%s,%.2f" % (direction, f), "%s: want %s (expected %s,%.2f)" % (line, v["want"], direction, f))
    ok &= check(d[0] == direction and near(d[1], f), "  the drive took it: %s" % v["drive"])
    ok &= check(v["at"] == "1", "  at speed after %.1f s (%.1f Hz/s), output %.2f Hz" % (dt, rate(s), d[2]))
    ok &= check(v["trouble"] == "0" and not alarms(g, n0), "  no trouble, no alarm (trouble=%s %s)" % (v["trouble"], alarms(g, n0)))
    return ok


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def t_link(c):
    say("\nlink while idle:")
    g = c.g
    v0 = report(g)
    time.sleep(3.0)
    v = report(g)
    n_ok = int(v["ok"]) - int(v0["ok"])
    n_fail = int(v["fail"]) - int(v0["fail"])
    n_retry = int(v["retry"]) - int(v0["retry"])
    ok = check(v["link"] == "OK" and v["err"] == "0", "link=%s err=%s st=%s" % (v["link"], v["err"], v["st"]))
    ok &= check(v["remote"] == "3", "  frequency and run command from RS485 (remote=%s)" % v["remote"])
    ok &= check(n_ok >= 25 and n_fail == 0, "  3 s: %d transactions, %d retries, %d failed" % (n_ok, n_retry, n_fail))
    regs = read_regs(g, 0x2100, 4)
    if regs:
        ok &= check(regs[0] & 0xFF == int(v["err"]) and regs[1] == int(v["st"], 16),
                    "  $VFD=R2100,4 -> %s" % ",".join("%04X" % r for r in regs))
    return ok


def t_params(c):
    say("\ndrive parameters:")
    g = c.g
    r = read_regs(g, 0x0900, 5)
    if r:
        say("  09-00..09-04 = %s (address, baud code, on loss, timeout 0.1 s, frame)" % ",".join(str(x) for x in r))
    r = read_regs(g, 0x0200, 2)
    if r:
        say("  02-00, 02-01 = %d, %d (VFD-E: 3, 3 = RS485)" % (r[0], r[1]))
    return check(c.fmax > 0, "test with $37 = %.2f Hz, $30 = %d" % (c.fmax, RPM_MAX))


def t_run(c):
    say("\nM3 at half speed, override:")
    g = c.g
    ok = spin(c, "M3 S%d" % (RPM_MAX // 2), "FWD", RPM_MAX // 2)
    f = c.f(RPM_MAX // 2 * 1.1)
    g.write(b"\x9A")                                 # spindle override +10 %
    v, dt, s = follow(g, lambda v: near(hz(v["drive"])[1], f), timeout=3.0)
    ok &= check(v["want"] == "FWD,%.2f" % f and near(hz(v["drive"])[1], f),
                "override +10 %%: want %s, drive %s" % (v["want"], v["drive"]))
    v, dt, s = follow(g, lambda v: v["at"] == "1")
    ok &= check(v["at"] == "1", "  at speed after %.1f s" % dt)
    g.write(b"\x99")                                 # back to 100 %
    f = c.f(RPM_MAX // 2)
    v, dt, s = follow(g, lambda v: near(hz(v["drive"])[1], f) and v["at"] == "1")
    ok &= check(near(hz(v["drive"])[1], f) and v["at"] == "1", "override 100 %%: drive %s, at=%s" % (v["drive"], v["at"]))
    return ok


def t_speed(c):
    say("\nS at a quarter:")
    return spin(c, "S%d" % (RPM_MAX // 4), "FWD", RPM_MAX // 4)


def t_reverse(c):
    say("\nM4, through zero:")
    g = c.g
    n0 = len(g.log)
    g.ok("M4")
    v, dt, s = follow(g, lambda v: v["at"] == "1" or v["trouble"] != "0")
    f = c.f(RPM_MAX // 4)
    lowest = min(o for _, o in s)
    ok = check(v["want"] == "REV,%.2f" % f and hz(v["drive"])[0] == "REV" and v["at"] == "1",
               "M4: want %s, drive %s, at speed after %.1f s" % (v["want"], v["drive"], dt))
    say("        lowest output seen %.2f Hz (a fast ramp passes zero between two polls)" % lowest)
    ok &= check(v["trouble"] == "0" and not alarms(g, n0), "  no trouble, no alarm")
    return ok


def t_wait(c):
    say("\n$52: ok only once the drive is at speed:")
    g = c.g
    g.ok("$52=60000")
    ok = True
    for line, direction, rpm in (("M4 S%d" % (RPM_MAX // 2), "REV", RPM_MAX // 2),
                                 ("M3", "FWD", RPM_MAX // 2),          # reversed at the same speed
                                 ("M3 S%d" % RPM_MAX, "FWD", RPM_MAX)):
        n0 = len(g.log)
        t0 = time.perf_counter()
        out = g.cmd(line, timeout=RAMP_TIMEOUT + 10)
        dt = time.perf_counter() - t0
        v = report(g)
        d = hz(v["drive"])
        f = c.f(rpm)
        ok &= check(out[-1] == b"ok" and d[0] == direction and v["at"] == "1" and abs(d[2] - f) <= max(0.5, f * 0.02),
                    "%s: %s after %.2f s, then drive %s at=%s" % (line, out[-1].decode(), dt, v["drive"], v["at"]))
        ok &= check(not alarms(g, n0), "  no alarm %s" % alarms(g, n0))
    g.ok("$52=0")
    return ok


def t_stop(c):
    say("\nM5:")
    g = c.g
    g.ok("M5")
    v, dt, s = follow(g, lambda v: hz(v["drive"])[0] == "STOP" and hz(v["drive"])[2] == 0.0)
    return check(hz(v["drive"])[0] == "STOP" and hz(v["drive"])[2] == 0.0 and v["want"].startswith("STOP"),
                 "stopped, output 0 after %.1f s (%.1f Hz/s): drive %s" % (dt, rate(s), v["drive"]))


def run_half(c):
    g = c.g
    g.ok("M3 S%d" % (RPM_MAX // 2))
    v, dt, s = follow(g, lambda v: v["at"] == "1")
    return v["at"] == "1"


def recover(c, timeout=15.0):
    """Spindle off, fault reset ($X sends it), $X once the drive stands."""
    g = c.g
    g.cmd("M5")
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < timeout:
        v = report(g)
        if state(g).startswith("Alarm") or v["err"] != "0":
            g.cmd("$X")
        if v["err"] == "0" and v["trouble"] == "0" and not state(g).startswith("Alarm"):
            return True, v
        time.sleep(0.3)
    return False, report(g)


def t_keypad(c):
    say("\nSTOP on the drive's keypad:")
    g = c.g
    if not run_half(c):
        return check(False, "the drive did not get to speed")
    n0 = len(g.log)
    say("  >>> press STOP on the drive's keypad now (waiting up to 60 s)")
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 60.0:
        v = report(g)
        if hz(v["drive"])[0] != "FWD" or v["trouble"] != "0" or alarms(g, n0):
            break
        time.sleep(0.1)
    else:
        return check(False, "nothing happened")
    t1 = time.perf_counter()
    timeline = []
    while time.perf_counter() - t1 < 4.0:
        v = report(g)
        timeline.append((time.perf_counter() - t1, v["drive"], v["trouble"], state(g)))
        time.sleep(0.2)
    for t, d, tr, st in timeline[::3]:
        say("    %4.1f s  drive %-22s trouble=%s %s" % (t, d, tr, st))
    restarted = hz(timeline[-1][1])[0] == "FWD" and not alarms(g, n0)
    ok = check(not restarted, "  the drive stayed stopped (restarted by Grbl's repeated run: %s)" % restarted)
    ok &= check(any(a == "ALARM:23" for a in alarms(g, n0)), "  ALARM:23 (%s)" % alarms(g, n0))
    rec, v = recover(c)
    ok &= check(rec, "  back: M5, $X ok (drive %s)" % v["drive"])
    return ok


def t_cable(c):
    say("\nRS485 cable pulled while running:")
    g = c.g
    if not run_half(c):
        return check(False, "the drive did not get to speed")
    n0 = len(g.log)
    say("  >>> pull the RS485 cable now (waiting up to 60 s)")
    t0 = time.perf_counter()
    v = None
    while time.perf_counter() - t0 < 60.0:
        v = report(g)
        if v["link"] == "LOST" or alarms(g, n0):
            break
        time.sleep(0.1)
    t_lost = time.perf_counter()
    while time.perf_counter() - t_lost < 3.0 and "ALARM:23" not in alarms(g, n0):
        time.sleep(0.1)
        state(g)
    ok = check(v["link"] == "LOST" and "ALARM:23" in alarms(g, n0),
               "link %s, %s" % (v["link"], alarms(g, n0)))
    r = g.cmd("$X")[-1]
    ok &= check(r == b"error:51", "  $X while the drive may still run: %s" % r.decode())
    say("  >>> plug the cable back in (waiting up to 60 s)")
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 60.0:
        v = report(g)
        if v["link"] == "OK":
            break
        time.sleep(0.2)
    ok &= check(v["link"] == "OK", "  link %s, the drive reports err=%s drive %s" % (v["link"], v["err"], v["drive"]))
    rec, v = recover(c)
    ok &= check(rec, "  back: stopped, $X ok (drive %s)" % v["drive"])
    return ok


def hold_status(g):
    """State and X from ? - the only thing Grbl answers while it holds."""
    s = g.query()
    return (s[1], s[2]) if s else ("", 0.0)


def wait_state(g, prefix, timeout=10.0):
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < timeout:
        st, x = hold_status(g)
        if st.startswith(prefix):
            return True
        time.sleep(0.05)
    return False


def resume_case(c, what, sign, stop, before_start, ramp_s, wait_s=None):
    """One move held with its spindle stopped, then resumed: returns ok.

    stop() stops it (and returns the state prefix to wait for before the
    resume), before_start() runs right before the cycle START. wait_s: the
    resume is expected after that long (the $54 case); otherwise only with
    the drive at speed."""
    g = c.g
    f = c.f(RPM_MAX // 2)
    tol = max(0.5, f * 0.02)
    g.ok("G91 G1 X%d F60" % (10 * sign))
    time.sleep(1.0)
    ready = stop()
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < ramp_s + 15.0:   # the spindle runs down meanwhile
        st, x0 = hold_status(g)
        if st.startswith(ready) and time.perf_counter() - t0 > ramp_s + 1.0:
            break
        time.sleep(0.05)
    else:
        return check(False, "%s: never %s (%s)" % (what, ready, st))
    before_start()
    t1 = time.perf_counter()
    g.write(b"~")
    t_move = None
    while time.perf_counter() - t1 < ramp_s + 10.0:
        st, x = hold_status(g)
        if abs(x - x0) > 0.005:
            t_move = time.perf_counter() - t1
            break
        time.sleep(0.02)
    if t_move is None:
        g.write(b"~")
        g.wait_idle(timeout=30)
        return check(False, "%s: the move did not resume (%s)" % (what, st))
    v = report(g)                                     # running again: Grbl reads lines
    out = hz(v["drive"])[2]
    g.wait_idle(timeout=30)
    if wait_s is not None:
        return check(abs(t_move - wait_s) < 0.3,
                     "%s: X moves after %.2f s (expected %.1f s), the drive at %.2f Hz" % (what, t_move, wait_s, out))
    return check(abs(out - f) <= tol,
                 "%s: X moves after %.2f s, the drive then at %.2f of %.2f Hz" % (what, t_move, out, f))


def t_resume(c):
    say("\nresume with the spindle stopped meanwhile: does the move wait for it?")
    g = c.g
    door = int(g.settings().get(76, 0))
    g.ok("$52=30000")
    t0 = time.perf_counter()
    g.ok("M3 S%d" % (RPM_MAX // 2), timeout=40)
    ramp_s = time.perf_counter() - t0
    v = report(g)
    ok = check(v["at"] == "1", "M3 S%d with $52: ok after %.1f s, at speed (drive %s)" % (RPM_MAX // 2, ramp_s, v["drive"]))

    def hold_and_stop():
        g.write(b"!")
        wait_state(g, "Hold:0")
        g.write(b"\x9E")                              # spindle stop override
        return "Hold:0"

    def nothing():
        pass

    def spindle_start():
        g.write(b"\x9E")                              # the spindle START of the OVR page
        time.sleep(0.2)

    ok &= resume_case(c, "hold, spindle STOP, cycle START", 1, hold_and_stop, nothing, ramp_s)
    ok &= resume_case(c, "hold, spindle STOP, spindle START, cycle START", -1, hold_and_stop, spindle_start, ramp_s)
    if door:
        def door_open():
            g.ok("$TIN=%d,1" % door)                  # open: the spindle stops
            g.ok("$TIN=%d,0,%d" % (door, int((ramp_s + 2.0) * 1000)))   # closed again later
            return "Door:0"
        ok &= resume_case(c, "safety door (IN%d) opened and closed, cycle START" % door, 1, door_open, nothing, ramp_s)
        g.ok("$TIN=0,0")
    else:
        say("  (no door input assigned, $76=0: door skipped)")
    g.ok("$52=0")
    g.ok("$54=3000")
    ok &= resume_case(c, "$52=0, $54=3000: hold, spindle STOP, cycle START", -1 if door else 1,
                      hold_and_stop, nothing, ramp_s, wait_s=3.0)
    g.ok("$54=0")
    g.ok("G90")
    return ok


TOUCH_OVR, TOUCH_DRO = (287, 444), (59, 444)          # tabs (test_touch.py)
TOUCH_SPIN_STOP, TOUCH_SPIN_START = (507, 254), (631, 254)


def gc_spindle(g):
    """The spindle word of $G; waits while Grbl is busy (an M3 waiting for the speed)."""
    for l in g.cmd("$G", timeout=RAMP_TIMEOUT):
        if l.startswith(b"[GC:"):
            return next((w for w in l[4:-1].decode().split() if w in ("M3", "M4", "M5")), "")
    return ""


def t_touch(c):
    say("\nthe OVR page's spindle START / STOP while idle ($52):")
    g = c.g
    if g.cmd("$TOUCH=%d,%d,100" % TOUCH_OVR)[-1] != b"ok":
        say("  (no display: skipped)")
        return True
    time.sleep(0.4)
    g.ok("$52=30000")
    g.ok("M5 S0")
    t0 = time.perf_counter()
    g.ok("$TOUCH=%d,%d,700" % TOUCH_SPIN_START)            # fires after 0.5 s
    time.sleep(0.6)
    sp = gc_spindle(g)                                     # answered once M3 is through
    dt = time.perf_counter() - t0
    v = report(g)
    ok = check(sp == "M3" and v["at"] == "1" and v["want"] == "FWD,%.2f" % c.f(RPM_MAX // 2),
               "START held, S0: M3 at half of $31-$30, at speed after %.1f s (want %s, drive %s)"
               % (dt, v["want"], v["drive"]))
    g.ok("$TOUCH=%d,%d,100" % TOUCH_SPIN_STOP)
    time.sleep(0.4)
    v = report(g)
    ok &= check(gc_spindle(g) == "M5" and v["want"].startswith("STOP"), "STOP: M5, the drive told to stop")
    follow(g, lambda v: hz(v["drive"])[2] == 0.0)
    # Grbl reads no line while the M3 waits: the STOP is queued beforehand,
    # 2 s from now - 1.5 s into the spin-up.
    g.ok("$TOUCH=%d,%d,700" % TOUCH_SPIN_START)
    g.ok("$TOUCH=%d,%d,100,2000" % TOUCH_SPIN_STOP)
    t1 = time.perf_counter() + 2.0
    time.sleep(0.6)                                        # the START has fired: $G waits for its M3
    sp = ""
    while time.perf_counter() - t1 < 10.0 and sp != "M5":  # the M5 runs right after the M3 gave up
        sp = gc_spindle(g)
    dt = time.perf_counter() - t1
    v = report(g)
    ok &= check(sp == "M5" and dt < 1.0 and v["want"].startswith("STOP"),
                "STOP while the START still waits: M5 after %.2f s, the drive at %.2f Hz told to stop"
                % (dt, hz(v["drive"])[2]))
    follow(g, lambda v: hz(v["drive"])[2] == 0.0)
    g.ok("$52=0")
    g.ok("$TOUCH=%d,%d,100" % TOUCH_DRO)
    return ok


TESTS = [("link", t_link), ("params", t_params), ("run", t_run), ("speed", t_speed),
         ("reverse", t_reverse), ("wait", t_wait), ("stop", t_stop), ("touch", t_touch)]
MANUAL = [("keypad", t_keypad), ("cable", t_cable)]
MOTION = [("resume", t_resume)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--only")
    ap.add_argument("--fmax", type=float, help="the drive's highest frequency, Hz (default: read 01-00)")
    ap.add_argument("--manual", action="store_true", help="also the keypad and cable tests")
    ap.add_argument("--motion", action="store_true", help="also the resume test, which moves X")
    ap.add_argument("--yes", action="store_true", help="do not ask before running the drive")
    a = ap.parse_args()

    g = Grbl(a.port)
    g.drain()
    keep = g.settings()
    if int(keep.get(33, 0)) != 1:
        say("$33=%d: no Delta drive on RS485 configured" % keep.get(33, 0))
        return 1
    say("Grbl on %s, drive: Delta at address %d, %d baud, frame $36=%d"
        % (g.port, keep[34], keep[35], keep[36]))
    if not a.yes:
        input("The drive will RUN (no motor, or one that may turn freely). Enter to go on, Ctrl-C to stop: ")
    results = {}
    try:
        if state(g).startswith("Alarm"):
            say("  Alarm: $X -> %s" % g.cmd("$X")[-1].decode())
        g.ok("M5")
        fmax = a.fmax
        if not fmax:
            r = read_regs(g, 0x0100)
            fmax = r[0] / 100.0 if r and 10.0 <= r[0] / 100.0 <= 655.35 else 0
            say("01-00 = %s -> %.2f Hz" % (r, fmax))
        if not fmax:
            say("highest frequency unknown: give --fmax")
            return 1
        for n, v in [(30, RPM_MAX), (31, 0), (37, "%.2f" % fmax), (52, 0), (54, 0)]:
            g.ok("$%d=%s" % (n, v))
        c = Ctx(g, fmax)
        v0 = report(g)
        tests = TESTS + (MANUAL if a.manual else []) + (MOTION if a.motion else [])
        only = a.only.split(",") if a.only else [n for n, _ in tests]
        for name, fn in tests:
            if name in only:
                results[name] = fn(c)
        v = report(g)
        exc = int(v["exc"].split("/")[0]) - int(v0["exc"].split("/")[0])
        say("\nRS485: %d transactions, %d retries, %d failed, %d exceptions"
            % (int(v["ok"]) - int(v0["ok"]), int(v["retry"]) - int(v0["retry"]),
               int(v["fail"]) - int(v0["fail"]), exc))
        say("\n" + ", ".join("%s %s" % (n, "ok" if r else "FAIL") for n, r in results.items()))
        return 0 if results and all(results.values()) else 1
    finally:
        try:
            g.cmd("M5")
            for n in (30, 31, 37, 52, 54):
                if n in keep:
                    r = g.cmd("$%d=%s" % (n, ("%g" % keep[n])))
                    if r[-1] != b"ok":
                        say("  could not restore $%d=%g: %s" % (n, keep[n], r[-1].decode()))
        finally:
            g.close()


if __name__ == "__main__":
    raise SystemExit(main())
