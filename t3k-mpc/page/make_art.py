#!/usr/bin/env python3
"""The TONE3000 page's pictures, drawn here (our own drawings: no photos, no logos), copying the layout and colours
of the HeadRush Prime editor's TONE3000 browser from the user's screenshots (Nov 2026): the near-black frame and
sidebar, the charcoal panel, the search pill and Sort by, the tab rules, the dark cards with their footer icons, the
scroll track, the bottom tab bar; the tone pop-up, the filter drawer and the account box over the dimmed screen;
a gear picture per kind of tone; the icons.
    make_art.py <page dir> <font dir>   writes the PNGs into the page dir (build.sh skin; not kept in git)

Geometry is in plugin coordinates (1280 x 628; layout.conf adds 86 to y) and must match layout.conf."""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT, FONTS = sys.argv[1], sys.argv[2]
W, H = 1280, 628


def hexc(s, a=None):
    c = tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))
    return c + (a,) if a is not None else c


def font(sz, bold=False):
    return ImageFont.truetype(os.path.join(FONTS, "TitilliumWeb-%s.ttf" % ("Bold" if bold else "SemiBold")), sz)


OUTER, PANEL, PILL, SEP, CARD, TEAL = hexc("13131d"), hexc("22222e"), hexc("31303e"), hexc("2d2d39"), hexc("13131d"), hexc("00deab")
GREY, WHITE, MODAL, ROW = hexc("8a8796"), hexc("f4f4f6"), hexc("0f0e13"), hexc("171721")

# grid geometry (layout.conf's list key=row)
GX, GY, CW, CH, GAP = 76, 110, 225, 216, 10


def card_xy(k):
    return GX + (k % 5) * (CW + GAP), GY + (k // 5) * (CH + GAP)


# ---- icons (white strokes on clear unless given), drawn at 4x then reduced -------------------------------------
def icon(fn, size=96, col=WHITE):
    S = 4
    im = Image.new("RGBA", (size * S, size * S), (0, 0, 0, 0))
    fn(ImageDraw.Draw(im), size * S, col + (255,))
    return im.resize((size, size), Image.LANCZOS)


def i_search(d, I, c):
    d.ellipse((I * .2, I * .2, I * .62, I * .62), outline=c, width=int(I * .08))
    d.line((I * .56, I * .56, I * .8, I * .8), fill=c, width=int(I * .1))


def i_x(d, I, c):
    d.line((I * .28, I * .28, I * .72, I * .72), fill=c, width=int(I * .08))
    d.line((I * .72, I * .28, I * .28, I * .72), fill=c, width=int(I * .08))


def i_filter(d, I, c):   # three sliders
    for k, kx in enumerate((.62, .36, .55)):
        y = I * (.28 + k * .22)
        d.line((I * .18, y, I * .82, y), fill=c, width=int(I * .055))
        d.rectangle((I * kx - I * .05, y - I * .08, I * kx + I * .05, y + I * .08), fill=c)


def i_cloud(d, I, c):
    d.ellipse((I * .18, I * .42, I * .5, I * .74), fill=c)
    d.ellipse((I * .36, I * .26, I * .72, I * .62), fill=c)
    d.ellipse((I * .56, I * .44, I * .84, I * .74), fill=c)
    d.rectangle((I * .34, I * .58, I * .7, I * .74), fill=c)


def i_person(d, I, c):
    d.ellipse((I * .36, I * .16, I * .64, I * .44), fill=c)
    d.pieslice((I * .2, I * .5, I * .8, I * 1.1), 180, 360, fill=c)


def i_up(d, I, c):
    d.polygon([(I * .5, I * .3), (I * .76, I * .66), (I * .24, I * .66)], fill=c)


def i_down(d, I, c):
    d.polygon([(I * .5, I * .7), (I * .76, I * .34), (I * .24, I * .34)], fill=c)


def i_download(d, I, c):
    w = int(I * .08)
    d.line((I * .5, I * .18, I * .5, I * .62), fill=c, width=w)
    d.line((I * .3, I * .44, I * .5, I * .64, I * .7, I * .44), fill=c, width=w, joint="curve")
    d.line((I * .22, I * .66, I * .22, I * .8, I * .78, I * .8, I * .78, I * .66), fill=c, width=w, joint="curve")


def i_heart(d, I, c):
    d.ellipse((I * .2, I * .26, I * .52, I * .56), fill=c)
    d.ellipse((I * .48, I * .26, I * .8, I * .56), fill=c)
    d.polygon([(I * .22, I * .48), (I * .78, I * .48), (I * .5, I * .82)], fill=c)


def i_folder(d, I, c):
    w = int(I * .07)
    d.line((I * .18, I * .3, I * .42, I * .3, I * .48, I * .38, I * .82, I * .38, I * .82, I * .76, I * .18, I * .76, I * .18, I * .3),
           fill=c, width=w, joint="curve")


def labelled(fn, text, col):   # a sidebar icon with its word under it (one picture: the tile has no text)
    im = Image.new("RGBA", (96, 96), (0, 0, 0, 0))
    im.alpha_composite(icon(fn, 56, col), (20, 6))
    d = ImageDraw.Draw(im)
    d.text((48, 78), text, font=font(17), fill=col + (255,), anchor="mm")
    return im


for name, fn, col in (("ic_search", i_search, WHITE), ("ic_x", i_x, GREY), ("ic_xw", i_x, WHITE), ("ic_filter", i_filter, WHITE),
                      ("ic_up", i_up, GREY), ("ic_down", i_down, GREY), ("ic_dl_dark", i_download, hexc("15151c")),
                      ("ic_dl", i_download, WHITE)):
    icon(fn, 96, col).save(os.path.join(OUT, name + ".png"))
labelled(i_cloud, "Cloud", TEAL).save(os.path.join(OUT, "ic_cloud.png"))
labelled(i_person, "Account", GREY).save(os.path.join(OUT, "ic_account.png"))


# ---- the library screen --------------------------------------------------------------------------------------
def library():
    im = Image.new("RGB", (W, H), OUTER)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((60, 4, 1276, 562), 6, fill=PANEL)                     # the panel
    d.rounded_rectangle((76, 14, 300, 46), 16, fill=PILL)                      # search pill
    im.paste(icon(i_search, 18, GREY), (86, 21), icon(i_search, 18, GREY))
    d.text((1098, 30), "Sort by:", font=font(14), fill=GREY + (255,), anchor="rm")
    d.line((60, 58, 1276, 58), fill=SEP, width=2)
    d.line((60, 100, 1276, 100), fill=SEP, width=1)
    for k in range(10):                                                          # the cards
        x, y = card_xy(k)
        d.rounded_rectangle((x, y, x + CW, y + CH), 6, fill=CARD)
        for fn, ix in ((i_download, 8), (i_heart, 42), (i_folder, CW - 32)):    # footer icons
            ic = icon(fn, 22, GREY)
            im.paste(ic, (x + ix, y + 168), ic)
    d.rounded_rectangle((1254, 110, 1266, 552), 6, fill=PILL)                  # the scroll track
    d.rounded_rectangle((1254, 176, 1266, 300), 6, fill=hexc("a1a19f"))
    return im


lib = library()
lib.save(os.path.join(OUT, "t3k_bg.png"))


def dimmed():
    dim = Image.blend(lib, Image.new("RGB", (W, H), (0, 0, 0)), 0.72)
    return dim


def shadowed_box(im, box, r=8, fill=MODAL, outline=SEP):
    sh = Image.new("L", (W, H), 0)
    ImageDraw.Draw(sh).rounded_rectangle(box, r, fill=180)
    sh = sh.filter(ImageFilter.GaussianBlur(14))
    im = Image.composite(Image.new("RGB", (W, H), (0, 0, 0)), im, sh)
    ImageDraw.Draw(im).rounded_rectangle(box, r, fill=fill, outline=outline, width=1)
    return im


# ---- the tone pop-up ---------------------------------------------------------------------------------------------
m = shadowed_box(dimmed(), (150, 8, 1130, 620))
d = ImageDraw.Draw(m)
d.line((172, 60, 1108, 60), fill=SEP, width=1)
d.line((172, 160, 1108, 160), fill=SEP, width=1)
d.ellipse((172, 126, 198, 152), outline=GREY, width=2)                       # avatar ring
d.text((185, 139), "T", font=font(14), fill=GREY, anchor="mm")
for txt, y in (("Description", 372), ("Makes and Models", 476), ("Tags", 530)):
    d.text((172, y), txt, font=font(16), fill=GREY, anchor="lm")
d.text((584, 184), "Models", font=font(19), fill=GREY, anchor="lm")
d.rounded_rectangle((584, 206, 1108, 246), 6, fill=CARD)                     # NAM A2 (n) header row
for k in range(6):                                                           # model rows
    y = 254 + k * 50
    d.rounded_rectangle((594, y, 1098, y + 44), 5, fill=ROW)
d.line((172, 552 - 4, 172, 552 - 4), fill=SEP)
m.save(os.path.join(OUT, "t3k_modal.png"))

# ---- the filter drawer -------------------------------------------------------------------------------------------
f = dimmed()
d = ImageDraw.Draw(f)
d.rectangle((920, 0, W, H), fill=PANEL)
d.text((940, 26), "Filter", font=font(22, True), fill=WHITE, anchor="lm")
d.text((1018, 52), "Gear", font=font(16), fill=TEAL, anchor="mm")
d.text((1185, 52), "Architecture", font=font(16), fill=GREY, anchor="mm")
d.line((936, 66, 1100, 66), fill=TEAL, width=2)
d.line((920, 68, W, 68), fill=SEP, width=1)
d.text((940, 330), "NAM A2 only (the MPC's CPU)", font=font(14), fill=GREY, anchor="lm")
f.save(os.path.join(OUT, "t3k_drawer.png"))

# ---- the account box ----------------------------------------------------------------------------------------------
a = shadowed_box(dimmed(), (290, 70, 990, 560))
d = ImageDraw.Draw(a)
d.text((320, 112), "TONE3000 Account", font=font(24, True), fill=WHITE, anchor="lm")
d.line((312, 140, 968, 140), fill=SEP, width=1)
a.save(os.path.join(OUT, "t3k_account.png"))


# ---- gear pictures: 400 x 200, on the cards' dark ---------------------------------------------------------------
GW, GH, S = 400, 200, 2


def card_bg():
    im = Image.new("RGB", (GW * S, GH * S), hexc("1b1b26"))
    spot = Image.new("L", im.size, 0)
    ImageDraw.Draw(spot).ellipse((GW * S * .1, GH * S * .0, GW * S * .9, GH * S * 1.3), fill=110)
    spot = spot.filter(ImageFilter.GaussianBlur(50 * S))
    return Image.composite(Image.new("RGB", im.size, hexc("3a2e2a")), im, spot)   # (a warm studio light)


def knob(d, cx, cy, r, face="d9d4c8", ring="1a1a1a"):
    d.ellipse((cx - r - 2 * S, cy - r - 2 * S, cx + r + 2 * S, cy + r + 2 * S), fill=hexc(ring))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=hexc(face))
    a = math.radians(-120)
    d.line((cx, cy, cx + r * .8 * math.sin(a), cy - r * .8 * math.cos(a)), fill=hexc("222222"), width=2 * S)


def amp_head(d, x0, y0, w, h, panel="c8b27a"):
    d.rounded_rectangle((x0, y0, x0 + w, y0 + h), 10 * S, fill=hexc("161616"), outline=hexc("2a2a2a"), width=2 * S)
    d.rounded_rectangle((x0 + w * .06, y0 + h * .1, x0 + w * .94, y0 + h * .52), 4 * S, fill=hexc("2b2b2b"))
    for k in range(9):
        yy = y0 + h * .13 + k * h * .042
        d.line((x0 + w * .08, yy, x0 + w * .92, yy), fill=hexc("3a3a3a"), width=S)
    d.rectangle((x0 + w * .06, y0 + h * .6, x0 + w * .94, y0 + h * .88), fill=hexc(panel))
    for k in range(7):
        knob(d, x0 + w * (.16 + k * .105), y0 + h * .74, h * .065)
    d.ellipse((x0 + w * .88, y0 + h * .7, x0 + w * .91, y0 + h * .78), fill=hexc("ff3b2f"))


def g_amp():
    im = card_bg()
    amp_head(ImageDraw.Draw(im), GW * S * .17, GH * S * .2, GW * S * .66, GH * S * .62)
    return im


def g_rig():
    im = card_bg()
    d = ImageDraw.Draw(im)
    X, Y = GW * S, GH * S
    amp_head(d, X * .3, Y * .06, X * .4, Y * .3, panel="b9b9b9")
    x0, y0, w, h = X * .29, Y * .38, X * .42, Y * .58
    d.rounded_rectangle((x0, y0, x0 + w, y0 + h), 8 * S, fill=hexc("161616"), outline=hexc("2a2a2a"), width=2 * S)
    d.rectangle((x0 + 8 * S, y0 + 8 * S, x0 + w - 8 * S, y0 + h - 8 * S), fill=hexc("2a2620"))
    for i in range(2):
        for j in range(2):
            cx, cy, r = x0 + w * (.29 + .42 * i), y0 + h * (.28 + .44 * j), h * .17
            d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=hexc("3d372d"), width=2 * S)
    return im


def g_pedal():
    im = card_bg()
    d = ImageDraw.Draw(im)
    X, Y = GW * S, GH * S
    x0, y0, w, h = X * .38, Y * .1, X * .24, Y * .8
    d.rounded_rectangle((x0, y0, x0 + w, y0 + h), 14 * S, fill=hexc("2f9e5a"), outline=hexc("1d6a3b"), width=2 * S)
    for k in range(3):
        knob(d, x0 + w * (.22 + k * .28), y0 + h * .2, w * .09, face="111111", ring="0a0a0a")
    d.ellipse((x0 + w * .46, y0 + h * .36, x0 + w * .54, y0 + h * .42), fill=hexc("ff3b2f"))
    d.rounded_rectangle((x0 + w * .12, y0 + h * .5, x0 + w * .88, y0 + h * .66), 4 * S, fill=hexc("e8e3d3"))
    cx, cy, r = x0 + w * .5, y0 + h * .83, w * .11
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=hexc("c9c9c9"), outline=hexc("777777"), width=2 * S)
    return im


def g_ir():
    im = card_bg()
    d = ImageDraw.Draw(im)
    X, Y = GW * S, GH * S
    cx, cy = X * .33, Y * .5
    for r, c in ((Y * .42, "101010"), (Y * .38, "2a2a2a"), (Y * .3, "1c1c1c"), (Y * .12, "3a3a3a"), (Y * .07, "4a4a4a")):
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=hexc(c))
    pts = [(X * .58 + i / 199 * X * .36, Y * .5 - math.exp(-i / 199 * 6) * math.sin(i / 199 * 70) * (1 if i > 4 else 2.2) * Y * .32)
           for i in range(200)]
    d.line(pts, fill=TEAL, width=3 * S, joint="curve")
    return im


def g_outboard():
    im = card_bg()
    d = ImageDraw.Draw(im)
    X, Y = GW * S, GH * S
    x0, y0, w, h = X * .1, Y * .3, X * .8, Y * .4
    d.rounded_rectangle((x0, y0, x0 + w, y0 + h), 4 * S, fill=hexc("23272c"), outline=hexc("3b4148"), width=2 * S)
    for k in range(6):
        knob(d, x0 + w * (.14 + k * .1), y0 + h * .55, h * .16, face="9aa3ad")
    d.rectangle((x0 + w * .76, y0 + h * .3, x0 + w * .94, y0 + h * .72), fill=hexc("0d0f11"))
    d.line((x0 + w * .85, y0 + h * .7, x0 + w * .81, y0 + h * .38), fill=hexc("ffb347"), width=2 * S)
    return im


for name, fn in (("gear_amp", g_amp), ("gear_rig", g_rig), ("gear_pedal", g_pedal), ("gear_ir", g_ir), ("gear_outboard", g_outboard)):
    fn().resize((GW, GH), Image.LANCZOS).save(os.path.join(OUT, name + ".png"), optimize=True)
print("art: wrote the TONE3000 page pictures into", OUT)
