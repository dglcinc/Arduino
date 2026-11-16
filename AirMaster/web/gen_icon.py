#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright David Lewis
"""Draws the home-screen icon: a white DL in a white circle on a blue disc, on white. Writes icon.svg and the PNGs
that build_page.py compiles into the firmware. Needs Pillow. Run it only when the icon changes.

The letters are the DL monogram of pivac's hardware/dl-monogram.svg, in its units. The disc reaches 80 % of the
width, which is the safe zone of an Android maskable icon, so one drawing serves every launcher."""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
BLUE, WHITE = "#135ea8", "#ffffff"
HALF = 6.0          # half the width of the square, in monogram units
DISC = 4.8          # blue disc
RING, RING_W = 4.35, 0.2   # white circle inside the disc's edge

D = ("M -2.35 1.01 L -2.35 -2.51 A 0.24 0.24 0 0 1 -2.11 -2.75 L -1.55 -2.75 A 2 2 0 0 1 -1.55 1.25 L -2.11 1.25 "
     "A 0.24 0.24 0 0 1 -2.35 1.01 Z M -1.55 -1.95 L -1.55 0.45 A 1.2 1.2 0 0 0 -1.55 -1.95 Z")
L = ("M 0.8 -1.23 A 0.24 0.24 0 0 1 1.04 -1.47 L 1.36 -1.47 A 0.24 0.24 0 0 1 1.6 -1.23 L 1.6 1.59 "
     "A 0.14 0.14 0 0 0 1.74 1.73 L 2.793 1.73 A 0.24 0.24 0 0 1 3.033 1.97 L 3.033 2.29 A 0.24 0.24 0 0 1 2.793 2.53 "
     "L 1.04 2.53 A 0.24 0.24 0 0 1 0.8 2.29 Z")

SVG = f"""<?xml version="1.0" encoding="UTF-8"?>
<!-- SPDX-License-Identifier: CC-BY-NC-4.0 -->
<!-- Copyright David Lewis -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="{-HALF} {-HALF} {2 * HALF} {2 * HALF}" width="100%" height="100%">
  <rect x="{-HALF}" y="{-HALF}" width="{2 * HALF}" height="{2 * HALF}" fill="{WHITE}"/>
  <circle r="{DISC}" fill="{BLUE}"/>
  <circle r="{RING}" fill="none" stroke="{WHITE}" stroke-width="{RING_W}"/>
  <path d="{D}" fill="{WHITE}" fill-rule="evenodd"/>
  <path d="{L}" fill="{WHITE}"/>
</svg>
"""


def draw(px, ss=8):
    n = px * ss
    k = n / (2 * HALF)
    im = Image.new("RGB", (n, n), WHITE)
    d = ImageDraw.Draw(im)
    P = lambda x, y: (round((x + HALF) * k), round((y + HALF) * k))
    box = lambda x0, y0, x1, y1: [P(x0, y0), P(x1, y1)]
    circ = lambda cx, cy, r: box(cx - r, cy - r, cx + r, cy + r)
    rr = lambda x0, y0, x1, y1, r, fill, corners=(True,) * 4: d.rounded_rectangle(box(x0, y0, x1, y1), round(r * k), fill, corners=corners)
    d.ellipse(circ(0, 0, DISC), BLUE)
    d.ellipse(circ(0, 0, RING + RING_W / 2), WHITE)
    d.ellipse(circ(0, 0, RING - RING_W / 2), BLUE)
    # D: a stem rounded on the left, a half disc of radius 2 on its right, a counter of radius 1.2
    rr(-2.35, -2.75, -1.55, 1.25, 0.24, WHITE, (True, False, False, True))
    d.pieslice(circ(-1.55, -0.75, 2.0), -90, 90, WHITE)
    d.pieslice(circ(-1.55, -0.75, 1.2), -90, 90, BLUE)
    # L: two rounded bars and the fillet in their corner
    rr(0.8, -1.47, 1.6, 2.53, 0.24, WHITE)
    rr(0.8, 1.73, 3.033, 2.53, 0.24, WHITE)
    d.rectangle(box(1.6, 1.59, 1.74, 1.73), WHITE)
    d.ellipse(circ(1.74, 1.59, 0.14), BLUE)
    im = im.resize((px, px), Image.LANCZOS)
    return im.quantize(32, method=Image.MEDIANCUT, dither=Image.NONE)


if __name__ == "__main__":
    open(os.path.join(HERE, "icon.svg"), "w").write(SVG)
    for px in (180, 192, 512):
        f = os.path.join(HERE, "icon-%d.png" % px)
        draw(px).save(f, optimize=True)
        print("%s %d bytes" % (os.path.basename(f), os.path.getsize(f)))
