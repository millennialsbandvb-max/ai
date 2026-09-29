#!/usr/bin/env python3
"""Make every text on the NAM MPC page larger: patch a copy of mpc-vst-plugins' tools/shadow_skin.py.

    text_scale.py <shadow_skin.py to patch in place> <scale>

The page generator hard-codes its text sizes. This scales each one, and the box each text sits in, so nothing is
clipped: knob names and values, toggle names, the Model/IR readouts, and frame titles (drawn with a real font via
SHADOW_TITLE_FONT). Every edit must match the expected number of times, so a changed upstream fails here instead of silently
leaving small text.
"""
import sys

path, S = sys.argv[1], float(sys.argv[2])


def px(v):
    return str(int(round(v * S)))


def fl(v):
    return "%.1f" % (v * S)


TOGGLE_H = 34 + int(px(20)) + 4   # pill, then its (taller) name label

EDITS = [
    # knob: name label box, value text and box
    ("name_y, name_h = s // 2 + r + 2, 20", "name_y, name_h = s // 2 + r + 2, " + px(20)),
    ("ch = value_y + 26 + 6", "ch = value_y + %s + 6" % px(26), 2),           # knobs and sliders
    ("_name_label(0, name_y, cw, name_h, 17.0, INK)", "_name_label(0, name_y, cw, name_h, %s, INK)" % fl(17), 2),
    ("name_y, name_h = (sq - sh_) // 2 + sh_ + 2, 20", "name_y, name_h = (sq - sh_) // 2 + sh_ + 2, " + px(20)),
    ("_value_label(0, value_y, cw, 26, 22.0, INK_DIM)", "_value_label(0, value_y, cw, %s, %s, INK_DIM)" % (px(26), fl(22))),
    ('"style": "SemiBold", "height": 22.0},', '"style": "SemiBold", "height": %s},' % fl(22)),
    ("_bounds(0, value_y, cw, 26), \"Value\")", "_bounds(0, value_y, cw, %s), \"Value\")" % px(26)),
    # the plain toggle (pill): name label and component height
    ("[_focus(120, 58), _button(\"sh_pill_on.png\", \"sh_pill_off.png\", 1, 1, 53, 29, 33, 4),\n"
     "                                        _name_label(0, 34, 120, 20, 15.0, INK)])",
     "[_focus(150, %s), _button(\"sh_pill_on.png\", \"sh_pill_off.png\", 1, 1, 53, 29, 48, 4),\n"
     "                                        _name_label(0, 34, 150, %s, %s, INK)])" % (TOGGLE_H, px(20), fl(15))),
    ("kids.append(_placed(key, name, i, w[\"cx\"] - 60, w[\"cy\"] - 18, 120, 58))",
     "kids.append(_placed(key, name, i, w[\"cx\"] - 75, w[\"cy\"] - 18, 150, %d))" % TOGGLE_H),
    # readout and stepper text (the model and IR names)
    ("_value_label(8, 0, rw - 16, rh, 26.0, DISPLAY_INK if dot else ACCENT)",
     "_value_label(8, 0, rw - 16, rh, %s, DISPLAY_INK if dot else ACCENT)" % fl(26)),
    ("_value_label(8, 0, w[\"w\"] - 2 * h - 22, h, 26.0, DISPLAY_INK if dot else ACCENT,",
     "_value_label(8, 0, w[\"w\"] - 2 * h - 22, h, %s, DISPLAY_INK if dot else ACCENT," % fl(26)),
    # frame titles (real font): larger, and raised so they stay above the frame's header line (at y + 36)
    ("font=ImageFont.truetype(TITLE_FONT, 26)", "font=ImageFont.truetype(TITLE_FONT, %s)" % px(22)),
    ('dr.text((w["x"] + 18 - ox, w["y"] + 8 - oy)', 'dr.text((w["x"] + 18 - ox, w["y"] + 1 - oy)'),
]

src = open(path).read()
for edit in EDITS:
    old, new, want = edit if len(edit) == 3 else edit + (1,)
    n = src.count(old)
    if n != want:
        sys.exit("text_scale: expected %d match(es), found %d: %r" % (want, n, old[:70]))
    src = src.replace(old, new)
open(path, "w").write(src)
print("text scaled x%g in %s" % (S, path))
