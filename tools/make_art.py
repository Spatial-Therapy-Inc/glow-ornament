#!/usr/bin/env python3
"""Turns the drawn parts in art/*.svg into art/<name>.rgb565, the bytes the firmware embeds: RGB565, little endian,
on black (the firmware draws them additively), with ordered dithering so soft light shows no bands on the 16-bit
panel. Only the parts that need drawing by hand are here; glows, blobs, spheres and the moon are made in the firmware.

  python3 tools/make_art.py            # all of them
  python3 tools/make_art.py bell       # one

Needs Google Chrome (to render the SVG) and ImageMagick (`magick`). The sizes are each SVG's own width and height.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

ART = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "art")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
PARTS = ["mushroom", "flame", "bell"]
BAYER = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))


def dither(value, bits, threshold):
    drop = 8 - bits
    return min((value + (threshold << drop >> 4)) >> drop, (1 << bits) - 1)


def size_of(svg):
    head = open(svg, encoding="utf-8").read(400)
    w = re.search(r'width="(\d+)"', head)
    h = re.search(r'height="(\d+)"', head)
    return int(w.group(1)), int(h.group(1))


def make(name):
    svg = os.path.join(ART, name + ".svg")
    w, h = size_of(svg)
    with tempfile.TemporaryDirectory() as tmp:
        png = os.path.join(tmp, name + ".png")
        subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--default-background-color=000000ff",
                        f"--window-size={w},{h}", f"--screenshot={png}", "file://" + os.path.abspath(svg)],
                       check=True, capture_output=True)
        rgb = subprocess.run(["magick", png, "-crop", f"{w}x{h}+0+0", "+repage", "-depth", "8", "RGB:-"],
                             check=True, capture_output=True).stdout
    if len(rgb) != w * h * 3:
        sys.exit(f"{name}: expected {w * h * 3} bytes of RGB, got {len(rgb)}")
    out = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = rgb[(y * w + x) * 3:(y * w + x) * 3 + 3]
            t = BAYER[y & 3][x & 3]
            out += struct.pack("<H", dither(r, 5, t) << 11 | dither(g, 6, t) << 5 | dither(b, 5, t))
    with open(os.path.join(ART, name + ".rgb565"), "wb") as f:
        f.write(out)
    print(f"{name}: {w} x {h}, {len(out)} bytes")


if __name__ == "__main__":
    for part in sys.argv[1:] or PARTS:
        make(part)
