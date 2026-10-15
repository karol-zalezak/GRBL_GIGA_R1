#!/usr/bin/env python3
"""Small helper for talking to Grbl-Giga over the GIGA R1's native USB port.

Shared by the test scripts in this directory. The port is whatever enumerates
as the GIGA R1 running a sketch (VID 2341 PID 0266); the baud rate is
irrelevant on native USB. Status reports carry four axes (X, Y, Z, A); the
parsed tuples keep X/Y/Z where the three-axis scripts expect them and add A
at the end.
"""
import re
import sys
import time

import serial
import serial.tools.list_ports

GIGA_VID, GIGA_PID = 0x2341, 0x0266

STATUS_RE = re.compile(rb"<([A-Za-z:0-9]+)\|MPos:([-0-9.]+),([-0-9.]+),([-0-9.]+)(?:,([-0-9.]+))?(.*)>")


def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == GIGA_VID and p.pid == GIGA_PID:
            return p.device
    return None


class Grbl:
    def __init__(self, port=None, timeout=0.05, quiet=False):
        port = port or find_port()
        if port is None:
            sys.exit("No Arduino GIGA R1 port found (VID 2341 PID 0266).")
        self.port = port
        self.quiet = quiet
        self.ser = serial.Serial(port, 115200, timeout=timeout, write_timeout=2)
        self.buf = b""
        self.log = []          # every line received, for post-mortems
        self.status = []       # (t, state, x, y, z, rest, a) for every status frame

    # -- low level -----------------------------------------------------------

    def close(self):
        self.ser.close()

    def write(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.ser.write(data)

    def _pump(self):
        n = self.ser.in_waiting
        if n:
            self.buf += self.ser.read(n)
        else:
            b = self.ser.read(1)
            if b:
                self.buf += b

    def readline(self, timeout=1.0):
        """Returns one complete line (without CR/LF) or None on timeout."""
        t0 = time.monotonic()
        while True:
            i = self.buf.find(b"\n")
            if i >= 0:
                line = self.buf[:i].rstrip(b"\r")
                self.buf = self.buf[i + 1:]
                self.log.append(line)
                m = STATUS_RE.match(line)
                if m:
                    self.status.append((time.monotonic(), m.group(1).decode(),
                                        float(m.group(2)), float(m.group(3)), float(m.group(4)),
                                        m.group(6).decode(errors="replace"),
                                        float(m.group(5)) if m.group(5) else 0.0))
                return line
            if time.monotonic() - t0 > timeout:
                return None
            self._pump()

    def drain(self, quiet_for=0.2):
        """Reads until nothing has arrived for `quiet_for` seconds."""
        lines = []
        while True:
            l = self.readline(timeout=quiet_for)
            if l is None:
                return lines
            lines.append(l)

    # -- protocol ------------------------------------------------------------

    def reset(self, wait=2.0):
        """Ctrl-X, then waits for the Grbl banner. Returns the banner lines."""
        self.write(b"\x18")
        t0 = time.monotonic()
        banner = []
        seen = False
        while time.monotonic() - t0 < wait:
            l = self.readline(timeout=0.5)
            if l is None:
                if seen:
                    break
                continue
            banner.append(l)
            if l.startswith(b"Grbl "):
                seen = True
        return banner

    def cmd(self, line, timeout=5.0):
        """Sends one line and collects everything up to and including ok/error."""
        if isinstance(line, str):
            line = line.encode()
        self.write(line + b"\n")
        out = []
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            l = self.readline(timeout=timeout)
            if l is None:
                break
            out.append(l)
            if l == b"ok" or l.startswith(b"error:") or l.startswith(b"ALARM:"):
                return out
        raise TimeoutError("no ok for %r; got %r" % (line, out))

    def ok(self, line, timeout=5.0):
        out = self.cmd(line, timeout)
        if out[-1] != b"ok":
            raise RuntimeError("%r -> %r" % (line, out))
        return out

    def query(self, timeout=1.0):
        """Sends ? and returns the parsed status tuple, or None."""
        self.write(b"?")
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            l = self.readline(timeout=timeout)
            if l is None:
                break
            if l.startswith(b"<"):
                return self.status[-1] if self.status else None
        return None

    def wait_idle(self, timeout=120.0, poll=0.05):
        """Polls ? until the state is Idle (or Alarm/Hold, which also ends the wait)."""
        t0 = time.monotonic()
        last = None
        while time.monotonic() - t0 < timeout:
            s = self.query()
            if s:
                last = s
                if s[1].startswith("Idle") or s[1].startswith("Alarm"):
                    return s
            time.sleep(poll)
        raise TimeoutError("machine did not go idle; last %r" % (last,))

    def a_is_slave(self):
        return self.settings().get(42, 0) >= 2

    def settings(self):
        """Returns {number: value} from $$."""
        out = self.ok("$$")
        d = {}
        for l in out:
            m = re.match(rb"\$(\d+)=([-0-9.]+)", l)
            if m:
                d[int(m.group(1))] = float(m.group(2))
        return d

    def setup_bench(self, steps_per_mm=100, max_rate=6000, accel=1000, pulse_us=10):
        """Puts the controller into a known state for motion tests: no limits, no
        homing, generous travel, given step scale and limits on all axes. The
        step pulse ($0) must be shorter than the step period, so high-rate
        tests pass a small pulse_us."""
        self.reset()
        self.ok("$TIN=0,0")     # release inputs a previous test left pulled down
        time.sleep(0.05)        # ...and let the input debouncing see them released
        self.ok("$X")
        self.go_machine_zero()
        # No brake on the bench: no wait between enable and the first step.
        for n, v in [(0, pulse_us), (20, 0), (21, 0), (22, 0), (4, 0), (10, 1), (42, 1),
                     (48, 0), (49, 0), (50, 0), (1, 25),
                     (100, steps_per_mm), (101, steps_per_mm), (102, steps_per_mm), (103, steps_per_mm),
                     (110, max_rate), (111, max_rate), (112, max_rate), (113, max_rate),
                     (120, accel), (121, accel), (122, accel), (123, accel),
                     (130, 100000), (131, 100000), (132, 100000), (133, 100000)]:
            self.ok("$%d=%s" % (n, v))
        # Upstream Grbl keeps the parser and planner positions in mm and only
        # re-derives them from the step counters on a reset, so a change of
        # steps/mm while away from zero would leave them inconsistent (the
        # next absolute move would be the wrong length). A soft reset re-syncs.
        self.reset()
        self.ok("$X")
        self.ok("G21")
        self.ok("G90")
        self.ok("G92 X0 Y0 Z0 A0")

    def go_machine_zero(self, limit_mm=50.0):
        """Rapids back to machine zero if the test moves have carried the
        (virtual) machine far away. Grbl keeps positions in 32-bit floats, so
        tens of metres from origin a step is no longer representable and the
        final-position checks would fail for reasons that have nothing to do
        with the firmware. Uses its own fast rates; the caller sets the real
        ones afterwards."""
        s = self.wait_idle()
        if max(abs(s[2]), abs(s[3]), abs(s[4]), abs(s[6])) < limit_mm:
            return
        for n, v in [(20, 0), (110, 100000), (111, 100000), (112, 100000), (113, 100000),
                     (120, 5000), (121, 5000), (122, 5000), (123, 5000)]:
            self.ok("$%d=%s" % (n, v))
        self.ok("G53 G0 X0 Y0 Z0" + ("" if self.a_is_slave() else " A0"))
        self.wait_idle(timeout=600.0)


def say(*a):
    print(*a, flush=True)
