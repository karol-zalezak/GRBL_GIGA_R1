#!/usr/bin/env python3
"""Communication tests for Grbl-Giga on the Arduino GIGA R1.

Exercises the PC link (USB CDC -> Linux router -> LPUART1 -> STM32) the way a
g-code sender would, and reports numbers rather than pass/fail where the
number is the point:

  banner       reset, banner line, transport counters
  integrity    $I and $$ repeated, byte-identical every time
  latency      ? round trip, median / p95 / max, reports per second
  rtcmd        feed hold / resume seen in status within a bounded time
  throughput   comment-only lines streamed with character counting, lines/s
  soak         comment lines + 50 ms status polling for a while, zero errors
  settings     a $-write survives a soft reset

    python test_comm.py [--port COM4] [--soak 60] [--quick]
"""
import argparse
import statistics
import time

from grbl_link import Grbl, say


def t_banner(g):
    banner = g.reset()
    say("banner:", [b.decode(errors="replace") for b in banner])
    assert any(b.startswith(b"Grbl 1.1h") for b in banner), "no Grbl banner"
    msg = [b for b in banner if b.startswith(b"[MSG:")]
    assert msg, "no transport line"
    say("  transport:", msg[0].decode())
    assert b"nvs=1" in msg[0], "settings flash not usable"


def t_integrity(g, n=20):
    def info():
        # the [MSG:] diagnostics line carries live counters, so leave it out
        return [l for l in g.ok("$I") if not l.startswith(b"[MSG:")]
    ref_i = info()
    ref_s = g.ok("$$")
    bad = 0
    for _ in range(n):
        if info() != ref_i:
            bad += 1
        if g.ok("$$") != ref_s:
            bad += 1
    say("integrity: %d x ($I + $$), %d mismatches, $$ is %d lines / %d bytes"
        % (n, bad, len(ref_s), sum(len(l) + 2 for l in ref_s)))
    assert bad == 0


def t_latency(g, n=200):
    # Non-blocking reads and a spin loop: pyserial's blocking read on Windows
    # wakes on the 16 ms system tick, which would swamp the number measured.
    g.drain(0.1)
    ser = g.ser
    saved = ser.timeout
    ser.timeout = 0
    times = []
    try:
        for _ in range(n):
            t0 = time.perf_counter()
            ser.write(b"?")
            buf = b""
            while b"\n" not in buf and time.perf_counter() - t0 < 2.0:
                d = ser.read(256)
                if d:
                    buf += d
            t1 = time.perf_counter()
            assert buf.startswith(b"<"), "status request unanswered: %r" % buf
            times.append((t1 - t0) * 1000.0)
    finally:
        ser.timeout = saved
    times.sort()
    med = statistics.median(times)
    say("latency: ? round trip over %d samples: median %.2f ms, p95 %.2f ms, max %.2f ms, min %.2f ms  (%.0f reports/s)"
        % (n, med, times[int(n * 0.95)], times[-1], times[0], 1000.0 / med))
    return med


def t_rtcmd(g):
    # Feed hold from idle lands in Hold:0; resume takes it back to Idle. The
    # real-time character and the status request travel together, so the time
    # measured is one link round trip - the hold itself is immediate.
    g.drain(0.1)
    ser = g.ser
    saved = ser.timeout
    ser.timeout = 0

    def until(rt, want):
        t0 = time.perf_counter()
        ser.write(rt + b"?")
        buf = b""
        while time.perf_counter() - t0 < 2.0:
            d = ser.read(256)
            if d:
                buf += d
            if want in buf:
                return (time.perf_counter() - t0) * 1000.0, buf
        return None, buf

    try:
        t_hold, buf = until(b"!", b"<Hold")
        assert t_hold is not None, "feed hold not reflected in status: %r" % buf
        t_resume, buf = until(b"~", b"<Idle")
        assert t_resume is not None, "resume not reflected in status: %r" % buf
    finally:
        ser.timeout = saved
    g.drain(0.1)
    say("rtcmd: '!' -> Hold seen after %.1f ms, '~' -> Idle after %.1f ms" % (t_hold, t_resume))


def stream(g, lines, window=127, poll_ms=0, duration=None, on_tick=None):
    """Character-counting streamer. Returns (sent, oks, errors, bad_frames,
    status_frames, elapsed). `lines` is an iterable of bytes without newline.
    `on_tick(elapsed_s)` is called on every loop pass, for injecting real-time
    commands independently of the flow-controlled g-code stream."""
    inflight = []
    sent = oks = errs = bad = status = 0
    t0 = time.monotonic()
    last_poll = t0
    it = iter(lines)
    pending = next(it, None)
    done_sending = pending is None
    while True:
        now = time.monotonic()
        if on_tick:
            on_tick(now - t0)
        if duration and now - t0 > duration:
            done_sending = True
        if poll_ms and (now - last_poll) * 1000.0 >= poll_ms:
            g.write(b"?")
            last_poll = now
        # fill the window
        while not done_sending and pending is not None and sum(inflight) + len(pending) + 1 <= window:
            g.write(pending + b"\n")
            inflight.append(len(pending) + 1)
            sent += 1
            pending = next(it, None)
            if pending is None:
                done_sending = True
        l = g.readline(timeout=0.02)
        if l is None:
            if done_sending and not inflight:
                break
            if now - t0 > 30 and not inflight and done_sending:
                break
            if inflight and now - t0 > 60 + (duration or 0):
                say("  stream stalled with %d in flight" % len(inflight))
                break
            continue
        if l == b"ok":
            oks += 1
            if inflight:
                inflight.pop(0)
        elif l.startswith(b"error:") or l.startswith(b"ALARM"):
            errs += 1
            if inflight:
                inflight.pop(0)
        elif l.startswith(b"<") and l.endswith(b">"):
            status += 1
        elif l.startswith(b"[") or l.startswith(b"Grbl") or l == b"":
            pass
        else:
            bad += 1
            say("  bad frame: %r" % l)
        if done_sending and not inflight:
            break
    return sent, oks, errs, bad, status, time.monotonic() - t0


def t_throughput(g, n=2000, window=127):
    g.drain(0.1)
    lines = [b"(line %05d)" % i for i in range(n)]
    sent, oks, errs, bad, _, dt = stream(g, lines, window=window)
    nbytes = sum(len(l) + 1 for l in lines[:sent])
    say("throughput: %d comment lines, window %d: %d ok, %d errors, %d bad frames in %.2f s -> %.0f lines/s, %.1f kB/s in"
        % (sent, window, oks, errs, bad, dt, sent / dt, nbytes / dt / 1000.0))
    assert oks == sent and errs == 0 and bad == 0


def t_soak(g, seconds=60):
    g.drain(0.1)

    def gen():
        i = 0
        while True:
            yield b"(soak %06d)" % i
            i += 1

    sent, oks, errs, bad, status, dt = stream(g, gen(), window=127, poll_ms=50, duration=seconds)
    say("soak: %.0f s, %d lines (%0.f lines/s), %d ok, %d errors, %d bad frames, %d status frames"
        % (dt, sent, sent / dt, oks, errs, bad, status))
    assert oks == sent and errs == 0 and bad == 0
    info = g.ok("$I")
    diag = [l for l in info if l.startswith(b"[MSG:")]
    say("  diag:", diag[0].decode() if diag else "-")


def t_settings(g):
    before = g.settings()
    g.ok("$100=137.5")
    g.ok("$120=444")
    banner = g.reset()
    after = g.settings()
    say("settings: $100=%s $120=%s after soft reset" % (after.get(100), after.get(120)))
    assert abs(after[100] - 137.5) < 1e-3 and abs(after[120] - 444) < 1e-3
    g.ok("$100=%s" % before[100])
    g.ok("$120=%s" % before[120])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--soak", type=int, default=60)
    ap.add_argument("--quick", action="store_true")
    a = ap.parse_args()
    g = Grbl(a.port)
    say("port:", g.port)
    try:
        t_banner(g)
        t_integrity(g, 5 if a.quick else 20)
        t_latency(g, 50 if a.quick else 200)
        t_rtcmd(g)
        t_throughput(g, 500 if a.quick else 2000, window=127)
        t_throughput(g, 500 if a.quick else 2000, window=500)
        t_settings(g)
        if not a.quick:
            t_soak(g, a.soak)
        say("ALL COMM TESTS PASSED")
    finally:
        g.close()


if __name__ == "__main__":
    main()
