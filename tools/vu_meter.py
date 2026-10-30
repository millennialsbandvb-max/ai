#!/usr/bin/env python3
"""A gain-reduction VU meter for our pages: the face (a picture for the layout's `art` line) and the needle (a
filmstrip, drawn by tools/page_lists.py for a `slider_v ... narrow=1 vu=1` over the face; it doesn't take touches).
   vu_meter.py face <out.png> <w> <h>
GR 0 dB rests at the right, 20 dB at the left; position = 1 - (gr/20)^0.6 (more room for the first few dB)."""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MARKS = [0, 1, 2, 3, 5, 7, 10, 15, 20]


def geometry(w, h):
    """(pivot x, pivot y, radius, angle in degrees for a GR in dB), shared by the face and the needle"""
    px, py, r = w / 2, h * 1.18, h * 0.98
    return px, py, r, lambda gr: -46 + 92 * (1 - (min(max(gr, 0), 20) / 20) ** 0.6)


def face(path, w, h):
    from PIL import Image, ImageDraw, ImageFont
    S = 3
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, w * S - 1, h * S - 1), radius=10 * S, fill=(28, 26, 22, 255))
    d.rounded_rectangle((6 * S, 6 * S, (w - 6) * S, (h - 6) * S), radius=7 * S, fill=(242, 230, 196, 255))
    px, py, r, ang = geometry(w * S, h * S)
    fp = os.environ.get("VU_FONT")
    font = ImageFont.truetype(fp, 15 * S) if fp and os.path.exists(fp) else ImageFont.load_default()
    small = ImageFont.truetype(fp, 11 * S) if fp and os.path.exists(fp) else font
    for gr in [x / 2 for x in range(0, 41)]:   # ticks every half dB, longer at the marks
        a = math.radians(ang(gr))
        big = gr in MARKS
        r0, r1 = r * (0.86 if big else 0.9), r * 0.96
        d.line((px + r0 * math.sin(a), py - r0 * math.cos(a), px + r1 * math.sin(a), py - r1 * math.cos(a)),
               fill=(30, 28, 24, 255), width=(2 if big else 1) * S)
        if big:
            tx, ty = px + r * 0.76 * math.sin(a), py - r * 0.76 * math.cos(a)
            d.text((tx, ty), str(int(gr)), font=font, fill=(30, 28, 24, 255), anchor="mm")
    a0, a1 = math.radians(ang(20)), math.radians(ang(0))   # the arc
    d.arc((px - r * 0.96, py - r * 0.96, px + r * 0.96, py + r * 0.96), math.degrees(a0) - 90, math.degrees(a1) - 90,
          fill=(30, 28, 24, 255), width=2 * S)
    d.text((w * S / 2, h * S * 0.74), "GAIN REDUCTION  dB", font=small, fill=(110, 40, 30, 255), anchor="mm")
    im.resize((w, h), Image.LANCZOS).save(path)


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "face":
        face(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
    else:
        raise SystemExit(__doc__)
