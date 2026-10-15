#!/usr/bin/env python3
"""Programs from the USB stick (PLAN.md part 4). A FAT32 or exFAT stick must be
in the GIGA's USB-A port: every test file is uploaded onto it as zz_<name>
($FW) and deleted again at the end, so the stick's own files stay as they
were.

  check      first run: check pass ($C) with line count and X/Y/Z range, then
             the run; M0 pauses, START resumes; the second run skips the
             check; a changed file is checked again
  error      a G-code error in line 7: refused in the check pass, nothing
             moves, the parser state from before is back
  m6         M6 pauses with [MSG:Tool change T2 ...], START resumes
  pc         while a program runs: G-code from the PC refused (error:48),
             $I and ? answered
  stop       STOP ($FS) during motion: hold, then a reset that keeps the
             position - no ALARM:3
  reset      Ctrl-X during motion: ALARM:3, the job is aborted
  rate       10 000 segments of 0.05 mm at F12000: lines per second from the
             stick against the PC's figure (test_stream.py)
  demo       examples/demo_complex.nc run whole: time and end position (long)
  files      a name with spaces and lower case: listed, read back (CRC-32),
             run, deleted

    python test_job.py [--port COM5] [--only check,error,...] [--demo]
"""
import argparse
import os
import time

from grbl_link import Grbl, say
import usbjob

HERE = os.path.dirname(os.path.abspath(__file__))
USB = os.path.join(HERE, "..", "examples", "usb")
SHORT = usbjob.PREFIX + "short.nc"


def check(cond, what):
    cond = bool(cond)
    say(("  ok    " if cond else "  FAIL  ") + what)
    return cond


def collect(g, until, timeout):
    seen = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        l = g.readline(timeout=0.2)
        if l:
            seen.append(l)
            if l.startswith(until):
                break
    return seen


def wait_job(g, states=("done", "failed", "aborted"), timeout=60, resume_holds=False):
    """Polls $F until the job reaches one of states. With resume_holds, a
    Hold:0 is answered with START (~). Returns (job, holds seen)."""
    t0 = time.monotonic()
    holds = 0
    while time.monotonic() - t0 < timeout:
        s = g.query()
        if s and s[1].startswith("Hold:0") and resume_holds:
            holds += 1
            time.sleep(0.3)
            g.write(b"~")
            time.sleep(0.3)
            continue
        j = usbjob.job(g)
        if j and j["state"] in states:
            return j, holds
        time.sleep(0.2)
    return usbjob.job(g), holds


def bench(g):
    g.setup_bench(steps_per_mm=100, max_rate=12000, accel=1000)
    # Work coordinates = machine coordinates, so a program's X0 Y0 is MPos 0,0.
    g.ok("G10 L2 P1 X0 Y0 Z0 A0")
    g.ok("G92.1")


def t_check(g):
    say("\ncheck pass, run, M0, second run, changed file:")
    ok = True
    bench(g)
    usbjob.upload(g, os.path.join(USB, "short.nc"))
    files, j = usbjob.listing(g)
    ok &= check(SHORT in files, "listed: %s" % list(files))
    r = g.cmd("$F=" + SHORT)
    ok &= check(r[-1] == b"ok", "$F=%s: %s" % (SHORT, [l.decode() for l in r]))
    j, holds = wait_job(g, resume_holds=True, timeout=40)
    ok &= check(j["state"] == "done" and j["checked"] == 0,
                "done after a check pass: %s, %d lines" % (j["state"], j["lines"]))
    ok &= check(j["lines"] == 11 and j["range"] == "0.000:10.000,0.000:10.000,0.000:0.000",
                "11 lines, range X 0..10 Y 0..10: %s" % j["range"])
    ok &= check(holds == 1, "M0 paused once, START resumed (%d holds)" % holds)
    s = g.query()
    ok &= check(abs(s[2]) < 0.001 and abs(s[3]) < 0.001, "back at 0,0: %.3f %.3f" % (s[2], s[3]))

    r = g.cmd("$F=" + SHORT)
    j, holds = wait_job(g, resume_holds=True, timeout=40)
    ok &= check(j["state"] == "done" and j["checked"] == 1, "second run: no check pass (checked=%d)" % j["checked"])

    usbjob.upload(g, os.path.join(USB, "short.nc"))       # written again: what was known of it is dropped
    g.cmd("$F=" + SHORT)
    j, holds = wait_job(g, resume_holds=True, timeout=40)
    ok &= check(j["state"] == "done" and j["checked"] == 0, "uploaded again: checked again (checked=%d)" % j["checked"])
    return ok


def t_error(g):
    say("\nG-code error in line 7:")
    ok = True
    bench(g)
    usbjob.upload(g, os.path.join(USB, "error_l7.nc"))
    g.ok("G91")                         # parser state the check pass must give back
    p0 = g.query()
    r = g.cmd("$F=zz_error_l7.nc")
    seen = collect(g, b"[MSG:USB job zz_error_l7.nc check: error", 5)
    j, _ = wait_job(g, timeout=10)
    ok &= check(j["state"] == "failed" and j["err"] == 20 and j["err_line"] == 7,
                "check pass refuses it: %s error:%d in line %d" % (j["state"], j["err"], j["err_line"]))
    s = g.query()
    ok &= check(s[1].startswith("Idle") and abs(s[2] - p0[2]) < 1e-6 and abs(s[3] - p0[3]) < 1e-6,
                "Idle, nothing moved: %s" % s[1])
    gm = [l for l in g.ok("$G") if l.startswith(b"[GC:")]
    ok &= check(gm and b"G91" in gm[0], "parser state from before is back: %s" % (gm[0].decode() if gm else None))
    g.ok("G90")
    return ok


def t_m6(g):
    say("\nM6 tool change pause:")
    ok = True
    bench(g)
    usbjob.upload(g, os.path.join(USB, "toolchange.nc"))
    g.write(b"$F=zz_toolchange.nc\n")
    seen = collect(g, b"[MSG:Tool change", 15)
    msg = [l.decode() for l in seen if l.startswith(b"[MSG:Tool")]
    time.sleep(0.3)
    s = g.query()
    ok &= check(msg and "T2" in msg[0] and s[1].startswith("Hold"), "%s, %s" % (msg, s[1]))
    j, holds = wait_job(g, resume_holds=True, timeout=30)
    ok &= check(j["state"] == "done", "START resumes, done: %s" % j["state"])
    return ok


def t_pc(g):
    say("\nPC lines during a program:")
    ok = True
    bench(g)
    usbjob.upload(g, os.path.join(USB, "short.nc"))
    g.ok("$F=" + SHORT)
    time.sleep(0.5)
    r = g.cmd("G0 X1")
    ok &= check(r[-1] == b"error:48", "G0 X1 refused: %s" % r[-1].decode())
    r = g.cmd("$I")
    ok &= check(r[-1] == b"ok", "$I answered: %s" % r[-1].decode())
    r = g.cmd("$X")
    ok &= check(r[-1] == b"error:48", "$X refused: %s" % r[-1].decode())
    s = g.query()
    ok &= check(s and "|SD:" in s[5], "status has |SD: %s" % (s[5][:60] if s else None))
    j, _ = wait_job(g, resume_holds=True, timeout=40)
    ok &= check(j["state"] == "done", "the program finished: %s" % j["state"])
    return ok


def t_stop(g):
    say("\nSTOP during motion:")
    ok = True
    bench(g)
    usbjob.upload(g, os.path.join(USB, "demo_complex.nc"))
    g.ok("$F=zz_demo_complex.nc")
    wait_job(g, states=("run",), timeout=20)
    time.sleep(3.0)
    s0 = g.query()
    g.ok("$FS")
    seen = collect(g, b"[MSG:'$H'|'$X'", 6) if False else collect(g, b"Grbl ", 8)
    time.sleep(0.5)
    g.drain(0.3)
    s = g.query()
    alarm3 = any(l.startswith(b"ALARM:3") for l in seen)
    ok &= check(s and s[1].startswith("Idle") and not alarm3,
                "after STOP: %s, no ALARM:3 (%s)" % (s[1] if s else None, [l.decode() for l in seen if l.startswith(b"ALARM")]))
    j = usbjob.job(g)
    ok &= check(j["state"] == "aborted" and j["alarm"] == 0, "job aborted by STOP: %s" % j["state"])
    # The position is kept: a G-code move from here goes where it should.
    p = g.query()
    g.ok("G91 G0 X1")
    g.ok("G4 P0", timeout=30)
    q = g.query()
    g.ok("G90")
    ok &= check(abs(q[2] - p[2] - 1.0) < 0.002, "position kept, moves on from %.3f to %.3f" % (p[2], q[2]))
    return ok


def t_reset(g):
    say("\nreset during motion:")
    ok = True
    bench(g)
    usbjob.upload(g, os.path.join(USB, "demo_complex.nc"))
    g.ok("$F=zz_demo_complex.nc")
    wait_job(g, states=("run",), timeout=20)
    time.sleep(2.0)
    g.write(b"\x18")
    seen = collect(g, b"[MSG:'$H'|'$X'", 6)
    j = usbjob.job(g)
    ok &= check(j["state"] == "aborted" and j["alarm"] == 3, "ALARM:3, job aborted (alarm %d)" % j["alarm"])
    g.ok("$X")
    return ok


def t_rate(g):
    say("\nshort segments from the stick:")
    ok = True
    g.setup_bench(steps_per_mm=250, max_rate=30000, accel=2000)
    path = os.path.join(HERE, "_rate.nc")
    n = 6000
    from test_stream import raster          # the same raster as the PC test
    lines, _, _ = raster(0.05, n, 12000)
    with open(path, "wb") as f:
        f.write(b"G21 G90\n" + b"\n".join(lines) + b"\nM30\n")
    g.ok("G92 X0 Y0")
    t = usbjob.upload(g, path, "zz_rate.nc")
    say("    uploaded %d bytes in %.1f s" % (os.path.getsize(path), t))
    g.ok("$F=zz_rate.nc")
    j, _ = wait_job(g, states=("run",), timeout=30)
    t0 = time.perf_counter()
    j, _ = wait_job(g, timeout=120)
    dt = time.perf_counter() - t0
    say("    %d lines in %.2f s = %.0f lines/s (from the PC: 1282 lines/s, test_stream.py)" % (n, dt, n / dt))
    ok &= check(j["state"] == "done" and n / dt > 1100, "done, at least 1100 lines/s")
    os.remove(path)
    return ok


def t_demo(g):
    say("\nexamples/demo_complex.nc, whole:")
    ok = True
    bench(g)
    usbjob.upload(g, os.path.join(USB, "demo_complex.nc"))
    g.ok("$F=zz_demo_complex.nc")
    j, _ = wait_job(g, states=("run",), timeout=30)
    say("    check pass: %d lines, range %s" % (j["lines"], j["range"]))
    t0 = time.perf_counter()
    j, holds = wait_job(g, timeout=400, resume_holds=True)
    dt = time.perf_counter() - t0
    s = g.query()
    say("    run: %.1f s (from the PC 152.9 s), %d lines, end MPos %.3f %.3f %.3f" % (dt, j["line"], s[2], s[3], s[4]))
    raw = open(os.path.join(USB, "demo_complex.nc"), "rb").read().splitlines()
    n_m30 = next(i for i, l in enumerate(raw) if l.strip().startswith(b"M30")) + 1
    ok &= check(j["state"] == "done" and j["line"] == n_m30, "done, %d lines up to M30" % n_m30)
    ok &= check(abs(s[2]) < 0.001 and abs(s[3]) < 0.001, "ends at X0 Y0 (G0X0Y0)")
    return ok


def t_files(g):
    say("\na name with spaces and lower case: listed, read back, run, deleted:")
    ok = True
    import zlib
    src = os.path.join(USB, "short.nc")
    name = "zz test Short.nc"
    data = open(src, "rb").read()
    t = usbjob.upload(g, src, name)
    say("    uploaded %d bytes in %.1f s" % (len(data), t))
    files, _ = usbjob.listing(g)
    ok &= check(files.get(name) == len(data), "listed as %r, %d bytes" % (name, files.get(name, -1)))
    r = usbjob.read_test(g, name)
    ok &= check(r and r[0] == len(data) and r[2] == zlib.crc32(data),
                "read back: %s bytes, CRC32 %08X (PC %08X)" % (r and r[0], r[2] if r else 0, zlib.crc32(data)))
    bench(g)
    g.ok("$F=" + name)
    j, _ = wait_job(g, timeout=60, resume_holds=True)
    ok &= check(j["state"] == "done", "run from the stick: %s" % j["state"])
    g.ok("$FD=" + name, timeout=15)
    usbjob.uploaded.discard(name)
    files, _ = usbjob.listing(g)
    ok &= check(name not in files, "deleted again")
    return ok


TESTS = [("check", t_check), ("error", t_error), ("m6", t_m6), ("pc", t_pc), ("stop", t_stop),
         ("reset", t_reset), ("rate", t_rate), ("demo", t_demo), ("files", t_files)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--only")
    ap.add_argument("--demo", action="store_true", help="also run demo_complex.nc whole (2.5 min)")
    a = ap.parse_args()
    only = set(a.only.split(",")) if a.only else None
    g = Grbl(a.port, quiet=True)
    say("port:", g.port)
    state = usbjob.stick_state(g)
    say("USB stick:", state)
    if not state.startswith("ready"):
        say("A FAT32 or exFAT stick must be in the GIGA's USB-A port.")
        g.close()
        raise SystemExit(1)
    results = []
    try:
        for name, fn in TESTS:
            if only and name not in only:
                continue
            if name == "demo" and not (a.demo or (only and "demo" in only)):
                continue
            try:
                results.append((name, fn(g)))
            except Exception as e:
                say("  FAIL  %s: %r" % (name, e))
                results.append((name, False))
                try:
                    g.write(b"\x18")
                    time.sleep(1)
                    g.drain(0.5)
                except Exception:
                    pass
    finally:
        try:
            usbjob.cleanup(g)               # the zz_ test files off the stick again
        except Exception as e:
            say("  cleanup: %r" % e)
        g.close()
    say("\nsummary: " + ", ".join("%s %s" % (n, "ok" if r else "FAIL") for n, r in results))


if __name__ == "__main__":
    main()
