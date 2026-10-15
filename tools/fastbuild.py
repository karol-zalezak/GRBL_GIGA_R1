#!/usr/bin/env python3
"""
fastbuild.py - incremental rebuild without arduino-cli's library discovery

Part of Grbl-Giga. A development aid: arduino-cli preprocesses every source
file against the whole mbed include tree on every build just to rediscover
which libraries the sketch uses, which on a slow or heavily scanned disk takes
longer than the compilation itself. This script replays the compile commands
a previous verbose build printed, for the sources that changed since, then
relinks and converts to .bin.

    arduino-cli compile -v ... --build-path build/bp7 --output-dir build/m7 . > build/m7_log.txt
    python tools/fastbuild.py m7          # afterwards, after editing sources
    python tools/fastbuild.py m4

Only for edits to existing files; after adding or removing a source file (or
including a new library) run the full arduino-cli build again.
"""

import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else 'm7'
    log = os.path.join(ROOT, 'build', '%s_log.txt' % which)
    bp = os.path.join(ROOT, 'build', 'bp' + which[1])
    out_dir = os.path.join(ROOT, 'build', which)
    sketch_dir = os.path.join(bp, 'sketch')
    # arduino-cli prints the commands with every backslash doubled.
    lines = [l.replace('\\\\', '\\') for l in
             open(log, encoding='utf-8', errors='replace').read().splitlines()]

    compiles = []   # (source in build dir, object, command)
    link = None
    post = []
    for l in lines:
        if 'arm-none-eabi-g' in l and ' -c ' in l and '-o ' in l and sketch_dir.lower() in l.lower():
            objs = re.findall(r'-o\s+"([^"]+\.o)"', l)
            srcs = re.findall(r'"([^"]+\.(?:c|cpp|S))"', l)
            srcs = [s for s in srcs if s.lower().startswith(sketch_dir.lower())]
            if objs and srcs:
                compiles.append((srcs[-1], objs[0], l))
        elif 'arm-none-eabi-gcc' in l and '-Wl,-Map' in l:
            link = l
        elif 'arm-none-eabi-objcopy' in l:
            post.append(l)
    if not compiles or not link:
        sys.exit('no compile/link commands in %s - run a verbose arduino-cli build first' % log)

    # A source file added since the verbose build: compile it like a sibling of
    # the same type in the same directory and link its object next to the
    # sibling's. (Not for new libraries - those need the full build.)
    known = set(os.path.relpath(s, sketch_dir).lower() for s, _, _ in compiles)
    for dirpath, _, files in os.walk(os.path.join(ROOT, 'src')):
        for f in sorted(files):
            if not f.endswith(('.c', '.cpp')):
                continue
            rel = os.path.relpath(os.path.join(dirpath, f), ROOT)
            if rel.lower() in known:
                continue
            ext = os.path.splitext(f)[1]
            tmpl = [c for c in compiles
                    if os.path.dirname(os.path.relpath(c[0], sketch_dir)).lower() == os.path.dirname(rel).lower()
                    and c[0].endswith(ext)]
            if not tmpl:
                # A new directory: the sketch's compile commands differ only in
                # the file paths, so any file of the same type will do.
                tmpl = [c for c in compiles if c[0].endswith(ext) and not c[0].endswith('.ino.cpp')]
            if not tmpl:
                sys.exit('%s is new and has no file of its type to copy a command from - run the full build' % rel)
            t_src, t_obj, t_cmd = tmpl[0]
            n_src = os.path.join(sketch_dir, rel)
            n_obj = n_src + '.o'
            compiles.append((n_src, n_obj, t_cmd.replace(t_obj, n_obj).replace(t_src, n_src)))
            link = link.replace('"%s"' % t_obj, '"%s" "%s"' % (t_obj, n_obj), 1)
            known.add(rel.lower())
            print('new source %s (command from %s)' % (rel, os.path.relpath(t_src, sketch_dir)))

    todo = []
    for src, obj, cmd in compiles:
        rel = os.path.relpath(src, sketch_dir)
        if rel.endswith('.ino.cpp'):
            continue   # generated from the .ino; rebuild with arduino-cli if it changes
        orig = os.path.join(ROOT, rel)
        if not os.path.exists(orig):
            continue
        if (not os.path.exists(obj) or os.path.getmtime(orig) > os.path.getmtime(obj)
                or open(orig, 'rb').read() != open(src, 'rb').read()):
            os.makedirs(os.path.dirname(src), exist_ok=True)
            shutil.copyfile(orig, src)
            todo.append((rel, cmd))
    # A changed header means everything that includes it; be simple and
    # recompile all sources of that half when any header in src/ changed.
    headers_changed = False
    for dirpath, _, files in os.walk(os.path.join(ROOT, 'src')):
        for f in files:
            if f.endswith('.h'):
                p = os.path.join(dirpath, f)
                dst = os.path.join(sketch_dir, os.path.relpath(p, ROOT))
                if not os.path.exists(dst) or open(p, 'rb').read() != open(dst, 'rb').read():
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copyfile(p, dst)
                    headers_changed = True
    if headers_changed:
        todo = [(os.path.relpath(s, sketch_dir), c) for s, _, c in compiles
                if not s.endswith('.ino.cpp')]
        for s, _, _ in compiles:
            rel = os.path.relpath(s, sketch_dir)
            if os.path.exists(os.path.join(ROOT, rel)):
                shutil.copyfile(os.path.join(ROOT, rel), s)

    t0 = time.time()
    print('compiling %d file(s)' % len(todo))

    def run(item):
        rel, cmd = item
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        return rel, r.returncode, (r.stdout + r.stderr).strip()

    failed = False
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        for rel, rc, out in ex.map(run, todo):
            print(('  ok  ' if rc == 0 else '  ERR ') + rel)
            if out:
                print(out)
            failed |= rc != 0
    if failed:
        sys.exit(1)
    r = subprocess.run(link, shell=True, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit('link failed')
    for c in post:
        subprocess.run(c, shell=True, check=True)
    os.makedirs(out_dir, exist_ok=True)
    for ext in ('.bin', '.elf', '.hex', '.map'):
        f = os.path.join(bp, 'Grbl_Giga_R1.ino' + ext)
        if os.path.exists(f):
            shutil.copyfile(f, os.path.join(out_dir, 'Grbl_Giga_R1.ino' + ext))
    size = os.path.getsize(os.path.join(out_dir, 'Grbl_Giga_R1.ino.bin'))
    print('done in %.0f s, %s.bin %d bytes' % (time.time() - t0, which, size))


if __name__ == '__main__':
    main()
