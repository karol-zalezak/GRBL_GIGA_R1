#!/usr/bin/env python3
"""Files on the USB stick in the GIGA, from the PC - through Grbl's USB port.

The stick stays in the GIGA's USB-A port; the files go over the same serial
link a G-code sender uses (so the sender must let go of the port first).

    python usbfiles.py ls                       what is on the stick
    python usbfiles.py put part.nc "Front panel.nc"
    python usbfiles.py put *.nc                 (wildcards work on Windows too)
    python usbfiles.py rm old.nc
    python usbfiles.py sync D:\\CNC\\programs    copy what is new or changed
    python usbfiles.py sync D:\\CNC\\programs --delete   ...and remove the rest
    python usbfiles.py format                   format the stick (asks first)
    python usbfiles.py format --exfat           ...exFAT, also on a small stick

Options: --port COM5 (found by itself otherwise), --dry-run for sync,
--no-verify for put and sync (by default every copy is read back on the
GIGA and its CRC-32 compared with the PC file's).

A file keeps its date from the PC on the stick, which is how sync knows
what has changed: a file is copied when it is not on the stick, or its size
or date differ. Only G-code files are handled (.nc .ngc .gcode .gc .tap .txt
.cnc), from the root of the stick - what the GIGA's FILE screen lists.

Names: up to 47 characters; spaces and case are kept; accents are dropped
(the display has no font for them: "frézka.nc" goes as "frezka.nc"); '!',
'~' and '?' become '_' (the GIGA takes them for real-time commands).

Speed: about 150 kB/s over the link (hex), plus what the stick needs to
write; a 2 MB file takes a quarter of a minute.

Formatting: FAT32 up to 32 GB, exFAT above - what Windows makes too
(--exfat: exFAT whatever the size).
Everything on the stick is lost. It takes from seconds (exFAT) to a minute
or two (a big FAT32 stick).

Firmware side: $FW=name, $FW+<hex>, $FWE=<YYYYMMDDhhmmss>, $FWA, $FD=name,
$FT=name, $FF=ERASE, $FF=EXFAT, $F (src/job.c).
"""
import argparse
import glob
import os
import re
import sys
import time
import unicodedata
import zlib

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is missing:  pip install pyserial")

GIGA_VID, GIGA_PID = 0x2341, 0x0266
EXTENSIONS = (".nc", ".ngc", ".gcode", ".gc", ".tap", ".txt", ".cnc")
NAME_MAX = 47
CHUNK = 37              # bytes per $FW+ line: 4 + 74 hex digits, under Grbl's 80
IN_FLIGHT = 900         # bytes of lines sent ahead of their "ok" (Grbl's RX buffer is 1024)

FILE_RE = re.compile(r"\[FILE:([^|]+)\|SIZE:(\d+)(?:\|DATE:([-0-9T:]+))?\]")
READ_RE = re.compile(r"\[MSG:read .*: (\d+) bytes, \d+ lines, CRC32 ([0-9A-F]{8}) ")

ERRORS = {
    "8": "the machine is not idle (Idle or Alarm needed)",
    "46": "no USB stick in the GIGA",
    "47": "file error on the stick",
    "48": "a program from the stick is running",
}


class GrblError(Exception):
    pass


def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == GIGA_VID and p.pid == GIGA_PID:
            return p.device
    return None


class Link:
    def __init__(self, port):
        try:
            self.ser = serial.Serial(port, 115200, timeout=0.05, write_timeout=5)
        except serial.SerialException as e:
            sys.exit("Cannot open %s (%s).\nIs a G-code sender still connected to it?" % (port, e))
        self.buf = b""
        self.msgs = []
        time.sleep(0.1)
        self.ser.reset_input_buffer()
        self.command("")                    # sync: an empty line answers "ok"

    def close(self):
        self.ser.close()

    def write(self, text):
        self.ser.write(text.encode("ascii") + b"\n")

    def readline(self, timeout):
        t0 = time.monotonic()
        while True:
            i = self.buf.find(b"\n")
            if i >= 0:
                line = self.buf[:i].rstrip(b"\r").decode("utf-8", "replace")
                self.buf = self.buf[i + 1:]
                return line
            if time.monotonic() - t0 > timeout:
                return None
            n = self.ser.in_waiting
            self.buf += self.ser.read(n if n else 1)

    def answer(self, timeout=5.0):
        """Lines up to the next ok / error:n; returns (lines, None or error code)."""
        lines = []
        while True:
            l = self.readline(timeout)
            if l is None:
                raise GrblError("no answer from the GIGA")
            if l == "ok":
                return lines, None
            if l.startswith("error:"):
                return lines, l[6:]
            if l.startswith("[MSG:"):
                self.msgs.append(l)
            lines.append(l)

    def command(self, text, timeout=5.0):
        self.write(text)
        lines, err = self.answer(timeout)
        if err:
            raise GrblError(explain(err, lines))
        return lines


def explain(err, lines=()):
    why = ERRORS.get(err, "")
    msgs = [l[5:-1] for l in lines if l.startswith("[MSG:")]
    return "error:%s%s%s" % (err, " - " + why if why else "", "".join("\n  " + m for m in msgs))


# ---------------------------------------------------------------------------

def stick_name(path):
    """The name a PC file takes on the stick."""
    name = os.path.basename(path)
    name = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode("ascii")
    name = "".join("_" if (c in '!~?"*/:<>\\|' or ord(c) < 32) else c for c in name).strip()
    return name


def check_name(name, path):
    if not name.lower().endswith(EXTENSIONS):
        raise GrblError("%s: not a G-code file (%s)" % (path, " ".join(EXTENSIONS)))
    if len(name) > NAME_MAX:
        raise GrblError("%s: name longer than %d characters" % (path, NAME_MAX))
    if name.startswith("."):
        raise GrblError("%s: hidden file" % path)


def fat_date(mtime):
    """The PC file's local time as the stick keeps it (2 s steps)."""
    t = time.localtime(mtime)
    return time.strftime("%Y-%m-%dT%H:%M:", t) + "%02d" % (t.tm_sec - t.tm_sec % 2)


def listing(link):
    """[(name, size, date)] of the stick, and the [MSG:USB stick: ...] line."""
    lines = link.command("$F")
    files, state = [], ""
    for l in lines:
        m = FILE_RE.match(l)
        if m:
            files.append((m.group(1), int(m.group(2)), m.group(3)))
        elif l.startswith("[MSG:USB stick:"):
            state = l[5:-1]
    return files, state


def verify(link, name, data):
    """Reads the file back on the GIGA ($FT) and compares size and CRC-32."""
    for l in link.command("$FT=" + name, timeout=120):
        m = READ_RE.match(l)
        if m:
            if int(m.group(1)) != len(data) or int(m.group(2), 16) != zlib.crc32(data):
                raise GrblError("%s: the copy on the stick differs from the PC file (%s bytes, CRC32 %s)" %
                                (name, m.group(1), m.group(2)))
            return
    raise GrblError("%s: no read-back result" % name)


def put(link, path, name, check=True):
    data = open(path, "rb").read()
    date = time.strftime("%Y%m%d%H%M%S", time.localtime(os.path.getmtime(path)))
    t0 = time.monotonic()
    link.command("$FW=" + name, timeout=15)
    lines = ["$FW+" + data[i:i + CHUNK].hex().upper() for i in range(0, len(data), CHUNK)]
    sent = acked = 0
    inflight = []
    last_shown = 0
    try:
        while acked < len(lines):
            while sent < len(lines) and sum(inflight) + len(lines[sent]) + 1 < IN_FLIGHT:
                link.write(lines[sent])
                inflight.append(len(lines[sent]) + 1)
                sent += 1
            _, err = link.answer(timeout=15)
            if err:
                raise GrblError(explain(err, link.msgs[-1:]))
            inflight.pop(0)
            acked += 1
            now = time.monotonic()
            if now - last_shown > 0.5:
                last_shown = now
                done = min(acked * CHUNK, len(data))
                sys.stdout.write("\r  %-40s %3d %%  %6.0f kB/s" % (name[:40], 100 * done // max(len(data), 1),
                                                                 done / 1000 / max(now - t0, 1e-3)))
                sys.stdout.flush()
        link.command("$FWE=" + date, timeout=30)
    except (GrblError, KeyboardInterrupt):
        sys.stdout.write("\n")
        try:
            link.ser.reset_output_buffer()
            time.sleep(0.5)
            link.ser.reset_input_buffer()
            link.buf = b""
            link.command("$FWA")
        except GrblError:
            pass
        raise
    dt = time.monotonic() - t0
    sys.stdout.write("\r  %-40s %8d B  %5.1f s  %4.0f kB/s" % (name[:40], len(data), dt, len(data) / 1000 / max(dt, 1e-3)))
    if check:
        sys.stdout.flush()
        verify(link, name, data)
        sys.stdout.write("  verified")
    sys.stdout.write("\n")


def expand(patterns):
    paths = []
    for p in patterns:
        hits = glob.glob(p) if any(c in p for c in "*?[") else [p]
        if not hits:
            raise GrblError("%s: no such file" % p)
        paths += hits
    return paths


# ---------------------------------------------------------------------------

def cmd_ls(link, a):
    files, state = listing(link)
    print(state)
    for name, size, date in files:
        print("  %-47s %9d  %s" % (name, size, (date or "").replace("T", " ")))
    if "not shown" in state:
        print("  The GIGA lists 40 files at most (and names up to 47 characters).")


def cmd_put(link, a):
    paths = expand(a.files)
    if a.name and len(paths) != 1:
        raise GrblError("--name goes with one file only")
    for p in paths:
        name = stick_name(a.name or p)
        check_name(name, p)
        put(link, p, name, check=not a.no_verify)


def cmd_rm(link, a):
    for name in a.names:
        link.command("$FD=" + name, timeout=15)
        print("  deleted", name)


def cmd_sync(link, a):
    if not os.path.isdir(a.folder):
        raise GrblError("%s: not a folder" % a.folder)
    files, state = listing(link)
    if "ready" not in state:
        raise GrblError(state or "no USB stick")
    on_stick = {n.lower(): (n, size, date) for n, size, date in files}
    todo, same, skipped, wanted = [], 0, [], set()
    for fn in sorted(os.listdir(a.folder), key=str.lower):
        path = os.path.join(a.folder, fn)
        if not os.path.isfile(path) or not fn.lower().endswith(EXTENSIONS) or fn.startswith("."):
            continue
        name = stick_name(fn)
        try:
            check_name(name, fn)
        except GrblError as e:
            skipped.append(str(e))
            continue
        if name.lower() in wanted:
            skipped.append("%s: same stick name as another file (%s)" % (fn, name))
            continue
        wanted.add(name.lower())
        st = os.stat(path)
        have = on_stick.get(name.lower())
        if have and have[1] == st.st_size and have[2] == fat_date(st.st_mtime):
            same += 1
        else:
            todo.append((path, name, "new" if not have else "changed"))
    gone = [n for k, (n, _, _) in on_stick.items() if k not in wanted] if a.delete else []

    for s in skipped:
        print("  skipped:", s)
    print("%d up to date, %d to copy%s" % (same, len(todo), ", %d to delete" % len(gone) if a.delete else ""))
    for path, name, why in todo:
        if a.dry_run:
            print("  would copy %-40s (%s)" % (name, why))
        else:
            put(link, path, name, check=not a.no_verify)
    for name in gone:
        if a.dry_run:
            print("  would delete", name)
        else:
            link.command("$FD=" + name, timeout=15)
            print("  deleted", name)
    if len(wanted) > 40:
        print("Note: the GIGA lists 40 files at most; the folder has %d." % len(wanted))


def cmd_format(link, a):
    files, state = listing(link)
    print(state)
    if not a.yes:
        ans = input("Format the USB stick in the GIGA? Everything on it is lost.\nType ERASE to go on: ")
        if ans.strip() != "ERASE":
            raise GrblError("not formatted")
    for l in link.command("$FF=EXFAT" if a.exfat else "$FF=ERASE", timeout=900):
        if l.startswith("[MSG:"):
            print(l[5:-1])


def main():
    ap = argparse.ArgumentParser(description="Files on the USB stick in the Arduino GIGA (Grbl-Giga).")
    ap.add_argument("--port", help="serial port (default: the GIGA's, found by its USB ID)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ls", help="list the stick")
    p = sub.add_parser("put", help="copy files to the stick")
    p.add_argument("files", nargs="+")
    p.add_argument("--name", help="name on the stick (one file only)")
    p.add_argument("--no-verify", action="store_true", help="do not read the copy back")
    p = sub.add_parser("rm", help="delete files from the stick")
    p.add_argument("names", nargs="+")
    p = sub.add_parser("sync", help="copy new and changed files of a folder to the stick")
    p.add_argument("folder")
    p.add_argument("--delete", action="store_true", help="also delete G-code files the folder does not have")
    p.add_argument("--dry-run", action="store_true", help="only show what would happen")
    p.add_argument("--no-verify", action="store_true", help="do not read the copies back")
    p = sub.add_parser("format", help="format the stick (FAT32 up to 32 GB, exFAT above) - erases it")
    p.add_argument("--yes", action="store_true", help="do not ask")
    p.add_argument("--exfat", action="store_true", help="exFAT also on a stick of 32 GB or less")
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(errors="replace")      # names from the stick in a cp852/cp1250 console
    except AttributeError:
        pass

    port = a.port or find_port()
    if not port:
        sys.exit("No Arduino GIGA R1 found (USB ID 2341:0266); give --port.")
    link = Link(port)
    try:
        {"ls": cmd_ls, "put": cmd_put, "rm": cmd_rm, "sync": cmd_sync, "format": cmd_format}[a.cmd](link, a)
    except GrblError as e:
        sys.exit(str(e))
    except KeyboardInterrupt:
        sys.exit("interrupted")
    finally:
        link.close()


if __name__ == "__main__":
    main()
