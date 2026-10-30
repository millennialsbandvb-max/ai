#!/usr/bin/env python3
"""Per-effect enclosure colours for the Duo FX / Quad Rig pages (the colours: tools/pedal_looks.h).
   pedal_looks.py patch <shadow_skin.py>          mode images drawn in layout order (art first = under the frames),
                                                  and no Q-Link frame (MPC's orange box) on the page
   pedal_looks.py layout <layout.conf> <out dir>  writes <out dir>/layout.conf with every "looks key=<param>" line
                                                  replaced by one full-page art line per look (when=<param>:<n>)
                                                  and the look SVGs (look_<n>.svg) beside it

A "looks" line goes first in its tab, so each look's enclosure lies under the tab's frames and boxes. The plugin
sets <param> (a read-only option parameter) to the look of the effect the slot is running; MPC shows that mode's
background (when=, IndexedEnabling). The base background stays the page's own colour. The look backgrounds are
smooth (no hammertone): a textured full page doesn't compress, and there are many of them."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
Y_OFF, W, H = 86, 1280, 628


def looks():
    src = open(os.path.join(HERE, "pedal_looks.h")).read()
    body = src[src.index("PEDAL_LOOKS[N_LOOKS] = {"):]
    return re.findall(r'\{"([^"]+)", "([0-9a-fA-F]{6})", "([0-9a-fA-F]{6})"\}', body)


def patch(path):
    s = open(path).read()
    old = '''            for w in base + ws:
                script += baked_cmds(w, TITLE_FONT, base_dir)'''
    new = '''            for w in [w_ for w_ in tab["widgets"] if w_ in base or w_ in ws]:   # (layout order: tools/pedal_looks.py)
                script += baked_cmds(w, TITLE_FONT, base_dir)'''
    if s.count(old) != 1:
        raise SystemExit("pedal_looks.py: no longer applies to %s" % path)
    s = s.replace(old, new)
    # no orange Q-Link frame on our pages (the user's ask): an empty list, as Akai's own SP1200 / MPC3000 / MPC60
    # pages have it
    old = '''"qlinkBoundsData": [qlink_bounds(tab, keys)]'''
    if s.count(old) != 1:
        raise SystemExit("pedal_looks.py: no longer applies to %s" % path)
    open(path, "w").write(s.replace(old, '''"qlinkBoundsData": []'''))
    studio = os.path.join(os.path.dirname(path), "studio.py")   # (its previews outline the frame: none now)
    s = open(studio).read()
    old = '''tab["qlinkBoundsData"][0].split()'''
    if s.count(old) != 1:
        raise SystemExit("pedal_looks.py: no longer applies to %s" % studio)
    open(studio, "w").write(s.replace(old, '''(tab["qlinkBoundsData"] or ["0 0 0 0"])[0].split()'''))


def layout(src, out):
    os.makedirs(out, exist_ok=True)
    ls = looks()
    for i, (name, hi, lo) in enumerate(ls):
        open(os.path.join(out, "look_%d.svg" % i), "w").write(
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d"><!-- %s -->'
            '<linearGradient id="look%d" x1="0" y1="0" x2="0.25" y2="1"><stop offset="0" stop-color="#%s"/>'
            '<stop offset="1" stop-color="#%s"/></linearGradient>'
            '<rect width="%d" height="%d" fill="url(#look%d)"/></svg>' % (W, H, name, i, hi, lo, W, H, i))
    lines = []
    for raw in open(src):
        m = re.match(r"\s*looks\s+key=(\S+)\s*(#.*)?$", raw)
        if not m:
            lines.append(raw)
            continue
        for i in range(len(ls)):
            lines.append("art file=look_%d.svg x=0 y=%d w=%d h=%d when=%s:%d\n" % (i, Y_OFF, W, H, m.group(1), i))
    open(os.path.join(out, "layout.conf"), "w").write("".join(lines))


if __name__ == "__main__":
    if sys.argv[1] == "patch":
        patch(sys.argv[2])
    elif sys.argv[1] == "layout":
        layout(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)
