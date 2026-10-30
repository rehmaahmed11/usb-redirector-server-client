#!/usr/bin/env python3
"""Generates src/ui/app.ico (rounded blue tile + white USB glyph) with no dependencies.

Run: python3 tools/make_icon.py
"""
import os
import struct
import zlib


def rounded_rect_mask(w, h, r):
    mask = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            cx = min(max(x, r), w - 1 - r)
            cy = min(max(y, r), h - 1 - r)
            inside_corner = (x - cx) ** 2 + (y - cy) ** 2 <= r * r
            inside_body = (r <= x < w - r) or (r <= y < h - r)
            mask[y][x] = inside_corner and inside_body or (inside_body and inside_corner)
    # simpler: keep points inside the rounded rectangle
    for y in range(h):
        for x in range(w):
            cx = min(max(x, r), w - 1 - r)
            cy = min(max(y, r), h - 1 - r)
            mask[y][x] = (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return mask


def draw(size):
    px = [[(0, 0, 0, 0)] * size for _ in range(size)]
    r = max(2, size // 6)
    mask = rounded_rect_mask(size, size, r)
    for y in range(size):
        for x in range(size):
            if mask[y][x]:
                t = y / size
                px[y][x] = (int(37 + 8 * t), int(99 - 10 * t), int(235 - 6 * t), 255)

    def setpx(x, y, col):
        if 0 <= x < size and 0 <= y < size:
            px[y][x] = col

    def disc(cx, cy, rad, col):
        for y in range(int(cy - rad), int(cy + rad) + 2):
            for x in range(int(cx - rad), int(cx + rad) + 2):
                if (x - cx) ** 2 + (y - cy) ** 2 <= rad * rad:
                    setpx(x, y, col)

    def line(x0, y0, x1, y1, thick, col):
        steps = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
        for i in range(steps + 1):
            t = i / steps
            disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, thick / 2.0, col)

    W = (255, 255, 255, 255)
    s = size
    cx = s / 2.0
    line(cx, s * 0.30, cx, s * 0.72, s * 0.075, W)
    # arrow head pointing up (narrow at the top, wide where it meets the stem)
    for i in range(int(s * 0.13)):  # arrow head
        half = (s * 0.13) * (i / (s * 0.13))
        for x in range(int(cx - half), int(cx + half) + 1):
            setpx(x, int(s * 0.20) + i, W)
    line(cx, s * 0.46, cx - s * 0.20, s * 0.58, s * 0.055, W)
    line(cx, s * 0.56, cx + s * 0.20, s * 0.44, s * 0.055, W)
    disc(cx - s * 0.20, s * 0.58, s * 0.075, W)
    disc(cx + s * 0.20, s * 0.44, s * 0.075, W)
    disc(cx, s * 0.74, s * 0.085, W)
    return px


def png_bytes(px):
    h = len(px)
    w = len(px[0])
    raw = b""
    for row in px:
        raw += b"\x00" + b"".join(struct.pack("BBBB", *p) for p in row)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    sizes = [16, 32, 48, 64, 128, 256]
    images = [(s, png_bytes(draw(s))) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    data = b""
    for s, png in images:
        entries += struct.pack("<BBBBHHII", 0 if s >= 256 else s, 0 if s >= 256 else s, 0, 0, 1, 32, len(png),
                               offset)
        offset += len(png)
        data += png
    os.makedirs("src/ui", exist_ok=True)
    path = "src/ui/app.ico"
    with open(path, "wb") as f:
        f.write(header + entries + data)
    print("wrote", path, os.path.getsize(path), "bytes")


if __name__ == "__main__":
    main()
