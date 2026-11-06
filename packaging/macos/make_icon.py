#!/usr/bin/env python3
"""Regenerate AppIcon.icns: a dark rounded square with three green
equalizer bars. Python stdlib only (PNG via zlib + struct), then macOS
`iconutil`. The .icns is committed, so builds never need this script.

    python3 packaging/macos/make_icon.py
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

# Geometry on Apple's 1024 grid: an 824 px plate with 100 px margins.
PLATE = (100.0, 100.0, 924.0, 924.0)
PLATE_R = 185.0
TOP, BOTTOM = (40, 40, 44), (18, 18, 20)    # plate gradient
GREEN = (46, 204, 113)
BAR_W = 112.0
BAR_GAP = 64.0
BAR_H = (300.0, 460.0, 220.0)               # left, middle, right
BAR_BASE = 512.0 + max(BAR_H) / 2          # bottom edge: tallest bar centred


def rrect_dist(x, y, x0, y0, x1, y1, r):
    """Signed distance from (x, y) to a rounded rectangle (< 0 inside)."""
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2 - r, (y1 - y0) / 2 - r
    dx, dy = abs(x - cx) - hx, abs(y - cy) - hy
    ox, oy = max(dx, 0.0), max(dy, 0.0)
    return (ox * ox + oy * oy) ** 0.5 + min(max(dx, dy), 0.0) - r


def bars():
    total = 3 * BAR_W + 2 * BAR_GAP
    x = 512.0 - total / 2
    for h in BAR_H:
        yield (x, BAR_BASE - h, x + BAR_W, BAR_BASE, BAR_W / 2)
        x += BAR_W + BAR_GAP


def render(size):
    """RGBA rows for one icon size, anti-aliased by signed distance."""
    s = size / 1024.0
    shapes = list(bars())
    rows = []
    for py in range(size):
        y = (py + 0.5) / s
        t = min(max((y - PLATE[1]) / (PLATE[3] - PLATE[1]), 0.0), 1.0)
        plate_rgb = tuple(a + (b - a) * t for a, b in zip(TOP, BOTTOM))
        row = bytearray()
        for px in range(size):
            x = (px + 0.5) / s
            # Distances in output pixels: one pixel of soft edge at any size.
            a_plate = min(max(0.5 - rrect_dist(x, y, *PLATE, PLATE_R) * s, 0.0), 1.0)
            if a_plate == 0.0:
                row += b"\0\0\0\0"
                continue
            a_bar = 0.0
            for b in shapes:
                a_bar = max(a_bar, min(max(0.5 - rrect_dist(x, y, *b) * s, 0.0), 1.0))
            rgb = [p + (g - p) * a_bar for p, g in zip(plate_rgb, GREEN)]
            row += bytes(int(c + 0.5) for c in rgb) + bytes([int(a_plate * 255 + 0.5)])
        rows.append(bytes(row))
    return rows


def write_png(path, size, rows):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    raw = b"".join(b"\0" + r for r in rows)  # filter type 0 per row
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    if not shutil.which("iconutil"):
        sys.exit("iconutil not found (macOS only)")
    work = tempfile.mkdtemp(dir=HERE)  # stays inside the repo, removed below
    try:
        iconset = os.path.join(work, "AppIcon.iconset")
        os.mkdir(iconset)
        cache = {}
        for base in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = base * scale
                if px not in cache:
                    cache[px] = render(px)
                name = "icon_%dx%d%s.png" % (base, base, "@2x" if scale == 2 else "")
                write_png(os.path.join(iconset, name), px, cache[px])
        out = os.path.join(HERE, "AppIcon.icns")
        subprocess.run(["iconutil", "-c", "icns", "-o", out, iconset], check=True)
        print("wrote", out)
    finally:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
