#!/usr/bin/env python3
"""
screenshot.py - saves what the GIGA Display Shield shows as a PNG

Part of Grbl-Giga. Needs pyserial and Pillow:

    python tools/screenshot.py [--port COM5] [out.png]

Sends Grbl's port-specific `$SCREEN` command, which makes the M7 read the
display's framebuffer out of SDRAM (480x800 portrait, RGB565) and send it
over the USB port, and turns it into an 800x480 landscape image the way the
panel shows it. Grbl must be idle or in alarm; the transfer takes a second
or two.
"""

import argparse
import sys
import time

import serial
import serial.tools.list_ports
from PIL import Image

W, H = 480, 800


def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x2341 and p.pid == 0x0266:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default=None)
    ap.add_argument('out', nargs='?', default='screenshot.png')
    args = ap.parse_args()
    port = args.port or find_port()
    if not port:
        sys.exit('No GIGA R1 port found')

    s = serial.Serial(port, 115200, timeout=0.2)
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write(b'$SCREEN\n')
    data = b''
    t0 = time.time()
    header = b'[SCREEN:480x800:RGB565:'
    while time.time() - t0 < 20:
        data += s.read(65536)
        i = data.find(header)
        if i >= 0:
            j = data.find(b']\r\n', i)
            if j > 0:
                n = int(data[i + len(header):j])
                start = j + 3
                if len(data) >= start + n:
                    fb = data[start:start + n]
                    break
        if b'error:' in data and header not in data:
            sys.exit('Grbl refused: ' + data.decode(errors='replace').strip())
    else:
        sys.exit('timed out (%d bytes received)' % len(data))
    s.close()

    # RGB565 little-endian -> RGB888, portrait as stored.
    px = bytearray(W * H * 3)
    for k in range(W * H):
        v = fb[2 * k] | (fb[2 * k + 1] << 8)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        px[3 * k] = (r << 3) | (r >> 2)
        px[3 * k + 1] = (g << 2) | (g >> 4)
        px[3 * k + 2] = (b << 3) | (b >> 2)
    img = Image.frombytes('RGB', (W, H), bytes(px))
    # The UI draws landscape (lx, ly) at panel (479 - ly, lx).
    img = img.transpose(Image.Transpose.ROTATE_90)
    img.save(args.out)
    print('saved %s %dx%d' % (args.out, img.size[0], img.size[1]))


if __name__ == '__main__':
    main()
