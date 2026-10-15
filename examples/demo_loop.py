#!/usr/bin/env python3
"""Runs a g-code file on the machine over and over.

    python demo_loop.py                     demo_complex.nc, until Ctrl-C
    python demo_loop.py --laps 5            five times, then stop
    python demo_loop.py part.nc --pause 3   another file, 3 s parked between laps
    python demo_loop.py --port COM7         if more than one board is plugged in

Grbl has no loop of its own (no subprograms, no O-words, no GOTO), so the loop
lives in the sender: the file is streamed with the usual character-counting
protocol, and once the last line is acknowledged and the machine is idle it is
streamed again. Nothing in the controller is touched - no settings, no unlock,
no homing. Put the machine where you want it, zero it in UGS if the file does
not do that itself, close UGS so the port is free, and start this.

Any origin-setting line in the file (`G10 L20 ...` or `G92 ...`) is sent on the
first lap only. The demo program parks 5 mm above its start point, so zeroing
again at every lap would lift the origin 5 mm each time.

Ctrl-C stops it the way a sender's Stop button does: feed hold, wait for the
motion to stop, soft reset. The machine keeps its position and its work zero,
so a new start carries on from where it stands.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))
from grbl_link import Grbl   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# <State|MPos:x,y,z|...> or <State|WPos:x,y,z|...>, whichever $10 selects.
STATUS_RE = re.compile(rb"<([^|>]+)\|([MW])Pos:([-\d.]+),([-\d.]+),([-\d.]+)(?:\|(.*))?>")
ORIGIN_RE = re.compile(rb"^(G10L20|G92(?!\.))")   # matched against the line with spaces removed


def load(path):
    """The file as a list of lines to send: comments and blanks stripped."""
    out = []
    for raw in open(path, "rb").read().splitlines():
        l = raw.split(b"(")[0].split(b";")[0].strip()
        if l and not l.startswith(b"%"):
            out.append(l)
    return out


class Machine:
    """Keeps the last status report in a usable form."""

    def __init__(self):
        self.state = "?"
        self.wpos = (0.0, 0.0, 0.0)
        self.feed = 0
        self.wco = None

    def take(self, line):
        m = STATUS_RE.match(line)
        if not m:
            return False
        self.state = m.group(1).decode()
        pos = tuple(float(v) for v in m.group(3, 4, 5))
        rest = m.group(6) or b""
        w = re.search(rb"WCO:([-\d.]+),([-\d.]+),([-\d.]+)", rest)
        if w:
            self.wco = tuple(float(v) for v in w.groups())
        if m.group(2) == b"M":
            if self.wco:
                pos = tuple(p - o for p, o in zip(pos, self.wco))
        self.wpos = pos
        f = re.search(rb"FS?:(\d+)", rest)
        self.feed = int(f.group(1)) if f else 0
        return True


def show(text):
    """One line that is rewritten in place."""
    sys.stdout.write("\r" + text[:100].ljust(100))
    sys.stdout.flush()


def note(text):
    """A line that stays."""
    sys.stdout.write("\r" + " " * 100 + "\r" + text + "\n")
    sys.stdout.flush()


def wait_state(g, mc, accept, timeout):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        g.write(b"?")
        l = g.readline(timeout=0.3)
        while l is not None:
            mc.take(l)
            l = g.readline(timeout=0.02)
        if any(mc.state.startswith(a) for a in accept):
            return True
        time.sleep(0.1)
    return False


def stop(g, mc):
    """Feed hold, wait for the axes to stop, soft reset - a sender's Stop."""
    g.write(b"!")
    if not wait_state(g, mc, ("Hold:0", "Idle", "Alarm", "Door:0"), 15.0):
        note("no hold after 15 s, resetting anyway (this may raise an alarm)")
    g.write(b"\x18")
    time.sleep(0.5)
    g.drain(0.3)
    wait_state(g, mc, ("Idle", "Alarm"), 3.0)
    note("stopped: %s  X%.3f Y%.3f Z%.3f" % (mc.state, *mc.wpos))
    if mc.state.startswith("Alarm"):
        note("the controller is in alarm; $X unlocks it")


def run_lap(g, mc, lines, lap, window, stats):
    """Streams one lap. Returns False if the controller raised an alarm."""
    inflight = []
    sent = acked = errors = 0
    t0 = time.monotonic()
    last_poll = last_show = t0
    while acked < len(lines):
        now = time.monotonic()
        if now - last_poll >= 0.2:
            g.write(b"?")
            last_poll = now
        while sent < len(lines) and sum(inflight) + len(lines[sent]) + 1 <= window:
            g.write(lines[sent] + b"\n")
            inflight.append(len(lines[sent]) + 1)
            sent += 1
        l = g.readline(timeout=0.02)
        if l is None:
            if now - last_show >= 0.25:
                show("lap %s  line %d/%d  %-7s X%9.3f Y%9.3f Z%8.3f  F%-5d  %5.0f s"
                     % (lap, acked, len(lines), mc.state, *mc.wpos, mc.feed, now - t0))
                last_show = now
            continue
        if l == b"ok":
            acked += 1
            inflight.pop(0)
        elif l.startswith(b"error:"):
            note("lap %s line %d: %s -> %s" % (lap, acked + 1, lines[acked].decode(errors="replace"), l.decode()))
            errors += 1
            acked += 1
            inflight.pop(0)
        elif l.startswith(b"ALARM:"):
            note("lap %s: %s" % (lap, l.decode()))
            g.drain(0.5)
            return False
        elif l.startswith(b"<"):
            mc.take(l)
        elif l.startswith(b"[MSG:") and l != b"[MSG:Pgm End]":
            note(l.decode(errors="replace"))
    stats.append((time.monotonic() - t0, errors))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("file", nargs="?", default=os.path.join(HERE, "demo_complex.nc"))
    ap.add_argument("--laps", type=int, default=0, help="how many times (default: until Ctrl-C)")
    ap.add_argument("--pause", type=float, default=0.0, help="seconds to wait between laps")
    ap.add_argument("--window", type=int, default=127, help="bytes in flight (Grbl's RX buffer minus one)")
    ap.add_argument("--port", help="COM port (default: the first UNO Q found)")
    args = ap.parse_args()

    lines = load(args.file)
    if not lines:
        sys.exit("%s: nothing to send" % args.file)
    origin = [i for i, l in enumerate(lines) if ORIGIN_RE.match(l.replace(b" ", b"").upper())]
    body = [l for i, l in enumerate(lines) if i not in origin]

    try:
        g = Grbl(args.port, quiet=True)
    except Exception as e:
        sys.exit("cannot open the port (%s) - is UGS still connected?" % e)
    mc = Machine()
    g.write(b"\r\n")
    time.sleep(0.3)
    g.drain(0.3)
    if not wait_state(g, mc, ("",), 5.0):
        sys.exit("no status report from Grbl on %s" % g.port)
    if mc.state.startswith("Alarm"):
        sys.exit("the controller is in alarm (%s): unlock or home it in UGS first" % mc.state)
    if not mc.state.startswith("Idle"):
        sys.exit("the controller is busy (%s)" % mc.state)

    note("%s: %d lines%s" % (os.path.basename(args.file), len(lines),
                             ", origin set by %d line(s) on the first lap only" % len(origin) if origin else ""))
    note("at X%.3f Y%.3f Z%.3f, %s - Ctrl-C stops" % (*mc.wpos, "until Ctrl-C" if args.laps <= 0 else "%d laps" % args.laps))

    stats = []
    lap = 0
    try:
        while args.laps <= 0 or lap < args.laps:
            lap += 1
            label = "%d" % lap if args.laps <= 0 else "%d/%d" % (lap, args.laps)
            if not run_lap(g, mc, lines if lap == 1 else body, label, args.window, stats):
                break
            if not wait_state(g, mc, ("Idle", "Alarm", "Hold"), 600.0):
                note("lap %s: machine did not go idle (%s)" % (label, mc.state))
                break
            if not mc.state.startswith("Idle"):
                note("lap %s ended in %s" % (label, mc.state))
                break
            t, e = stats[-1]
            note("lap %s: %.1f s, %d lines, %d error(s)" % (label, t, len(lines if lap == 1 else body), e))
            if args.pause > 0:
                time.sleep(args.pause)
    except KeyboardInterrupt:
        note("")
        stop(g, mc)
    if stats:
        note("%d lap(s) run, %.1f s on average, %d error(s)"
             % (len(stats), sum(t for t, _ in stats) / len(stats), sum(e for _, e in stats)))
    g.close()


if __name__ == "__main__":
    main()
