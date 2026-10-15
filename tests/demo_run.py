"""Streams examples/demo_complex.nc like a sender (127-byte window, 50 ms
polling) and reports the total time plus, per section, the line rate and how
often the planner was nearly empty while the machine was moving.

Read the per-section numbers with care: an `ok` arrives when a block is
queued, not when it runs, so a section's clock starts while the previous
section's tail is still executing, and a long single block (an arc, a slow Z
plunge) counts as "planner nearly empty" although nothing is starving. The
total time and the error counts are the reliable part."""
import re, sys, time
from grbl_link import Grbl

g = Grbl()
g.setup_bench(steps_per_mm=250, max_rate=6000, accel=500)
g.ok("$10=3")
raw = open('../examples/demo_complex.nc', 'rb').read().splitlines()
lines = [l.split(b'(')[0].strip() for l in raw]
idx = [i for i, l in enumerate(lines) if l]          # original line numbers of sent lines
lines = [l for l in lines if l]
sections = []
for i, l in enumerate(raw):
    m = re.match(rb"\( (\d)/8 +(.*?) \)", l)
    if m: sections.append((i, m.group(2).decode()))
print("lines:", len(lines), "sections:", [s[1][:20] for s in sections], flush=True)

ok_times = []       # time of each ok, index = sent line number
inflight = []
sent = 0; pending = 0
t0 = time.monotonic(); last_poll = t0
starve = []         # (t, planner_free) samples where Run and planner nearly empty
while pending < len(lines) or inflight:
    now = time.monotonic()
    if now - last_poll >= 0.05:
        g.write(b"?"); last_poll = now
    while pending < len(lines) and sum(inflight) + len(lines[pending]) + 1 <= 127:
        g.write(lines[pending] + b"\n"); inflight.append(len(lines[pending]) + 1); pending += 1
    l = g.readline(timeout=0.02)
    if l is None:
        if now - t0 > 900: print("timeout"); break
        continue
    if l == b"ok" or l.startswith(b"error"):
        if l != b"ok": print("line %d -> %s" % (len(ok_times), l))
        ok_times.append(time.monotonic()); inflight.pop(0)
    elif l.startswith(b"<"):
        s = g.status[-1]
        m = re.search(r"Bf:(\d+),(\d+)", s[5])
        if m and s[1].startswith("Run") and int(m.group(1)) >= 33:
            starve.append((s[0], int(m.group(1))))
total = time.monotonic() - t0
g.wait_idle()
print("total %.1f s, %d ok" % (total, len(ok_times)))
# per section
bounds = [s[0] for s in sections] + [10**9]
for k, (start, name) in enumerate(sections):
    end = bounds[k + 1]
    ids = [j for j, orig in enumerate(idx) if start <= orig < end]
    if len(ids) < 2: continue
    ta, tb = ok_times[ids[0]], ok_times[ids[-1]]
    dur = tb - ta
    st = sum(1 for t, _ in starve if ta <= t <= tb)
    print("  %-52s %5d lines %7.1f s  %6.1f lines/s  planner-dry samples: %d" % (name[:52], len(ids), dur, len(ids) / dur if dur else 0, st))
g.ok("$10=1"); g.close()
