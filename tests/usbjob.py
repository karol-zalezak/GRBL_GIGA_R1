#!/usr/bin/env python3
"""Helpers for programs from the USB stick: upload a test file onto the stick
($FW), list ($F), parse [JOB:...] lines, clean up.

Test files go onto the stick as zz_<name> (sorted last, never a user's own
file) and are deleted again by cleanup().

    python usbjob.py upload ../examples/usb/short.nc [more files]
    python usbjob.py list
"""
import os
import re
import sys
import time

from grbl_link import Grbl

JOB_RE = re.compile(rb"\[JOB:(\w+)(?:\|([^|]*)\|LINE:(\d+)/(\d+)\|POS:(\d+)/(\d+)\|ERR:(\d+),(\d+)\|ALARM:(\d+)"
                    rb"\|CHECKED:(\d+)\|MS:(\d+)\|RANGE:([^\]]*))?\]")
FILE_RE = re.compile(rb"\[FILE:([^|]+)\|SIZE:(\d+)(?:\|DATE:([-0-9T:]+))?\]")
READ_RE = re.compile(rb"\[MSG:read .*: (\d+) bytes, (\d+) lines, CRC32 ([0-9A-F]{8}) in (\d+) ms")
CHUNK = 37          # bytes per $FW+ line: 4 + 74 hex digits < 80
PREFIX = "zz_"      # test files on the stick

uploaded = set()    # names to delete in cleanup()


def test_name(path):
    return PREFIX + os.path.basename(path)


def upload(g, path, name=None):
    """Uploads path onto the stick as `name` (default: zz_<its file name>),
    dated as the PC file. Returns the time it took."""
    name = name or test_name(path)
    data = open(path, "rb").read()
    t0 = time.perf_counter()
    g.ok("$FW=" + name, timeout=15)
    uploaded.add(name)
    # Character counting: keep up to ~100 lines in flight.
    lines = ["$FW+" + data[i:i + CHUNK].hex().upper() for i in range(0, len(data), CHUNK)]
    sent = 0
    acked = 0
    inflight = []
    while acked < len(lines):
        while sent < len(lines) and sum(inflight) + len(lines[sent]) + 1 < 900:
            g.write(lines[sent] + "\n")
            inflight.append(len(lines[sent]) + 1)
            sent += 1
        l = g.readline(timeout=15.0)
        if l is None:
            raise TimeoutError("upload stalled at line %d" % acked)
        if l == b"ok" or l.startswith(b"error:"):
            if l != b"ok":
                raise RuntimeError("upload line %d: %r" % (acked, l))
            inflight.pop(0)
            acked += 1
    g.ok("$FWE=" + time.strftime("%Y%m%d%H%M%S", time.localtime(os.path.getmtime(path))), timeout=30)
    return time.perf_counter() - t0


def cleanup(g):
    """Deletes the test files uploaded so far."""
    for name in sorted(uploaded):
        g.cmd("$FD=" + name, timeout=15)
    uploaded.clear()


def read_test(g, name):
    """$FT=name: (bytes, lines, crc32, ms) as the GIGA read it."""
    for l in g.ok("$FT=" + name, timeout=60):
        m = READ_RE.match(l)
        if m:
            return int(m.group(1)), int(m.group(2)), int(m.group(3), 16), int(m.group(4))
    return None


def stick_state(g):
    for l in g.ok("$F"):
        if l.startswith(b"[MSG:USB stick:"):
            return l[16:-1].decode(errors="replace")
    return ""


def listing(g):
    """({name: size} in the order the GIGA lists them, the job)."""
    files, job = {}, None
    for l in g.ok("$F"):
        m = FILE_RE.match(l)
        if m:
            files[m.group(1).decode(errors="replace")] = int(m.group(2))
        m = JOB_RE.match(l)
        if m:
            job = parse_job(m)
    return files, job


def parse_job(m):
    g = m.groups()
    d = {"state": g[0].decode()}
    if g[1] is not None:
        d.update(name=g[1].decode(errors="replace"), line=int(g[2]), lines=int(g[3]), pos=int(g[4]),
                 size=int(g[5]), err=int(g[6]), err_line=int(g[7]), alarm=int(g[8]), checked=int(g[9]),
                 ms=int(g[10]), range=g[11].decode())
    return d


def job(g):
    return listing(g)[1]


if __name__ == "__main__":
    g = Grbl(quiet=True)
    if sys.argv[1] == "upload":
        for p in sys.argv[2:]:
            print(p, "%.1f s" % upload(g, p))
    files, j = listing(g)
    print(files)
    print(j)
