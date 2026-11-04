#!/usr/bin/env python3
"""Generate assets/feather.ico (a page with a feather) using only the standard library."""
import math
import struct
import sys
import zlib


def png(size, pixels):
    raw = b"".join(b"\x00" + bytes(pixels[y * size * 4:(y + 1) * size * 4]) for y in range(size))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def rounded_box(x, y, x0, y0, x1, y1, r):
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return r - math.hypot(x - cx, y - cy)  # >0 inside


def render(size):
    ss = 4  # supersampling
    px = [0] * (size * size * 4)
    for y in range(size):
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    u = (x + (sx + 0.5) / ss) / size
                    v = (y + (sy + 0.5) / ss) / size
                    col = None
                    # page
                    fold = 0.24
                    inside = rounded_box(u, v, 0.16, 0.06, 0.84, 0.94, 0.06) > 0 and not (u > 0.84 - fold and v < 0.06 + fold and (u - (0.84 - fold)) > (v - 0.06))
                    if inside:
                        col = (255, 255, 255)
                        # folded corner
                        if u > 0.84 - fold and v < 0.06 + fold and (u - (0.84 - fold)) <= (v - 0.06):
                            col = (205, 214, 224)
                        # red band
                        if 0.70 < v < 0.84:
                            col = (220, 53, 69)
                        # feather: a curved quill from bottom-left to upper-right
                        t = (u - 0.28) / 0.44
                        if 0 <= t <= 1:
                            cy = 0.62 - 0.40 * t + 0.06 * math.sin(t * math.pi)
                            w = 0.10 * math.sin(t * math.pi) ** 0.8
                            d = v - cy
                            if abs(d) < w and v < 0.70:
                                col = (40, 120, 200) if d < 0 else (70, 150, 225)
                            if abs(d) < 0.012 and v < 0.70:
                                col = (25, 70, 130)
                    if col:
                        acc[0] += col[0]; acc[1] += col[1]; acc[2] += col[2]; acc[3] += 255
                    else:
                        # soft outline/shadow
                        if rounded_box(u, v, 0.15, 0.05, 0.85, 0.95, 0.07) > 0 and not (u > 0.84 - fold and v < 0.06 + fold and (u - (0.84 - fold)) > (v - 0.06) + 0.012):
                            acc[0] += 60; acc[1] += 70; acc[2] += 80; acc[3] += 200
            n = ss * ss
            a = acc[3] / n
            i = (y * size + x) * 4
            if a > 0:
                px[i:i + 4] = [int(acc[0] / (acc[3] / 255)), int(acc[1] / (acc[3] / 255)), int(acc[2] / (acc[3] / 255)), int(a)]
                px[i:i + 3] = [min(255, c) for c in px[i:i + 3]]
    return png(size, px)


def main(out):
    sizes = [16, 24, 32, 48, 64, 256]
    images = [render(s) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for s, data in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(out, "wb") as f:
        f.write(header + entries + b"".join(images))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "assets/feather.ico")
