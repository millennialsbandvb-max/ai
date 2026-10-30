#!/usr/bin/env python3
"""Shrinks a page's PNGs to 256-colour palette images (alpha kept) where that's smaller: the pedal look's shaded
knob filmstrips and textured backgrounds are several times bigger as full colour, and the firmware's root
filesystem has little room. Also writes TUI.json without indentation (a quarter of the size; MPC's JSON reader
doesn't care), and the per-effect colour backgrounds (sh_mode_*, smooth gradients) with 64 colours.
   png_shrink.py <dir>..."""
import json
import os
import sys

from PIL import Image

saved = 0
for d in sys.argv[1:]:
    for root, _, files in os.walk(d):
        for f in files:
            if f == "TUI.json":
                p = os.path.join(root, f)
                before = os.path.getsize(p)
                d = json.load(open(p))
                with open(p, "w") as o:
                    json.dump(d, o, separators=(",", ":"))
                saved += before - os.path.getsize(p)
                continue
            if not f.endswith(".png"):
                continue
            p = os.path.join(root, f)
            im = Image.open(p)
            if im.mode not in ("RGB", "RGBA"):
                continue
            q = im.quantize(64 if f.startswith("sh_mode_") and im.mode == "RGB" else 256, method=Image.Quantize.FASTOCTREE if im.mode == "RGBA" else Image.Quantize.MEDIANCUT,
                            dither=Image.Dither.FLOYDSTEINBERG)
            tmp = p + ".tmp"
            q.save(tmp, "PNG", optimize=True)
            before, after = os.path.getsize(p), os.path.getsize(tmp)
            if after < before * 0.9:
                os.replace(tmp, p)
                saved += before - after
            else:
                os.remove(tmp)
print("png_shrink: %.1f MB saved" % (saved / 1e6))
