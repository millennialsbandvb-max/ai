#!/usr/bin/env python3
"""The TONE3000 page's pictures, drawn here (our own drawings, no photos or logos), in the HeadRush-style dark
look the user asked for: the screen background (a top bar, the dark blue gradient, a bottom bar), a gear picture
per kind of tone (amp head, full rig, pedal, IR speaker, outboard rack, empty slot) and the tap icons (search,
account, back, scroll up / down).
    make_art.py <page dir>     writes the PNGs there (build.sh skin runs it; they're not kept in git)"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

OUT = sys.argv[1]
S = 2   # drawn at twice the size, then scaled down (smooth edges)


def save(im, name, size):
    im.resize(size, Image.LANCZOS).save(os.path.join(OUT, name), optimize=True)


def grad(w, h, top, bot):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return im


def hexc(s):
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


# ---- the screen: 1280 x 628 (the plugin area); top bar 0..54, bottom bar 572..628 ------------------------------
W, H = 1280, 628
bg = grad(W, H, hexc("0b1a2b"), hexc("03070c"))
glow = Image.new("L", (W, H), 0)
ImageDraw.Draw(glow).ellipse((-200, -260, 900, 360), fill=70)   # (a soft blue light from the top left)
glow = glow.filter(ImageFilter.GaussianBlur(120))
bg = Image.composite(Image.new("RGB", (W, H), hexc("1d4a73")), bg, glow)
d = ImageDraw.Draw(bg)
d.rectangle((0, 0, W, 54), fill=hexc("05090e"))
d.line((0, 54, W, 54), fill=hexc("1b2c3d"), width=2)
d.rectangle((0, 572, W, H), fill=hexc("05090e"))
d.line((0, 572, W, 572), fill=hexc("1b2c3d"), width=2)
bg.save(os.path.join(OUT, "hr_bg.png"), optimize=True)


# ---- gear pictures: 400 x 176 (cards) / reused larger on the tone view -------------------------------------
GW, GH = 400 * S, 176 * S


def card_bg():
    im = grad(GW, GH, hexc("1a2a3c"), hexc("0a121b"))
    spot = Image.new("L", (GW, GH), 0)
    ImageDraw.Draw(spot).ellipse((GW * 0.15, GH * 0.05, GW * 0.85, GH * 1.25), fill=90)
    spot = spot.filter(ImageFilter.GaussianBlur(40 * S))
    return Image.composite(Image.new("RGB", (GW, GH), hexc("3a5878")), im, spot)


def knob(d, cx, cy, r, face="d9d4c8", ring="1a1a1a"):
    d.ellipse((cx - r - 2 * S, cy - r - 2 * S, cx + r + 2 * S, cy + r + 2 * S), fill=hexc(ring))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=hexc(face))
    a = math.radians(-120)
    d.line((cx, cy, cx + r * 0.8 * math.sin(a), cy - r * 0.8 * math.cos(a)), fill=hexc("222222"), width=2 * S)


def shadow(im, box, r=10):
    sh = Image.new("L", im.size, 0)
    ImageDraw.Draw(sh).rounded_rectangle(box, r * S, fill=150)
    sh = sh.filter(ImageFilter.GaussianBlur(9 * S))
    return Image.composite(Image.new("RGB", im.size, (0, 0, 0)), im, sh)


def amp_head(d, x0, y0, w, h, panel="c8b27a"):
    d.rounded_rectangle((x0, y0, x0 + w, y0 + h), 10 * S, fill=hexc("161616"), outline=hexc("2a2a2a"), width=2 * S)
    d.rounded_rectangle((x0 + w * 0.06, y0 + h * 0.1, x0 + w * 0.94, y0 + h * 0.52), 4 * S, fill=hexc("2b2b2b"))   # grille
    for k in range(9):
        yy = y0 + h * 0.13 + k * h * 0.042
        d.line((x0 + w * 0.08, yy, x0 + w * 0.92, yy), fill=hexc("3a3a3a"), width=S)
    d.rectangle((x0 + w * 0.06, y0 + h * 0.6, x0 + w * 0.94, y0 + h * 0.88), fill=hexc(panel))   # control panel
    for k in range(7):
        knob(d, x0 + w * (0.16 + k * 0.105), y0 + h * 0.74, h * 0.065)
    d.ellipse((x0 + w * 0.88, y0 + h * 0.7, x0 + w * 0.91, y0 + h * 0.78), fill=hexc("ff3b2f"))   # the pilot light


def gear_amp():
    im = card_bg()
    im = shadow(im, (GW * 0.17, GH * 0.2, GW * 0.83, GH * 0.86))
    amp_head(ImageDraw.Draw(im), GW * 0.17, GH * 0.18, GW * 0.66, GH * 0.64)
    return im


def gear_rig():
    im = card_bg()
    im = shadow(im, (GW * 0.3, GH * 0.08, GW * 0.7, GH * 0.98))
    d = ImageDraw.Draw(im)
    amp_head(d, GW * 0.3, GH * 0.06, GW * 0.4, GH * 0.3, panel="b9b9b9")
    x0, y0, w, h = GW * 0.29, GH * 0.38, GW * 0.42, GH * 0.58   # 4x12 cabinet
    d.rounded_rectangle((x0, y0, x0 + w, y0 + h), 8 * S, fill=hexc("161616"), outline=hexc("2a2a2a"), width=2 * S)
    d.rectangle((x0 + 8 * S, y0 + 8 * S, x0 + w - 8 * S, y0 + h - 8 * S), fill=hexc("2a2620"))
    for i in range(2):
        for j in range(2):
            cx, cy, r = x0 + w * (0.29 + 0.42 * i), y0 + h * (0.28 + 0.44 * j), h * 0.17
            d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=hexc("3d372d"), width=2 * S)
    return im


def gear_pedal():
    im = card_bg()
    x0, y0, w, h = GW * 0.36, GH * 0.1, GW * 0.28, GH * 0.8
    im = shadow(im, (x0, y0 + 6 * S, x0 + w, y0 + h + 6 * S), 14)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((x0, y0, x0 + w, y0 + h), 14 * S, fill=hexc("2f9e5a"), outline=hexc("1d6a3b"), width=2 * S)
    for k in range(3):
        knob(d, x0 + w * (0.22 + k * 0.28), y0 + h * 0.2, w * 0.09, face="111111", ring="0a0a0a")
    d.ellipse((x0 + w * 0.46, y0 + h * 0.36, x0 + w * 0.54, y0 + h * 0.42), fill=hexc("ff3b2f"))   # LED
    d.rounded_rectangle((x0 + w * 0.12, y0 + h * 0.5, x0 + w * 0.88, y0 + h * 0.66), 4 * S, fill=hexc("e8e3d3"))   # label
    cx, cy, r = x0 + w * 0.5, y0 + h * 0.83, w * 0.11   # footswitch
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=hexc("c9c9c9"), outline=hexc("777777"), width=2 * S)
    return im


def gear_ir():
    im = card_bg()
    d = ImageDraw.Draw(im)
    cx, cy = GW * 0.33, GH * 0.5
    for r, c in ((GH * 0.42, "101010"), (GH * 0.38, "2a2a2a"), (GH * 0.3, "1c1c1c"), (GH * 0.12, "3a3a3a"), (GH * 0.07, "4a4a4a")):
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=hexc(c))
    pts = []   # an impulse response: a spike and its ringing decay
    for i in range(200):
        t = i / 199
        y = math.exp(-t * 6) * math.sin(t * 70) * (1 if i > 4 else 2.2)
        pts.append((GW * 0.58 + t * GW * 0.36, GH * 0.5 - y * GH * 0.32))
    d.line(pts, fill=hexc("49d7f0"), width=3 * S, joint="curve")
    return im


def gear_outboard():
    im = card_bg()
    im = shadow(im, (GW * 0.1, GH * 0.3, GW * 0.9, GH * 0.72), 6)
    d = ImageDraw.Draw(im)
    x0, y0, w, h = GW * 0.1, GH * 0.28, GW * 0.8, GH * 0.42
    d.rounded_rectangle((x0, y0, x0 + w, y0 + h), 4 * S, fill=hexc("23272c"), outline=hexc("3b4148"), width=2 * S)
    for k in (0, 1):
        d.ellipse((x0 + w * (0.02 + 0.94 * k), y0 + h * 0.15, x0 + w * (0.04 + 0.94 * k), y0 + h * 0.3), fill=hexc("888888"))
    for k in range(6):
        knob(d, x0 + w * (0.14 + k * 0.1), y0 + h * 0.55, h * 0.16, face="9aa3ad")
    d.rectangle((x0 + w * 0.76, y0 + h * 0.3, x0 + w * 0.94, y0 + h * 0.72), fill=hexc("0d0f11"))   # a VU window
    d.line((x0 + w * 0.85, y0 + h * 0.7, x0 + w * 0.81, y0 + h * 0.38), fill=hexc("ffb347"), width=2 * S)
    return im


def gear_empty():
    return grad(GW, GH, hexc("0c141d"), hexc("070b10"))


for name, f in (("gear_amp", gear_amp), ("gear_rig", gear_rig), ("gear_pedal", gear_pedal), ("gear_ir", gear_ir),
                ("gear_outboard", gear_outboard), ("gear_empty", gear_empty)):
    save(f(), name + ".png", (400, 176))


# ---- icons: white strokes on clear, 96 x 96 -----------------------------------------------------------------
I = 96 * S


def icon(draw_fn, name, col="f2f4f7"):
    im = Image.new("RGBA", (I, I), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(im), hexc(col) + (255,))
    save(im, name, (96, 96))


def i_search(d, c):
    d.ellipse((I * 0.2, I * 0.2, I * 0.62, I * 0.62), outline=c, width=7 * S)
    d.line((I * 0.57, I * 0.57, I * 0.8, I * 0.8), fill=c, width=9 * S)


def i_account(d, c):
    d.ellipse((I * 0.36, I * 0.16, I * 0.64, I * 0.44), fill=c)
    d.pieslice((I * 0.2, I * 0.5, I * 0.8, I * 1.1), 180, 360, fill=c)


def i_back(d, c):
    d.line((I * 0.62, I * 0.2, I * 0.34, I * 0.5, I * 0.62, I * 0.8), fill=c, width=9 * S, joint="curve")


def i_up(d, c):
    d.polygon([(I * 0.5, I * 0.28), (I * 0.78, I * 0.68), (I * 0.22, I * 0.68)], fill=c)


def i_down(d, c):
    d.polygon([(I * 0.5, I * 0.72), (I * 0.78, I * 0.32), (I * 0.22, I * 0.32)], fill=c)


icon(i_search, "icon_search.png")
icon(i_account, "icon_account.png")
icon(i_back, "icon_back.png")
icon(i_up, "icon_up.png")
icon(i_down, "icon_down.png")
print("art: wrote the TONE3000 page pictures into", OUT)
