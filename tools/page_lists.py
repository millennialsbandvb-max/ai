#!/usr/bin/env python3
"""Full-page lists for steppers, patched into a copy of mpc-vst-plugins' shadow_skin.py.
   page_lists.py <shadow_skin.py>

Tapping a stepper's name used to open MPC's round knob menu. A stepper whose plugin has a "<key>__list" parameter
(tools/picklist.h) instead opens a list over the whole plugin area while that parameter is 1: a title, Close,
< Page / Page > and 24 rows (3 columns of 8), each row's text the plugin's own (pick_1..pick_24). A tap on a row is
a Toggle Switch on it; the plugin takes the entry and closes the list. Everything uses the field pattern the
popups use (Toggle Switch actions, a live value label), with the boxes drawn into one panel image. Knobs with a
"<key>__list" parameter open the same list (see below)."""
import sys

path = sys.argv[1]
s = open(path).read()


def patch(old, new):
    global s
    if s.count(old) != 1:
        raise SystemExit("page_lists.py: no longer applies to %s: %r" % (path, old[:60]))
    s = s.replace(old, new)


# the parameters a list stepper needs
patch('''                if w.get("get"):
                    need.append(w["get"])
''', '''                if w.get("get"):
                    need.append(w["get"])
                if k + "__list" in index:
                    need += PICK_KEYS
''')

# the field: a tap toggles the list open (no knob menu); the panel goes on top like a popup's
patch('''                key = "shStepText_%s%dx%d" % ("dot_" if dot else "", w["w"] - 2 * h - 6, h)
                defs.setdefault(key, _local(key, [_action("Mouse Down", "Q-Link"),
                                                  _action("Double Click", "Show Overlay", "knob overlay")],
                                            [_focus(w["w"] - 2 * h - 6, h),
                                             _value_label(8, 0, w["w"] - 2 * h - 22, h, 26.0, DISPLAY_INK if dot else ACCENT,
                                                          handle="Text")]))
                kids.append(_placed(key, name, i, x0 + h + 3, y0, w["w"] - 2 * h - 6, h, extra={"Text": gi}))
''', '''                li = index.get(w["key"] + "__list")
                if li is not None:
                    fw = w["w"] - 2 * h - 6
                    key = "shStepList_%s%dx%d" % ("dot_" if dot else "", fw, h)
                    defs.setdefault(key, _local(key, [_action("Mouse Down", "Toggle Switch"), _action("Enter Pressed", "Toggle Switch")],
                                                [_focus(fw, h), _value_label(8, 0, fw - 16, h, 26.0, DISPLAY_INK if dot else ACCENT,
                                                                             handle="Text")]))
                    kids.append(_placed(key, name, li, x0 + h + 3, y0, fw, h, extra={"Text": gi}))
                    on_top += pick_panel(t, w, li)
                else:
                    key = "shStepText_%s%dx%d" % ("dot_" if dot else "", w["w"] - 2 * h - 6, h)
                    defs.setdefault(key, _local(key, [_action("Mouse Down", "Q-Link"),
                                                      _action("Double Click", "Show Overlay", "knob overlay")],
                                                [_focus(w["w"] - 2 * h - 6, h),
                                                 _value_label(8, 0, w["w"] - 2 * h - 22, h, 26.0, DISPLAY_INK if dot else ACCENT,
                                                              handle="Text")]))
                    kids.append(_placed(key, name, i, x0 + h + 3, y0, w["w"] - 2 * h - 6, h, extra={"Text": gi}))
''')

# the panel, built inside build() (it needs script, defs, art and index)
patch('''    radii, sliders, looks = set(), set(), {}''', '''    defs_made = set()

    def pick_panel(t, w, li):
        """The full-page list of a stepper with a "<key>__list" parameter (tools/page_lists.py)."""
        shown = "IndexedEnabling/1/2/Parameter %d" % li
        bar_y, bar_h, m = Y_OFF + 12, 60, 16
        cols, rows, gap = 3, 8, 6
        top = bar_y + bar_h + 12
        tw = (W - 2 * m - (cols - 1) * 8) // cols
        th = (Y_OFF + H - m - top - (rows - 1) * gap) // rows
        boxes = [("pick_close", m, bar_y, 180, bar_h), ("pick_prev", W - m - 2 * 150 - 12, bar_y, 150, bar_h),
                 ("pick_next", W - m - 150, bar_y, 150, bar_h)]
        cells = [("pick_%d" % (r + 1), m + (r // rows) * (tw + 8), top + (r % rows) * (th + gap), tw, th) for r in range(cols * rows)]
        panel = "sh_pick_panel"   # (one picture for every list: they're all the same)
        if panel not in defs_made:
            defs_made.add(panel)
            script.extend(["clear|" + LCD, "tile|0|%d|%d|%d|%s|%s|2" % (Y_OFF, W, H, LCD, ACCENT)]
                          + ["tile|%d|%d|%d|%d|%s|%s|2" % (x, y, bw, bh, SEG_OFF, ACCENT) for _, x, y, bw, bh in boxes]
                          + ["tile|%d|%d|%d|%d|%s|%s|1" % (x, y, bw, bh, LCD, LINE) for _, x, y, bw, bh in cells]
                          + ["crop|%s|0|%d|%d|%d" % (art(panel), Y_OFF, W, H)])
        pkey = "shPickPanel"
        defs[pkey] = _local(pkey, [], [_sub("Image", {"version": 2, "imageType": "Regular", "colour": "0",
                                                      "image": panel + ".png"}, _bounds(0, 0, W, H), "Image")])
        parts = [_placed(pkey, "%s list" % w["key"], li, 0, Y_OFF, W, H, focus="No")]
        tx = m + 180 + 16
        tkey = "shPickTitle_%d" % (boxes[1][1] - 16 - tx)
        defs.setdefault(tkey, _local(tkey, [], [_value_label(0, 0, boxes[1][1] - 16 - tx, bar_h, 26.0, ACCENT)]))
        parts.append(_placed(tkey, "list title", index["pick_title"], tx, bar_y, boxes[1][1] - 16 - tx, bar_h, focus="No"))
        for k, x, y, bw, bh in boxes + cells:
            left = k.startswith("pick_") and k[5:].isdigit()
            key = "shPickTap_%dx%d" % (bw, bh)
            defs.setdefault(key, _local(key, [_action("Mouse Down", "Toggle Switch"), _action("Enter Pressed", "Toggle Switch")],
                                        [_value_label(12, 0, bw - 24, bh, 24.0 if left else 26.0, ACCENT if left else INK,
                                                      "left verticallyCentred" if left else "horizontallyCentred verticallyCentred")]))
            parts.append(_placed(key, k, index[k], x, y, bw, bh, focus="No"))
        for c in parts:
            c["bounds"]["showWhenDataModelInvalid"] = "Show"
            c["bounds"]["additionalInvalidatingHandles"] = [shown]
        return parts

    radii, sliders, looks = set(), set(), {}''')

# (the knob's own value text shows through: the field's label is clear, there only to take the touch)
patch('''def _focus(w, h):''', '''def clear_label(c):
    c["componentData"]["data"]["textStyle"]["colour"] = "00000000"
    return c


def _focus(w, h):''')

# knobs with a list (a choice knob, e.g. Duo FX's Pedal): a double tap opens the list instead of the knob menu
# (the action on a "List" handle bound to <key>__list), and so does a tap on the value text (a field over it, the
# stepper's pattern). The plugin keeps the list shut for a knob that isn't a choice at the moment.
patch('''                key = "shKnob%d%s" % (r, sfx)
                defs.setdefault(key, _local(key, [_action("Mouse Down", "Q-Link"),
                                                  _action("Double Click", "Show Overlay", "knob overlay"),
                                                  _action("Enter Pressed", "Show Overlay", "knob overlay")], [''',
      '''                li = index.get(w["key"] + "__list")
                key = "shKnob%d%s%s" % (r, sfx, "_list" if li is not None else "")
                opener = ([dict(_action(ev, "Toggle Switch"), handleName="List") for ev in ("Double Click", "Enter Pressed")]
                          if li is not None else
                          [_action("Double Click", "Show Overlay", "knob overlay"), _action("Enter Pressed", "Show Overlay", "knob overlay")])
                defs.setdefault(key, _local(key, [_action("Mouse Down", "Q-Link")] + opener, [''')
patch('''                kids.append(_placed(key, name, i, w["cx"] - cw // 2, w["cy"] - s // 2, cw, ch))
''', '''                kids.append(_placed(key, name, i, w["cx"] - cw // 2, w["cy"] - s // 2, cw, ch,
                                    extra={"List": li} if li is not None else None))
                if li is not None:
                    vkey = "shKnobValueList_%d" % cw
                    defs.setdefault(vkey, _local(vkey, [_action("Mouse Down", "Toggle Switch"), _action("Enter Pressed", "Toggle Switch")],
                                                 [clear_label(_value_label(0, 0, cw, 26, 22.0, INK_DIM, handle="Text"))]))
                    kids.append(_placed(vkey, name + " list", li, w["cx"] - cw // 2, w["cy"] - s // 2 + value_y, cw, 26,
                                        focus="No", extra={"Text": i}))
                    on_top += pick_panel(t, w, li)
''')

patch('''OPEN_SUFFIX = "__open"''', '''PICK_KEYS = ["pick_%d" % (r + 1) for r in range(24)] + ["pick_prev", "pick_next", "pick_close", "pick_title"]
OPEN_SUFFIX = "__open"''')

open(path, "w").write(s)
