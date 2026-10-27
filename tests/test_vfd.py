#!/usr/bin/env python3
"""Spindle VFD over RS485 / Modbus RTU for Grbl-Giga on the GIGA R1 (src/vfd.h).

Two ways to have a drive:

  (default)        the firmware's test hook - a simulated Delta drive takes
                   the place of the RS485 port and gets the very frames the
                   port would send:
                     $TVFD=1          healthy (output ramps at 400 Hz/s); a
                                      reported error stays latched until the
                                      drive's fault reset
                     $TVFD=2,code     in fault with that error code, the
                                      fault reset ignored while it lasts
                     $TVFD=3          silent
                     $TVFD=4          every second answer broken (bad CRC)
                     $TVFD=5          answers, but ignores run/stop (keypad)
                     $TVFD=0          off: the RS485 port again
  --rs485 COMx     this PC is the Delta drive, on a USB-RS485 adapter wired to
                   the shield's RS485 terminals (A-A, B-B, GND) - or, without
                   the shield, a 3.3 V USB-UART on D18 (TX) / D19 (RX). That
                   tests the real USART2, the frames on the wire and the
                   driver enable; the tests that need a fault, a garbled
                   answer or a keypad drive are then played by the PC drive.

and $VFD reports what Grbl believes:
  [VFD:Delta link=OK want=FWD,200.00 drive=FWD,200.00,199.50 err=0 st=0503
   remote=3 at=1 trouble=0 tx=.. ok=.. retry=.. fail=.. exc=n/code sim=1 clk=120]

  run       M3/M4/M5, S scaled by $30/$37 and limited by $31/$30, the override
  atspeed   $52 waits for the drive's output frequency; too slow = ALARM:14
  fault     a drive fault: ALARM:19, $X refused, the fault reset clears it
  silent    a silent drive: nothing while off; ALARM:23 when running, $X refused
  cut       the drive goes silent while cutting: hold, then ALARM:23, position kept
  garble    every second answer broken: retries, no alarm
  keypad    a drive that ignores run: ALARM:23 after VFD_CONFIRM_MS
  refused   a frequency the drive refuses (exception 3): ALARM:23
  raw       $VFD=R / $VFD=W
  interlock collet closed while the drive has output; $32 and $33 exclusive

    python test_vfd.py [--port COM5] [--only run,fault,...]
    python test_vfd.py --rs485 COM7 [--baud 9600] [--frame 8N2] [--echo]
"""
import argparse
import re
import threading
import time

from grbl_link import Grbl, say

FRAMES = ["8N1", "8N2", "8E1", "8O1", "8E2", "8O2"]


def check(cond, what):
    cond = bool(cond)
    say(("  ok    " if cond else "  FAIL  ") + what)
    return cond


# ---------------------------------------------------------------------------
# A Delta drive on the PC's RS485 adapter (--rs485)
# ---------------------------------------------------------------------------

def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def with_crc(data):
    c = crc16(data)
    return bytes(data) + bytes([c & 0xFF, c >> 8])


class DeltaDrive(threading.Thread):
    """Answers like a Delta drive with 01-00 = 600 Hz: 2000H-2002H written,
    2100H-2104H read, anything else exception 2, a frequency above 600 Hz
    exception 3. Requests from Grbl are always 8 bytes (03 or 06)."""

    def __init__(self, port, baud, frame, addr=1, echo=False):
        super().__init__(daemon=True)
        import serial
        parity = {"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN, "O": serial.PARITY_ODD}[frame[1]]
        self.ser = serial.Serial(port, baud, bytesize=8, parity=parity, stopbits=int(frame[2]), timeout=0.01)
        self.addr, self.echo = addr, echo
        self.lock = threading.Lock()
        self.running, self.dir, self.fcmd, self.fout, self.err = False, 1, 0, 0, 0
        self.mode = 1              # as $TVFD: 1 ok, 2 fault, 3 silent, 4 garble, 5 keypad
        self.flip = 0
        self.t = time.perf_counter()
        self.requests, self.bad_crc, self.answers = 0, 0, 0
        self.log = []              # (t, request bytes)
        self.stop_flag = False

    def set_mode(self, mode, code=2):
        with self.lock:
            self.ramp()
            self.mode = mode
            if mode == 2:
                self.err, self.running, self.fout = code, False, 0

    def ramp(self):
        now = time.perf_counter()
        step = int((now - self.t) * 40000)          # 400 Hz/s in 0.01 Hz
        self.t = now
        target = self.fcmd if self.running else 0
        if self.fout < target:
            self.fout = min(target, self.fout + step)
        elif self.fout > target:
            self.fout = max(target, self.fout - step)

    def reg(self, r):
        if r == 0x2100:
            return self.err
        if r == 0x2101:
            st = (3 if self.fcmd else 2) if self.running else (1 if self.fout else 0)
            if self.dir == 2:
                st |= 3 << 3
            if self.mode != 5:
                st |= (1 << 8) | (1 << 10)
            return st
        return {0x2102: self.fcmd, 0x2103: self.fout, 0x2104: self.fout // 400}.get(r, 0)

    def answer(self, q):
        fc, reg, v = q[1], (q[2] << 8) | q[3], (q[4] << 8) | q[5]
        exc = 0
        if fc == 6:
            if reg == 0x2000:
                if self.mode != 5 and not self.err:
                    d = (v >> 4) & 3
                    if d in (1, 2):
                        self.dir = d
                    elif d == 3:
                        self.dir = 3 - self.dir
                    if v & 3 == 1:
                        self.running = False
                    elif v & 3 >= 2:
                        self.running = True
            elif reg == 0x2001:
                if v > 60000:
                    exc = 3
                else:
                    self.fcmd = v
            elif reg == 0x2002:
                if v & 2 and self.mode != 2:
                    self.err = 0
            else:
                exc = 2
            r = bytes(q[:6]) if not exc else None
        elif fc == 3:
            if v == 0 or v > 5 or reg < 0x2100 or reg + v > 0x2105:
                exc, r = 2, None
            else:
                r = bytes([q[0], 3, 2 * v]) + b"".join(self.reg(reg + k).to_bytes(2, "big") for k in range(v))
        else:
            exc, r = 1, None
        if exc:
            r = bytes([q[0], fc | 0x80, exc])
        r = with_crc(r)
        if self.mode == 4:
            self.flip ^= 1
            if self.flip:
                r = r[:-1] + bytes([r[-1] ^ 0x5A])
        return r

    def run(self):
        buf = b""
        while not self.stop_flag:
            buf += self.ser.read(64)
            while len(buf) >= 8:
                q = buf[:8]
                if crc16(q[:6]) != (q[6] | (q[7] << 8)):
                    self.bad_crc += 1
                    buf = buf[1:]               # resynchronise
                    continue
                buf = buf[8:]
                with self.lock:
                    self.ramp()
                    self.requests += 1
                    self.log.append((time.perf_counter(), q))
                    if q[0] != self.addr or self.mode == 3:
                        continue
                    r = self.answer(q)
                time.sleep(0.002)                   # Delta's 09-09 response delay
                self.ser.write(r)
                self.answers += 1
                if self.echo:
                    self.ser.read(len(r))

    def close(self):
        self.stop_flag = True
        self.join(1.0)
        self.ser.close()


# ---------------------------------------------------------------------------
# The drive the tests talk to: the hook, or the PC drive
# ---------------------------------------------------------------------------

class Drive:
    def __init__(self, g, pc=None):
        self.g, self.pc = g, pc

    def mode(self, m, code=2):
        if self.pc:
            self.pc.set_mode(m, code)
        else:
            self.g.ok("$TVFD=%d,%d" % (m, code) if m == 2 else "$TVFD=%d" % m)


def vfd(g):
    for l in g.ok("$VFD"):
        if l.startswith(b"[VFD:") and b" link=" in l:
            d = {}
            for kv in l[5:-1].decode().split()[1:]:
                k, v = kv.split("=", 1)
                d[k] = v
            return d
    raise RuntimeError("no [VFD:] line")


def hz(s):
    """'FWD,200.00' -> ('FWD', 200.0); 'FWD,200.00,199.50' -> ('FWD', 200.0, 199.5)."""
    if s == "-":
        return (None, -1.0, -1.0)
    p = s.split(",")
    return (p[0],) + tuple(float(x) for x in p[1:])


def wait_vfd(g, pred, timeout=3.0):
    t0 = time.perf_counter()
    v = None
    while time.perf_counter() - t0 < timeout:
        v = vfd(g)
        if pred(v):
            return v
        time.sleep(0.05)
    return v


def state(g):
    s = g.query()
    return s[1] if s else ""


def wait_alarm(g, code, n0, timeout=3.0):
    """True once ALARM:code has been reported since g.log[n0] and the state is Alarm."""
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < timeout:
        st = state(g)
        if st.startswith("Alarm") and any(l == b"ALARM:%d" % code for l in g.log[n0:]):
            return True
        time.sleep(0.05)
    return False


def unlock(g):
    out = g.cmd("$X")
    return out[-1]


def spindle_off(g):
    g.cmd("M5")


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def t_run(g, drv):
    say("\nM3 / M4 / M5, S and the override:")
    ok = True
    v0 = vfd(g)
    g.ok("M3 S12000")
    v = wait_vfd(g, lambda v: hz(v["drive"])[0] == "FWD" and abs(hz(v["drive"])[2] - 200.0) < 0.01)
    ok &= check(v["want"] == "FWD,200.00" and hz(v["drive"]) == ("FWD", 200.0, 200.0),
                "M3 S12000 ($30=24000, $37=400): want %s, drive %s" % (v["want"], v["drive"]))
    g.ok("M4 S6000")
    v = wait_vfd(g, lambda v: hz(v["drive"])[0] == "REV" and abs(hz(v["drive"])[2] - 100.0) < 0.01)
    ok &= check(hz(v["drive"]) == ("REV", 100.0, 100.0), "M4 S6000: drive %s" % v["drive"])
    g.write(b"\x9A")                                 # spindle override +10 %
    v = wait_vfd(g, lambda v: v["want"] == "REV,110.00")
    ok &= check(v["want"] == "REV,110.00", "override +10 %%: want %s" % v["want"])
    v = wait_vfd(g, lambda v: hz(v["drive"])[1] == 110.0)
    ok &= check(hz(v["drive"])[1] == 110.0, "  the drive got it: set point %s" % v["drive"])
    g.write(b"\x99")                                 # override back to 100 %
    v = wait_vfd(g, lambda v: v["want"] == "REV,100.00")
    ok &= check(v["want"] == "REV,100.00", "override reset: want %s" % v["want"])
    g.ok("$31=6000")
    g.ok("S100")
    v = wait_vfd(g, lambda v: hz(v["drive"])[1] == 100.0)
    ok &= check(v["want"] == "REV,100.00", "S100 with $31=6000: limited to $31 -> want %s" % v["want"])
    g.ok("$31=0")
    g.ok("S30000")
    v = wait_vfd(g, lambda v: hz(v["drive"])[1] == 400.0)
    ok &= check(v["want"] == "REV,400.00" and hz(v["drive"])[1] == 400.0,
                "S30000 above $30: limited to $30 -> want %s, drive %s" % (v["want"], v["drive"]))
    g.ok("S0")
    v = wait_vfd(g, lambda v: hz(v["drive"])[0] == "STOP")
    ok &= check(v["want"].startswith("STOP") and hz(v["drive"])[0] == "STOP", "S0: stop (drive %s)" % v["drive"])
    g.ok("M3 S12000")
    wait_vfd(g, lambda v: hz(v["drive"])[0] == "FWD")
    g.ok("M5")
    v = wait_vfd(g, lambda v: hz(v["drive"])[0] == "STOP" and hz(v["drive"])[2] == 0.0)
    ok &= check(hz(v["drive"]) == ("STOP", 200.0, 0.0), "M5: stopped, output down to 0 (drive %s)" % v["drive"])
    fails = int(v["fail"]) - int(v0["fail"])
    ok &= check(v["trouble"] == "0" and fails == 0 and v["remote"] == "3",
                "no trouble, no failed transaction, both sources RS485 (%d failed, remote=%s)" % (fails, v["remote"]))
    return ok


def t_atspeed(g, drv):
    say("\nat speed ($52) from the drive's output frequency:")
    ok = True
    g.ok("$52=5000")
    t0 = time.perf_counter()
    g.ok("M3 S24000", timeout=8)
    dt = time.perf_counter() - t0
    v = vfd(g)
    ok &= check(0.9 < dt < 1.6 and v["at"] == "1",
                "M3 S24000 (0 -> 400 Hz at 400 Hz/s): ok after %.2f s, at=%s, drive %s" % (dt, v["at"], v["drive"]))
    t0 = time.perf_counter()
    g.ok("S12000", timeout=8)
    dt = time.perf_counter() - t0
    ok &= check(0.4 < dt < 1.1, "S12000 (400 -> 200 Hz): ok after %.2f s" % dt)
    g.ok("M5")
    wait_vfd(g, lambda v: hz(v["drive"])[2] == 0.0)
    g.ok("$52=500")
    n0 = len(g.log)
    g.cmd("M3 S24000", timeout=5)
    ok &= check(wait_alarm(g, 14, n0), "$52=500 and a 1 s ramp: ALARM:14")
    v = wait_vfd(g, lambda v: v["want"].startswith("STOP"))
    ok &= check(v["want"].startswith("STOP"), "  spindle told to stop (want %s)" % v["want"])
    ok &= check(unlock(g) == b"ok", "  $X")
    g.ok("$52=0")
    wait_vfd(g, lambda v: hz(v["drive"])[2] == 0.0, timeout=3)
    return ok


def t_fault(g, drv):
    say("\nfault reported by the drive:")
    ok = True
    g.ok("M3 S12000")
    wait_vfd(g, lambda v: hz(v["drive"])[0] == "FWD")
    n0 = len(g.log)
    drv.mode(2, 7)
    ok &= check(wait_alarm(g, 19, n0), "drive error 7: ALARM:19")
    v = vfd(g)
    ok &= check(v["err"] == "7" and v["want"].startswith("STOP"), "  err=%s, want %s" % (v["err"], v["want"]))
    r = unlock(g)
    ok &= check(r == b"error:43", "  $X while the cause lasts: %s (the fault reset it sends does nothing)" % r.decode())
    drv.mode(1)                                       # cause gone, error still latched
    time.sleep(0.3)
    v = vfd(g)
    ok &= check(v["err"] == "7", "  cause gone: still latched (err=%s)" % v["err"])
    r = unlock(g)
    ok &= check(r == b"error:43", "  $X: %s - and sends the fault reset" % r.decode())
    v = wait_vfd(g, lambda v: v["err"] == "0")
    ok &= check(v["err"] == "0", "  the drive cleared its fault (err=%s)" % v["err"])
    time.sleep(0.05)
    ok &= check(unlock(g) == b"ok", "  $X: ok")
    return ok


def t_silent(g, drv):
    say("\nsilent drive:")
    ok = True
    drv.mode(3)
    v = wait_vfd(g, lambda v: v["link"] == "LOST", timeout=2)
    time.sleep(0.3)
    ok &= check(v["link"] == "LOST" and state(g).startswith("Idle"),
                "spindle off: link %s, still %s (no alarm)" % (v["link"], state(g)))
    n0 = len(g.log)
    g.cmd("M3 S12000")
    ok &= check(wait_alarm(g, 23, n0), "M3 to a silent drive: ALARM:23")
    ok &= check(unlock(g) == b"ok", "  $X: ok (no run command ever went out)")
    g.cmd("M5")
    drv.mode(1)
    v = wait_vfd(g, lambda v: v["link"] == "OK", timeout=2)
    ok &= check(v["link"] == "OK", "drive back: link %s" % v["link"])
    g.ok("M3 S12000")
    wait_vfd(g, lambda v: hz(v["drive"])[0] == "FWD")
    n0 = len(g.log)
    drv.mode(3)
    t0 = time.perf_counter()
    ok &= check(wait_alarm(g, 23, n0), "running, drive goes silent: ALARM:23 after %.2f s" % (time.perf_counter() - t0))
    r = unlock(g)
    ok &= check(r == b"error:51", "  $X refused while the spindle may still run (%s)" % r.decode())
    drv.mode(1)
    v = wait_vfd(g, lambda v: v["link"] == "OK" and hz(v["drive"])[0] == "STOP", timeout=2)
    ok &= check(v["link"] == "OK" and hz(v["drive"])[0] == "STOP", "  drive back: told to stop (drive %s)" % v["drive"])
    ok &= check(unlock(g) == b"ok", "  $X: ok")
    return ok


def t_cut(g, drv):
    say("\ndrive silent while cutting:")
    ok = True
    g.ok("G92 X0")
    g.ok("M3 S12000")
    wait_vfd(g, lambda v: hz(v["drive"])[0] == "FWD")
    g.ok("G1 X50 F600")                      # 5 s
    time.sleep(1.0)
    n0 = len(g.log)
    drv.mode(3)
    t0 = time.perf_counter()
    states = set()
    while time.perf_counter() - t0 < 3.0:
        s = g.query()
        if s:
            states.add(s[1].split(":")[0])
            if s[1].startswith("Alarm"):
                break
        time.sleep(0.03)
    x = g.query()[2]
    ok &= check("Run" in states and "Alarm" in states and any(l == b"ALARM:23" for l in g.log[n0:]),
                "ALARM:23 while cutting (states seen %s)" % sorted(states))
    ok &= check(10.0 < x < 25.0, "  stopped under control at X=%.3f (feed hold, position kept)" % x)
    drv.mode(1)
    wait_vfd(g, lambda v: v["link"] == "OK" and hz(v["drive"])[0] == "STOP", timeout=2)
    ok &= check(unlock(g) == b"ok", "  drive back, $X: ok")
    g.ok("G90 G0 X0")
    g.wait_idle()
    return ok


def t_garble(g, drv):
    say("\nevery second answer broken:")
    ok = True
    v0 = vfd(g)
    drv.mode(4)
    g.ok("M3 S12000")
    v = wait_vfd(g, lambda v: hz(v["drive"])[0] == "FWD" and hz(v["drive"])[2] == 200.0, timeout=3)
    g.ok("M5")
    time.sleep(0.5)
    drv.mode(1)
    v = vfd(g)
    retries = int(v["retry"]) - int(v0["retry"])
    ok &= check(hz(v["drive"])[0] == "STOP" and retries > 3 and v["fail"] == v0["fail"],
                "M3, M5 went through: %d retries, no failed transaction, state %s" % (retries, state(g)))
    return ok


def t_keypad(g, drv):
    say("\ndrive that ignores the run command (keypad control):")
    ok = True
    drv.mode(5)
    t0 = time.perf_counter()
    n0 = len(g.log)
    g.ok("M3 S12000")
    ok &= check(wait_alarm(g, 23, n0), "ALARM:23 after %.2f s" % (time.perf_counter() - t0))
    v = vfd(g)
    ok &= check(v["remote"] == "0", "  $VFD remote=%s (run/frequency not from RS485)" % v["remote"])
    drv.mode(1)
    wait_vfd(g, lambda v: v["trouble"] == "0", timeout=2)
    ok &= check(unlock(g) == b"ok", "  back under RS485 control, $X: ok")
    return ok


def t_refused(g, drv):
    say("\nfrequency the drive refuses:")
    ok = True
    g.ok("$37=650")
    n0 = len(g.log)
    g.ok("M3 S24000")                         # 650 Hz > the drive's 600 Hz
    ok &= check(wait_alarm(g, 23, n0), "650 Hz: ALARM:23")
    v = vfd(g)
    ok &= check(v["exc"].endswith("/3"), "  exception 3 (exc=%s)" % v["exc"])
    g.ok("$37=400")
    wait_vfd(g, lambda v: v["trouble"] == "0", timeout=2)
    ok &= check(unlock(g) == b"ok", "  $37=400, $X: ok")
    return ok


def t_raw(g, drv):
    say("\n$VFD=R / $VFD=W:")
    ok = True
    g.ok("M3 S12000")
    wait_vfd(g, lambda v: hz(v["drive"])[2] == 200.0)
    out = g.ok("$VFD=R2100,4")
    line = next((l for l in out if l.startswith(b"[VFD:R2100=")), b"")
    regs = [int(x, 16) for x in line[11:-1].split(b",")] if line else []
    ok &= check(len(regs) == 4 and regs[0] == 0 and regs[1] & 3 == 3 and regs[2] == 20000,
                "$VFD=R2100,4 -> %s" % line.decode())
    g.ok("M5")
    out = g.ok("$VFD=W2001,1388")
    ok &= check(any(l == b"[VFD:W2001=1388]" for l in out), "$VFD=W2001,1388 -> %s" % out[0].decode())
    r = g.cmd("$VFD=R1234")
    ok &= check(r[-1] == b"error:52" and any(b"exception 2" in l for l in r),
                "$VFD=R1234 (no such register): %s" % b" ".join(r).decode())
    r = g.cmd("$VFD=Q1")
    ok &= check(r[-1] == b"error:3", "$VFD=Q1: %s" % r[-1].decode())
    return ok


def t_interlock(g, drv):
    say("\ncollet and settings:")
    ok = True
    s = g.settings()
    g.ok("$51=0")
    if s.get(90, 0):
        g.ok("M3 S24000")
        wait_vfd(g, lambda v: hz(v["drive"])[2] > 300.0)
        g.ok("M5")
        r = g.cmd("$COLLET=1")
        ok &= check(r[-1] == b"error:44", "collet while the drive runs down: %s" % r[-1].decode())
        wait_vfd(g, lambda v: hz(v["drive"])[2] == 0.0, timeout=3)
        time.sleep(0.2)
        r = g.cmd("$COLLET=1")
        ok &= check(r[-1] == b"ok", "collet once the drive stands: %s" % r[-1].decode())
        g.ok("$COLLET=0")
    else:
        say("  (no collet output assigned, skipped)")
    g.ok("$51=%d" % s.get(51, 5000))
    r = g.cmd("$32=1")
    ok &= check(r[-1] == b"error:5", "$32=1 (laser) with $33=1: %s" % r[-1].decode())
    g.ok("M3 S12000")
    r = g.cmd("$34=2")
    ok &= check(r[-1] == b"error:8", "$34 while the spindle runs: %s" % r[-1].decode())
    g.ok("M5")
    return ok


def prepare(g, drv):
    """A healthy drive, no latched error, Grbl idle."""
    drv.mode(1)
    g.reset()
    v = wait_vfd(g, lambda v: v["link"] == "OK", timeout=2)
    if v["err"] != "0":
        g.cmd("$X")                       # sends the fault reset
        wait_vfd(g, lambda v: v["err"] == "0", timeout=2)
    wait_vfd(g, lambda v: v["trouble"] == "0", timeout=2)
    time.sleep(0.05)
    if state(g).startswith("Alarm"):
        g.cmd("$X")


TESTS = [("run", t_run), ("atspeed", t_atspeed), ("fault", t_fault), ("silent", t_silent),
         ("cut", t_cut), ("garble", t_garble), ("keypad", t_keypad), ("refused", t_refused),
         ("raw", t_raw), ("interlock", t_interlock)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--only")
    ap.add_argument("--rs485", help="COM port of a USB-RS485 adapter: this PC is the drive")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--frame", default="8N2", choices=FRAMES)
    ap.add_argument("--echo", action="store_true", help="the adapter echoes what it sends")
    a = ap.parse_args()

    g = Grbl(a.port)
    pc = None
    if a.rs485:
        pc = DeltaDrive(a.rs485, a.baud, a.frame, echo=a.echo)
        pc.start()
    drv = Drive(g, pc)
    say("Grbl on %s, drive: %s" % (g.port, ("PC on %s, %d %s" % (a.rs485, a.baud, a.frame)) if pc else "$TVFD (simulated)"))
    keep = g.settings()
    try:
        g.setup_bench()
        g.ok("$TVFD=0" if pc else "$TVFD=1")
        for n, v in [(30, 24000), (31, 0), (33, 1), (34, 1), (35, a.baud), (36, FRAMES.index(a.frame)),
                     (37, 400), (52, 0), (54, 0)]:
            g.ok("$%d=%s" % (n, v))
        v = wait_vfd(g, lambda v: v["link"] == "OK", timeout=3)
        if not check(v["link"] == "OK", "link up: %s" % " ".join("%s=%s" % kv for kv in v.items())):
            return 1
        only = a.only.split(",") if a.only else [n for n, _ in TESTS]
        results = {}
        for name, fn in TESTS:
            if name not in only:
                continue
            prepare(g, drv)
            results[name] = fn(g, drv)
            spindle_off(g)
        if pc:
            say("\nPC drive: %d requests, %d with a bad CRC, %d answers" % (pc.requests, pc.bad_crc, pc.answers))
            if pc.log:
                gaps = [b[0] - a_[0] for a_, b in zip(pc.log, pc.log[1:])]
                say("  shortest gap between requests %.1f ms" % (min(gaps) * 1000 if gaps else 0))
            results["wire"] = check(pc.bad_crc == 0, "no request with a bad CRC on the wire")
        say("\n" + ", ".join("%s %s" % (n, "ok" if r else "FAIL") for n, r in results.items()))
        return 0 if all(results.values()) else 1
    finally:
        try:
            g.reset()
            g.cmd("$X")
            g.ok("$TVFD=0")
            for n in (33, 34, 35, 36, 37, 30, 31, 52, 54):
                if n in keep:
                    r = g.cmd("$%d=%s" % (n, ("%g" % keep[n])))
                    if r[-1] != b"ok":
                        say("  could not restore $%d=%g: %s" % (n, keep[n], r[-1].decode()))
        finally:
            if pc:
                pc.close()
            g.close()


if __name__ == "__main__":
    raise SystemExit(main())
