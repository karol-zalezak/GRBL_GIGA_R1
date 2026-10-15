#!/usr/bin/env python3
"""Touch-screen tests for Grbl-Giga on the Arduino GIGA R1 with the Display Shield.

No finger needed: `$TOUCH=x,y,ms` makes the M4 see a press at screen pixel
(x, y) for ms milliseconds, through the same code path as the GT911. The
script then checks what Grbl did, with `?`, `$#` and `$I`:

  realtime   HOLD / START / RESET / UNLOCK in the right column
  jog        step jogs on every axis, a continuous jog that stops on release
  zero       hold-to-zero one axis and all axes; a short tap does nothing
  override   feed / rapid / spindle overrides, auto-repeat, coolant toggles
  aux        hold-to-toggle AUX outputs on the I/O page
  silent     touch actions answer on the display, never with ok/error on USB
  stale      an action that waits through a dwell is dropped, not run late
  refused    jog in alarm is refused on the screen and does not move
  settings   the $$ page: keypad, bit switches and choices write $n=value;
             an invalid value is refused and leaves the setting alone

Also saves a screenshot of every page into docs/ with --shots.

    python test_touch.py [--port COM5] [--shots]
"""
import argparse
import os
import re
import subprocess
import sys
import time

from grbl_link import Grbl, say

DIAG_RE = re.compile(rb"\[MSG:\S+ .*motor=(-?\d+),(-?\d+),(-?\d+),(-?\d+) in=(\d+)/\d+ out=(\d+)/\d+.* touch=(\d+)/(\d+)@(\d+),(\d+)/(\d+)x(\d+) rgb=(\d+)")
OV_RE = re.compile(r"Ov:(\d+),(\d+),(\d+)")
HERE = os.path.dirname(os.path.abspath(__file__))

# Screen geometry (landscape pixels), centres of the buttons; see m4/gui.cpp.
TAB = {"dro": (59, 444), "jog": (173, 444), "ovr": (287, 444), "io": (401, 444), "set": (515, 444), "file": (629, 444)}
HOLD, START, RESET, UNLOCK = (746, 60), (746, 178), (746, 296), (746, 414)
STEP = [(48 + 94 * k, 90) for k in range(5)]            # 0.01 0.1 1 10 CONT
SPEED = (582, 90)
def pad(c, r): return (48 + 94 * c, 163 + 84 * r)
JOG = {"X+": pad(2, 1), "X-": pad(0, 1), "Y+": pad(1, 0), "Y-": pad(1, 2),
       "Z+": pad(3, 0), "Z-": pad(3, 2), "A+": pad(4, 0), "A-": pad(4, 2)}
HOME, ZERO_ALL = pad(1, 1), pad(0, 2)
ZERO = [(625, 93 + 66 * i) for i in range(4)]
FEED = [(210 + 107 * i, 98) for i in range(5)]           # -10 -1 100 +1 +10
RAPID = [(246 + 179 * i, 176) for i in range(3)]         # 25 50 100
SPIN = [(224 + 134 * i, 254) for i in range(4)]          # -10 100 +10 STOP
FLOOD, MIST = (291, 332), (559, 332)
AUX = [(37 + 70 * i, 327) for i in range(6)]
COLLET, SRV_RST, VFD_RST = (469, 327), (558, 327), (646, 327)
# FILE page: listing rows, START (hold 1 s), STOP of a running program (hold).
FROW = [(192, 87 + 52 * i) for i in range(5)]
FPREV, FNEXT = (429, 125), (429, 259)
FSTART, FSTOP = (585, 274), (525, 317)
# $$ page: list rows, scroll buttons, and the editor's keypad / options.
SROW = [(300, 87 + 52 * i) for i in range(6)]
SPREV, SNEXT = (643, 138), (643, 298)
def key(c, r): return (381 + 88 * c, 98 + 80 * r)
KEY = {"7": key(0, 0), "8": key(1, 0), "9": key(2, 0), "DEL": key(3, 0),
       "4": key(0, 1), "5": key(1, 1), "6": key(2, 1), "CLR": key(3, 1),
       "1": key(0, 2), "2": key(1, 2), "3": key(2, 2), ".": key(3, 2),
       "0": key(0, 3), "CANCEL": key(1, 3), "SAVE": (601, 338)}
OPT4 = [key(k, 0) for k in range(4)]                  # bit switches X Y Z A
OPT2 = [(469, 98), (645, 98), (469, 178), (645, 178)]  # choices, two per row
OPT22 = [(363 + 50 * (k % 7), 98 + 80 * (k // 7)) for k in range(22)]   # $79: IN1-IN20, STOP, PWR
SETTING_ORDER = [0, 1, 2, 3, 4, 10, 11, 12, 13, 20, 21, 22, 23, 24, 25, 26, 27,
                 30, 31, 32, 40, 41, 42, 43, 46] + list(range(48, 99)) + \
                [100, 101, 102, 103, 110, 111, 112, 113, 120, 121, 122, 123, 130, 131, 132, 133]


def check(cond, what):
    cond = bool(cond)
    say(("  ok    " if cond else "  FAIL  ") + what)
    return cond


def diag(g):
    for l in g.ok("$I"):
        m = DIAG_RE.match(l)
        if m:
            return tuple(int(x) for x in m.groups())
    raise RuntimeError("no diag/touch line in $I")


def touch(g, xy, ms=100, settle=0.15):
    """Presses (x, y) for ms milliseconds and waits until the finger is up."""
    g.ok("$TOUCH=%d,%d,%d" % (xy[0], xy[1], ms))
    time.sleep(ms / 1000.0 + settle)


def sync(g):
    g.ok("G4 P0", timeout=60)


def status(g):
    s = g.query()
    if not s:
        raise RuntimeError("no status")
    return s


def pos(g):
    sync(g)
    s = g.wait_idle()
    return [s[2], s[3], s[4], s[6]]


def ovr(g):
    for _ in range(20):            # Ov: is only in some status reports
        s = status(g)
        m = OV_RE.search(s[5])
        if m:
            return tuple(int(x) for x in m.groups())
        time.sleep(0.05)
    raise RuntimeError("no Ov: field")


def offsets(g):
    """{'G54': [x, y, z, a], 'G92': [...]} from $#."""
    d = {}
    for l in g.ok("$#"):
        m = re.match(rb"\[(G5[4-9]|G92):([-0-9.,]+)\]", l)
        if m:
            d[m.group(1).decode()] = [float(v) for v in m.group(2).split(b",")]
    return d


def unsolicited(g):
    """Lines that arrived without being asked for; ok/error would confuse a sender."""
    return [l for l in g.drain(0.2) if l == b"ok" or l.startswith(b"error")]


def screenshot(g, name):
    tool = os.path.join(HERE, "..", "tools", "screenshot.py")
    out = os.path.join(HERE, "..", "docs", name)
    g.close()
    subprocess.run([sys.executable, tool, "--port", g.port, out], check=True)
    g.__init__(g.port)


def near(a, b, tol=0.002):
    return abs(a - b) <= tol


def t_realtime(g):
    say("\nHOLD / START / RESET / UNLOCK:")
    ok = True
    g.ok("G1 X200 F600")
    time.sleep(0.4)
    # Grbl reads no lines during a feed hold, so START is queued beforehand.
    g.ok("$TOUCH=%d,%d,100" % HOLD)
    g.ok("$TOUCH=%d,%d,100,700" % START)
    time.sleep(0.5)
    s = status(g)
    ok &= check(s[1].startswith("Hold"), "HOLD during a move: %s" % s[1])
    time.sleep(0.6)
    s = status(g)
    ok &= check(s[1].startswith("Run"), "START resumes: %s" % s[1])
    touch(g, RESET)
    lines = g.drain(1.0)
    s = status(g)
    ok &= check(s[1].startswith("Alarm") and any(l.startswith(b"ALARM:3") for l in lines),
                "RESET while moving: ALARM:3, %s" % s[1])
    touch(g, UNLOCK, ms=200)
    ok &= check(status(g)[1].startswith("Alarm"), "UNLOCK tapped briefly: still Alarm")
    touch(g, UNLOCK, ms=800)
    s = status(g)
    ok &= check(s[1].startswith("Idle"), "UNLOCK held: %s" % s[1])
    g.ok("G90 G0 X0")
    sync(g)
    return ok


def t_jog(g):
    say("\njog page:")
    ok = True
    touch(g, TAB["jog"])
    p0 = pos(g)
    touch(g, STEP[2])                   # 1 mm
    for k, (axis, d) in enumerate((("X+", 1), ("Y-", -1), ("Z+", 1), ("A-", -1))):
        touch(g, JOG[axis])
        p = pos(g)
        ok &= check(near(p[k] - p0[k], d), "%s step 1 mm: moved %.3f" % (axis, p[k] - p0[k]))
    touch(g, STEP[0])                   # 0.01 mm
    for _ in range(3):
        touch(g, JOG["X-"])
    p = pos(g)
    ok &= check(near(p[0] - p0[0], 1 - 0.03), "3 x X- 0.01 mm: X at %.3f" % (p[0] - p0[0]))

    # Continuous: 25 % of $110 = 1500 mm/min = 25 mm/s while held for 1 s.
    touch(g, STEP[4])
    a = pos(g)
    t0 = time.monotonic()
    touch(g, JOG["Y+"], ms=1000, settle=0.0)
    t_rel = time.monotonic()
    b = None
    stopped_after = None
    while time.monotonic() - t_rel < 3:
        s = status(g)
        if s[1].startswith("Idle"):
            stopped_after = time.monotonic() - t_rel
            b = [s[2], s[3], s[4], s[6]]
            break
        time.sleep(0.02)
    ok &= check(stopped_after is not None and stopped_after < 0.4,
                "continuous Y+ stops after release (Idle %.2f s after)" % (stopped_after or -1))
    dy = (b[1] - a[1]) if b else 0
    ok &= check(18 < dy < 30, "held 1 s at 25 mm/s: Y moved %.2f mm" % dy)
    time.sleep(0.5)
    c = pos(g)
    ok &= check(b and near(c[1], b[1]), "and stays stopped (%.3f)" % c[1])

    # Speed button cycles 5 -> 10 -> 25 -> 50 -> 100 %.
    touch(g, SPEED)
    touch(g, SPEED)                     # 25 -> 50 -> 100
    a = pos(g)
    touch(g, JOG["X-"], ms=500, settle=0.0)
    time.sleep(0.6)
    b = pos(g)
    dx = a[0] - b[0]
    # Grbl stops ~90 ms after the cancel: the 9 prepared 10 ms step segments
    # (SEGMENT_BUFFER_SIZE) run out first, as for any feed hold.
    ok &= check(50 < dx < 66, "at 100 %% (100 mm/s) held 0.5 s: X moved %.1f mm" % dx)
    for _ in range(3):
        touch(g, SPEED)                 # 100 -> 5 -> 10 -> 25
    touch(g, STEP[2])
    g.ok("G90 G0 X0 Y0 Z0 A0")
    sync(g)
    bad = unsolicited(g)
    ok &= check(not bad, "no ok/error on USB for touch actions: %s" % bad)
    return ok


def t_zero(g):
    say("\nzero buttons:")
    ok = True
    g.ok("G10 L2 P1 X0 Y0 Z0 A0")
    g.ok("G0 X12.5 Y-3 Z4 A7")
    sync(g)
    touch(g, TAB["dro"])
    touch(g, ZERO[1], ms=300)
    o = offsets(g)
    ok &= check(near(o["G54"][1], 0), "Y=0 tapped briefly: G54 Y unchanged (%.3f)" % o["G54"][1])
    touch(g, ZERO[0], ms=1100)
    o = offsets(g)
    ok &= check(near(o["G54"][0], 12.5) and near(o["G54"][1], 0), "X=0 held: G54 X = 12.5 (%s)" % o["G54"])
    touch(g, TAB["jog"])
    touch(g, ZERO_ALL, ms=1300)
    o = offsets(g)
    ok &= check(all(near(a, b) for a, b in zip(o["G54"], [12.5, -3, 4, 7])),
                "ZERO all held: G54 = 12.5,-3,4,7 (%s)" % o["G54"])
    g.ok("G10 L2 P1 X0 Y0 Z0 A0")
    g.ok("G0 X0 Y0 Z0 A0")
    sync(g)
    bad = unsolicited(g)
    ok &= check(not bad, "no ok/error on USB: %s" % bad)
    return ok


def t_override(g):
    say("\noverride page:")
    ok = True
    touch(g, TAB["ovr"])
    touch(g, FEED[4])
    ok &= check(ovr(g)[0] == 110, "FEED +10: %s" % (ovr(g),))
    touch(g, FEED[0])
    touch(g, FEED[0])
    ok &= check(ovr(g)[0] == 90, "FEED -10 twice: %s" % (ovr(g),))
    touch(g, FEED[2])
    ok &= check(ovr(g)[0] == 100, "FEED 100%%: %s" % (ovr(g),))
    touch(g, FEED[3], ms=1000)          # +1, then repeats every 150 ms after 0.5 s
    f = ovr(g)[0]
    ok &= check(103 <= f <= 106, "FEED +1 held 1 s repeats: %d" % f)
    touch(g, FEED[2])
    touch(g, RAPID[0])
    ok &= check(ovr(g)[1] == 25, "RAPID 25%%: %s" % (ovr(g),))
    touch(g, RAPID[2])
    touch(g, SPIN[2])
    ok &= check(ovr(g)[2] == 110, "SPINDLE +10: %s" % (ovr(g),))
    touch(g, SPIN[1])
    ok &= check(ovr(g) == (100, 100, 100), "all back to 100: %s" % (ovr(g),))
    touch(g, FLOOD)
    out = diag(g)[5]
    ok &= check(out & 0b100, "FLOOD on: OUT3 (%s)" % bin(out))
    touch(g, MIST)
    out = diag(g)[5]
    ok &= check(out & 0b1000, "MIST on: OUT4 (%s)" % bin(out))
    touch(g, FLOOD)
    touch(g, MIST)
    out = diag(g)[5]
    ok &= check(not (out & 0b1100), "both off again (%s)" % bin(out))
    touch(g, SPIN[3])
    ok &= check(True, "STOP while idle only shows a hint (checked on screen)")
    return ok


def t_aux(g):
    say("\nI/O page, aux outputs:")
    ok = True
    touch(g, TAB["io"])
    touch(g, AUX[1], ms=200)
    ok &= check(not (diag(g)[5] & 0b100000), "AUX1 tapped briefly: stays off")
    touch(g, AUX[1], ms=800)
    ok &= check(diag(g)[5] & 0b100000, "AUX1 held: OUT6 on (%s)" % bin(diag(g)[5]))
    touch(g, AUX[1], ms=800)
    ok &= check(not (diag(g)[5] & 0b100000), "AUX1 held again: off (%s)" % bin(diag(g)[5]))
    return ok


def t_stale(g):
    say("\nstale and refused actions:")
    ok = True
    g.ok("G10 L2 P1 X0 Y0 Z0 A0")
    g.ok("G0 X5")
    sync(g)
    touch(g, TAB["dro"])
    # The dwell keeps Grbl in the parser for 2 s while it reports Idle: the
    # screen lets the zero through, the M7 takes it only after the dwell.
    # ($TOUCH itself would wait for the dwell too, so the press is queued
    # with a delay that puts it inside the dwell.)
    g.ok("$TOUCH=%d,%d,1000,300" % ZERO[0])
    g.write(b"G4 P2\n")
    lines = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 3:
        l = g.readline(timeout=0.2)
        if l is not None:
            lines.append(l)
            if l == b"ok":
                break
    time.sleep(0.2)
    o = offsets(g)
    ok &= check(near(o["G54"][0], 0), "zero pressed during a 2 s dwell is dropped (G54 X %.3f)" % o["G54"][0])
    ok &= check(lines.count(b"ok") == 1, "the dwell got its one ok, nothing else: %s" % lines)

    g.ok("G0 X0")
    sync(g)
    # Alarm via a reset during motion, then try to jog from the screen.
    g.ok("G1 X50 F3000")
    time.sleep(0.2)
    g.write(b"\x18")
    g.drain(1.0)
    s0 = g.wait_idle()
    touch(g, TAB["jog"])
    touch(g, JOG["X+"])
    time.sleep(0.3)
    s1 = status(g)
    ok &= check(s1[1].startswith("Alarm") and s1[2] == s0[2], "jog in alarm does nothing (%s)" % s1[1])
    touch(g, UNLOCK, ms=800)
    g.ok("G0 X0")
    sync(g)
    return ok


def t_homing(g):
    """HOME with no switch connected: the search runs its full distance. The
    display, '?' and the touch screen's RESET must keep working meanwhile."""
    say("\nhoming from the screen, no switches connected:")
    ok = True
    for n, v in ((22, 1), (25, 500), (130, 200), (131, 200), (132, 200)):
        g.ok("$%d=%s" % (n, v))
    g.reset()                       # with homing enabled Grbl starts in alarm
    touch(g, TAB["jog"])
    # HOME held 1.1 s; RESET queued 4 s ahead, while the 36 s search runs
    # (Grbl reads no lines during homing, so it cannot be sent later).
    g.ok("$TOUCH=%d,%d,1100" % HOME)
    g.ok("$TOUCH=%d,%d,100,4000" % RESET)
    t0 = time.monotonic()
    homes = 0
    alarm = None
    while time.monotonic() - t0 < 12 and alarm is None:
        g.write(b"?")
        t1 = time.monotonic()
        while time.monotonic() - t1 < 0.2:
            l = g.readline(timeout=0.2)
            if l is None:
                break
            if l.startswith(b"<Home"):
                homes += 1
            if l.startswith(b"ALARM:"):
                alarm = (l.decode(), time.monotonic() - t0)
    ok &= check(homes >= 5, "'?' answered during homing with state Home: %d reports" % homes)
    ok &= check(alarm and alarm[0] == "ALARM:6" and alarm[1] < 6,
                "touch RESET during the search stops homing: %s" % (alarm,))
    g.drain(0.5)
    g.ok("$22=0")
    g.reset()
    g.ok("$X")
    touch(g, TAB["dro"])
    return ok


def t_collet(g):
    say("\ncollet button (I/O page), also in an M6 pause:")
    ok = True
    g.ok("$51=0")
    touch(g, TAB["io"])
    touch(g, COLLET, ms=200)
    ok &= check(not (diag(g)[5] & (1 << 8)), "COLLET tapped briefly while closed: stays closed")
    touch(g, COLLET, ms=900)
    ok &= check(diag(g)[5] & (1 << 8), "COLLET held: OUT9 on (%s)" % bin(diag(g)[5]))
    r = g.cmd("M3 S100")
    ok &= check(r[-1] == b"error:39", "M3 while open: %s" % r[-1].decode())
    touch(g, COLLET, ms=150)
    ok &= check(not (diag(g)[5] & (1 << 8)), "a tap closes it (%s)" % bin(diag(g)[5]))
    # Grbl parses no line during the pause, $TOUCH included: queue the presses first.
    g.ok("$TOUCH=469,327,900,800")      # hold COLLET 0.8 s from now: opens
    g.ok("$TOUCH=469,327,150,3000")     # tap it 3 s from now: closes
    g.write(b"T5 M6\n")
    time.sleep(2.0)
    s = g.query()
    ok &= check(s and s[1].startswith("Hold"), "M6 pause, collet opened from the display: %s" % (s[1] if s else None))
    g.write(b"~")                    # START with the collet open: refused
    time.sleep(0.3)
    s = g.query()
    ok &= check(s and s[1].startswith("Hold"), "START refused while the collet is open: %s" % (s[1] if s else None))
    time.sleep(1.2)                  # the tap has closed it
    g.write(b"~")
    time.sleep(0.5)
    g.drain(0.3)
    s = g.query()
    ok &= check(s and s[1].startswith("Idle"), "collet closed, START resumes: %s" % (s[1] if s else None))
    ok &= check(not (diag(g)[5] & (1 << 8)), "collet closed at the end")
    g.ok("$51=5000")
    touch(g, TAB["dro"])
    return ok


def select_file(g, name):
    """On the FILE page: pages to the file and taps its row (the display
    lists the files in the order $F gives them, five to a page)."""
    import usbjob
    names = list(usbjob.listing(g)[0])
    i = names.index(name)
    for _ in range(len(names) // 5 + 1):
        touch(g, FPREV)                # back to the first page
    for _ in range(i // 5):
        touch(g, FNEXT)
    touch(g, FROW[i % 5])


def t_file(g):
    say("\nFILE page (a program from the USB stick):")
    import usbjob
    ok = True
    state = usbjob.stick_state(g)
    if not state.startswith("ready"):
        return check(False, "a FAT32 or exFAT stick in the GIGA (USB stick: %s)" % state)
    usbjob.upload(g, os.path.join(HERE, "..", "examples", "usb", "short.nc"))
    touch(g, TAB["file"])
    select_file(g, "zz_short.nc")
    time.sleep(1.2)                    # the display starts a program only after the PC was quiet 1 s
    touch(g, FSTART, ms=300)
    j = usbjob.job(g)
    ok &= check(j["state"] == "idle" or j["state"] in ("done", "failed", "aborted"),
                "START tapped briefly: nothing starts (%s)" % j["state"])
    time.sleep(1.2)
    touch(g, FSTART, ms=1300, settle=0.5)
    t0 = time.monotonic()
    paused = False
    while time.monotonic() - t0 < 30:
        s = g.query()
        if s and s[1].startswith("Hold:0") and not paused:
            paused = True
            touch(g, START, ms=100)    # M0: the right column's START resumes
        j = usbjob.job(g)
        if j["state"] in ("done", "failed", "aborted"):
            break
        time.sleep(0.2)
    ok &= check(j["state"] == "done" and paused, "held START: ran, paused at M0, START resumed, done (%s)" % j["state"])

    usbjob.upload(g, os.path.join(HERE, "..", "examples", "usb", "demo_complex.nc"))
    select_file(g, "zz_demo_complex.nc")
    time.sleep(1.2)
    touch(g, FSTART, ms=1300, settle=0.5)
    t0 = time.monotonic()
    while time.monotonic() - t0 < 20 and usbjob.job(g)["state"] != "run":
        time.sleep(0.2)
    time.sleep(1.0)
    touch(g, FSTOP, ms=800, settle=0.3)
    seen = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 6:
        l = g.readline(timeout=0.2)
        if l:
            seen.append(l)
            if l.startswith(b"[MSG:USB job") and b"stopped" in l:
                break
    time.sleep(0.5)
    g.drain(0.3)
    s = g.query()
    j = usbjob.job(g)
    alarm3 = any(l.startswith(b"ALARM:3") for l in seen)
    ok &= check(j["state"] == "aborted" and s[1].startswith("Idle") and not alarm3,
                "STOP held: stopped, Idle, no ALARM:3 (%s, %s)" % (j["state"], s[1]))
    touch(g, TAB["dro"])
    usbjob.cleanup(g)                  # the zz_ test files off the stick again
    return ok


def setting(g, n):
    return g.settings().get(n)


def open_setting(g, n):
    """From anywhere: the $$ page, scrolled to setting n, its editor open."""
    touch(g, TAB["set"])
    touch(g, TAB["set"])          # a second tap closes an open editor
    i = SETTING_ORDER.index(n)
    for _ in range(15):
        touch(g, SPREV, ms=60, settle=0.08)
    for _ in range(i // 6):
        touch(g, SNEXT, ms=60, settle=0.08)
    touch(g, SROW[i % 6])


def type_keys(g, text):
    for ch in text:
        touch(g, KEY[ch], ms=60, settle=0.08)


def t_settings(g):
    say("\n$$ page:")
    ok = True
    p0 = setting(g, 0)
    open_setting(g, 0)
    type_keys(g, "12")
    touch(g, KEY["SAVE"], settle=0.4)
    ok &= check(setting(g, 0) == 12, "$0: typed 12, SAVE -> $0=%s" % setting(g, 0))
    open_setting(g, 0)
    type_keys(g, "1")
    touch(g, KEY["SAVE"], settle=0.4)
    ok &= check(setting(g, 0) == 12, "$0=1 is refused by Grbl (min 3), $0 stays %s" % setting(g, 0))
    touch(g, KEY["CANCEL"])
    g.ok("$0=%g" % p0)

    open_setting(g, 110)
    type_keys(g, "12345")
    touch(g, KEY["DEL"])
    type_keys(g, ".5")
    touch(g, KEY["SAVE"], settle=0.4)
    ok &= check(setting(g, 110) == 1234.5, "$110: 12345 DEL .5 -> %s" % setting(g, 110))

    p3 = setting(g, 3)
    open_setting(g, 3)
    touch(g, OPT4[0])
    touch(g, OPT4[2])
    touch(g, KEY["SAVE"], settle=0.4)
    ok &= check(setting(g, 3) == 5, "$3: X and Z switched on -> $3=%s" % setting(g, 3))
    g.ok("$3=%g" % p3)

    open_setting(g, 42)
    touch(g, OPT2[1])               # 1 linear
    touch(g, KEY["SAVE"], settle=0.4)
    ok &= check(setting(g, 42) == 1, "$42: choice 1 (linear) -> $42=%s" % setting(g, 42))
    open_setting(g, 42)
    touch(g, OPT2[0])
    touch(g, KEY["CANCEL"])
    ok &= check(setting(g, 42) == 1, "CANCEL leaves $42=%s" % setting(g, 42))

    open_setting(g, 79)
    touch(g, OPT22[0])              # IN1
    touch(g, OPT22[20])             # STOP
    touch(g, OPT22[21])             # PWR, left of CANCEL
    touch(g, KEY["SAVE"], settle=0.4)
    ok &= check(setting(g, 79) == 1 + (1 << 20) + (1 << 21), "$79: IN1, STOP and PWR ticked -> $79=%s" % setting(g, 79))
    g.ok("$79=0")

    p95 = setting(g, 95)
    open_setting(g, 95)
    type_keys(g, "750")
    touch(g, KEY["SAVE"], settle=0.4)
    ok &= check(setting(g, 95) == 750, "$95 (last global setting): typed 750 -> %s" % setting(g, 95))
    g.ok("$95=%g" % p95)

    open_setting(g, 90)
    type_keys(g, "4")
    touch(g, KEY["SAVE"], settle=0.4)
    r = setting(g, 90)
    ok &= check(r == 9, "$90=4 (OUT4 is mist) refused, stays %s" % r)
    touch(g, KEY["CANCEL"])
    bad = unsolicited(g)
    ok &= check(not bad, "no ok/error on USB: %s" % bad)
    touch(g, TAB["dro"])
    return ok


def t_shots(g):
    say("\nscreenshots:")
    g.ok("G0 X12.345 Y-6.7 Z3.2 A45")
    sync(g)
    for name, tab in (("display.png", "dro"), ("display_jog.png", "jog"),
                      ("display_override.png", "ovr"), ("display_io.png", "io"),
                      ("display_settings.png", "set")):
        touch(g, TAB[tab])
        time.sleep(0.3)
        screenshot(g, name)
        say("  saved docs/" + name)
    touch(g, TAB["dro"])
    g.ok("G0 X0 Y0 Z0 A0")
    sync(g)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--shots", action="store_true", help="save screenshots of the pages to docs/")
    ap.add_argument("--only", choices=["realtime", "jog", "zero", "override", "aux", "collet", "file", "stale", "settings", "homing"])
    a = ap.parse_args()
    g = Grbl(a.port)
    say("port:", g.port)
    results = []
    try:
        g.setup_bench(steps_per_mm=100, max_rate=6000, accel=1000)
        d = diag(g)
        say("touch controller: status %d (1 = GT911 found), %d presses, resolution %dx%d; RGB LED %d"
            % (d[6], d[7], d[10], d[11], d[12]))
        results.append(("rgb", check(d[12] == 1, "RGB LED driver answers on I2C")))
        tests = [("realtime", t_realtime), ("jog", t_jog), ("zero", t_zero), ("override", t_override),
                 ("aux", t_aux), ("collet", t_collet), ("file", t_file), ("stale", t_stale), ("settings", t_settings), ("homing", t_homing)]
        for name, fn in tests:
            if not a.only or name == a.only:
                results.append((name, fn(g)))
        if a.shots:
            results.append(("shots", t_shots(g)))
    finally:
        try:
            g.ok("$TOUCH=%d,%d,50" % TAB["dro"])   # back to the DRO page
            g.ok("$42=0")
        except Exception as e:
            say("cleanup failed:", e)
        g.close()
    say("\nsummary: " + ", ".join("%s %s" % (n, "ok" if r else "FAIL") for n, r in results))


if __name__ == "__main__":
    main()
