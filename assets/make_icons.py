#!/usr/bin/env python3
"""Generate the UD file icons -- pure stdlib, no Pillow.

Two black-and-white monogram icons, both a rounded square with a "UD" mark:

  ud-source.ico    (.ud)   black tile, white letters      -- the source
  ud-bytecode.ico  (.ldx)  white tile, black letters+edge -- the compiled binary

Rendering is analytic: every pixel samples a signed distance field, so edges are
anti-aliased from one evaluation each (no Pillow, no slow supersampling). Output
is multi-resolution PNG-in-ICO (16..256 px), which Windows 10/11 reads directly.
"""
import math, struct, zlib, os

# ---- geometry, all in a unit canvas [0,1]x[0,1], y downwards ----------------

MARGIN, CORNER = 0.05, 0.20          # rounded background square
HALFW          = 0.052               # letter stroke half-width
BORDERW        = 0.028               # edge outline on the light icon

def _arc(cx, cy, r, a0, a1, n):
    return [(cx + r*math.cos(a0 + (a1-a0)*i/(n-1)),
             cy + r*math.sin(a0 + (a1-a0)*i/(n-1))) for i in range(n)]

# letters in their own [0,1] box, later scaled/placed uniformly
def _letters():
    U = [[(0.22, 0.05), (0.22, 0.58)],
         [(0.78, 0.05), (0.78, 0.58)],
         _arc(0.50, 0.58, 0.28, 0.0, math.pi, 14)]
    bowl = ([(0.20, 0.10), (0.52, 0.10)]
            + _arc(0.52, 0.34, 0.24, -math.pi/2, 0.0, 8)
            + [(0.76, 0.34), (0.76, 0.66)]
            + _arc(0.52, 0.66, 0.24, 0.0, math.pi/2, 8)
            + [(0.52, 0.90), (0.20, 0.90)])
    D = [[(0.20, 0.05), (0.20, 0.95)], bowl]
    S, GAP = 0.34, 0.03
    x0u = (1 - (2*S + GAP)) / 2
    x0d = x0u + S + GAP
    y0  = (1 - S) / 2
    place = lambda strokes, x0: [[(x0 + lx*S, y0 + ly*S) for (lx, ly) in s]
                                 for s in strokes]
    return place(U, x0u) + place(D, x0d)

STROKES = _letters()

def _d_seg(px, py, ax, ay, bx, by):
    vx, vy = bx-ax, by-ay
    wx, wy = px-ax, py-ay
    vv = vx*vx + vy*vy
    t = 0.0 if vv == 0 else max(0.0, min(1.0, (wx*vx + wy*vy)/vv))
    dx, dy = px-(ax+t*vx), py-(ay+t*vy)
    return math.hypot(dx, dy)

def _d_letters(px, py):
    best = 1e9
    for stroke in STROKES:
        for i in range(len(stroke)-1):
            ax, ay = stroke[i]; bx, by = stroke[i+1]
            d = _d_seg(px, py, ax, ay, bx, by)
            if d < best: best = d
    return best

def _sdf_roundbox(px, py):
    hx = hy = (1 - 2*MARGIN)/2
    dx = abs(px-0.5) - (hx - CORNER)
    dy = abs(py-0.5) - (hy - CORNER)
    outside = math.hypot(max(dx, 0.0), max(dy, 0.0))
    inside  = min(max(dx, dy), 0.0)
    return outside + inside - CORNER          # <0 inside, >0 outside

def _cov(inside_amount, aa):
    return max(0.0, min(1.0, 0.5 + inside_amount/aa))

def render(size, dark_tile):
    aa = 1.0 / size                            # ~1px edge softening
    px_bytes = bytearray()
    for y in range(size):
        px_bytes.append(0)                     # PNG filter: none
        for x in range(size):
            p = (x + 0.5)/size
            q = (y + 0.5)/size
            tile   = _cov(-_sdf_roundbox(p, q), aa)          # inside the square
            letter = _cov(HALFW - _d_letters(p, q), aa)      # on a letter stroke
            if dark_tile:                       # black tile, white letters
                val = int(round(255 * letter))
                alpha = int(round(255 * tile))
            else:                               # white tile, black letters+edge
                edge = _cov(BORDERW - abs(_sdf_roundbox(p, q)), aa)
                ink  = max(letter, edge)
                val = int(round(255 * (1 - ink)))
                alpha = int(round(255 * max(tile, edge)))
            px_bytes += bytes((val, val, val, alpha))
    return bytes(px_bytes)

# ---- minimal PNG + ICO writers ---------------------------------------------

def _chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

def png(size, raw_rgba):
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n"
            + _chunk(b"IHDR", ihdr)
            + _chunk(b"IDAT", zlib.compress(raw_rgba, 9))
            + _chunk(b"IEND", b""))

def ico(path, dark_tile, sizes=(16, 32, 48, 64, 128, 256)):
    imgs = [png(s, render(s, dark_tile)) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(imgs))
    offset = 6 + 16*len(imgs)
    entries, blob = b"", b""
    for s, img in zip(sizes, imgs):
        entries += struct.pack("<BBBBHHII", s & 0xFF, s & 0xFF, 0, 0,
                               1, 32, len(img), offset)
        offset += len(img)
        blob += img
    with open(path, "wb") as f:
        f.write(header + entries + blob)
    print("wrote %s (%d bytes)" % (path, os.path.getsize(path)))

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    for name, dark in (("ud-source", True), ("ud-bytecode", False)):
        ico(os.path.join(here, name + ".ico"), dark_tile=dark)
        # a 256px PNG too: Linux icon themes, the VS Code extension, READMEs
        with open(os.path.join(here, name + ".png"), "wb") as f:
            f.write(png(256, render(256, dark)))
        print("wrote %s.png" % name)
