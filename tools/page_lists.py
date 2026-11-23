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
                    need += PICK_KEYS + ([k + "__sel"] if k + "__sel" in index else [])
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
                    # the field is built like the < > arrows (device-proven with the wheel): a button over its own
                    # background, on the list's "<key>__sel" parameter, and a touch gives it the data wheel (Q-Link; one
                    # Mouse Down action only: with two, the wheel never came). picklist.h: first tap selects (the wheel
                    # steps), the next opens the list.
                    fimg = "sh_field_%d_%s.png" % (t, w["key"])
                    key = "shStepSel_%d_%s" % (t, w["key"])
                    defs.setdefault(key, _local(key, [_action("Enter Pressed", "Toggle Switch"), _action("Mouse Down", "Q-Link")],
                                                [_focus(fw, h), _button(fimg, fimg, 1, 1, fw, h),
                                                 _value_label(8, 0, fw - 16, h, 26.0, DISPLAY_INK if dot else ACCENT, handle="Text")]))
                    kids.append(_placed(key, name, index[w["key"] + "__sel"], x0 + h + 3, y0, fw, h, extra={"Text": gi}))
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

patch('''def build(layout_path, params, skin_dir, art_bin, png_from_ppm):''', '''def fader_strip(path, w, h):
    """A narrow slider's filmstrip (tools/page_lists.py): FRAMES - 1 frames of w x h (the numFrames we give MPC: its own
    pages' strips hold exactly numFrames frames, of any shape), frame k = value k / (that - 1), the cap at the bottom
    for 0. Drawn at 3x and scaled down, RGBA (the page shows through)."""
    from PIL import Image, ImageDraw
    S, cap_h = 3, 22
    W_, H_ = w * S, h * S
    frame0 = Image.new("RGBA", (W_, H_), (0, 0, 0, 0))
    d = ImageDraw.Draw(frame0)
    top, bot = cap_h * S // 2 + 2 * S, H_ - cap_h * S // 2 - 2 * S
    for q in range(5):   # ticks: top, quarters, middle (wider), bottom
        y = top + (bot - top) * q // 4
        half = W_ // 2 if q == 2 else W_ // 3
        d.rectangle((W_ // 2 - half, y - S, W_ // 2 + half, y + S), fill=(30, 36, 32, 255) if q == 2 else (60, 70, 62, 230))
    d.rounded_rectangle((W_ // 2 - 4 * S, top - 6 * S, W_ // 2 + 4 * S, bot + 6 * S), radius=4 * S, fill=(14, 16, 15, 255))
    nf = FRAMES - 1
    out = Image.new("RGBA", (w, h * nf), (0, 0, 0, 0))
    for k in range(nf):
        f = frame0.copy()
        d = ImageDraw.Draw(f)
        cy = bot - (bot - top) * k / (nf - 1)
        x0, x1, y0, y1 = 2 * S, W_ - 2 * S, cy - cap_h * S / 2, cy + cap_h * S / 2
        d.rounded_rectangle((x0 + S, y0 + 2 * S, x1 + S, y1 + 2 * S), radius=3 * S, fill=(0, 0, 0, 110))   # shadow
        d.rounded_rectangle((x0, y0, x1, y1), radius=3 * S, fill=(232, 233, 228, 255), outline=(90, 92, 88, 255), width=S)
        for g in range(1, 4):   # a little shading
            d.line((x0 + 2 * S, y0 + g * 2 * S, x1 - 2 * S, y0 + g * 2 * S), fill=(250, 250, 248, 255), width=S)
        d.rectangle((x0 + S, cy - S, x1 - S, cy + S), fill=(20, 22, 20, 255))   # the line
        out.paste(f.resize((w, h), Image.LANCZOS), (0, k * h))
    out.save(path)


def vu_strip(path, w, h):
    """A VU meter's needle (tools/vu_meter.py has the face and the same geometry): FRAMES - 1 frames, frame k = GR
    20 * k / (that - 1) dB, transparent but for the needle and the pivot's cover."""
    import math
    from PIL import Image, ImageDraw
    S, nf = 3, FRAMES - 1
    px, py, r = w * S / 2, h * S * 1.18, h * S * 0.98
    out = Image.new("RGBA", (w, h * nf), (0, 0, 0, 0))
    for k in range(nf):
        gr = 20.0 * k / (nf - 1)
        a = math.radians(-46 + 92 * (1 - (gr / 20) ** 0.6))
        f = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
        d = ImageDraw.Draw(f)
        x1, y1 = px + r * 0.93 * math.sin(a), py - r * 0.93 * math.cos(a)
        x0, y0 = px + r * 0.30 * math.sin(a), py - r * 0.30 * math.cos(a)
        d.line((x0 + 2 * S, y0 + 2 * S, x1 + 2 * S, y1 + 2 * S), fill=(0, 0, 0, 70), width=2 * S)   # shadow
        d.line((x0, y0, x1, y1), fill=(20, 18, 16, 255), width=2 * S)
        d.rectangle((6 * S, h * S * 0.84, w * S - 6 * S, h * S - 6 * S), fill=(40, 38, 34, 255))   # the pivot's cover
        out.paste(f.resize((w, h), Image.LANCZOS), (0, k * h))
    out.save(path)


def build(layout_path, params, skin_dir, art_bin, png_from_ppm):''')

patch('''OPEN_SUFFIX = "__open"''', '''PICK_KEYS = ["pick_%d" % (r + 1) for r in range(24)] + ["pick_prev", "pick_next", "pick_close", "pick_title"]
OPEN_SUFFIX = "__open"''')

# narrow vertical sliders (slider_v ... narrow=1, the GQ31 graphic's 31 bands): the stock slider is a 130+ px wide
# component (square filmstrip frames, live name and value text under it), so side by side they'd overlap and a touch
# could move the next band. This one is exactly the slider's own size, and so is its filmstrip (our own drawing, a
# fader: black slot, scale ticks at the ends, quarters and middle, a white cap with a line; frames as tall as the
# slider: a square one hung over the sides drew wrong on the MPC), no live text; its label is printed under it.
patch('''            elif kind in ("slider_v", "slider_h"):
                sw_, sh_ = w["w"], w["h"]''', '''            elif kind == "slider_v" and w.get("narrow"):
                sw_, sh_ = w["w"], w["h"]
                vu = bool(w.get("vu"))   # (a VU needle over a printed face: tools/vu_meter.py; no touches)
                img = "sh_%s_%dx%d" % ("vu" if vu else "fader", sw_, sh_)
                if not os.path.exists(os.path.join(skin_dir, img + ".png")):
                    (vu_strip if vu else fader_strip)(os.path.join(skin_dir, img + ".png"), sw_, sh_)
                key = "shSlider%s_%dx%d" % ("VU" if vu else "N", sw_, sh_)
                defs.setdefault(key, _local(key, [] if vu else [_action("Mouse Down", "Q-Link")], [
                    _focus(sw_, sh_),
                    _sub("Knob", {"version": 5, "knobType": "FilmStrip", "filmStrip": img + ".png", "numFrames": FRAMES - 1,
                                  "invert": False, "dragOrientation": "Vertical", "handleName": "Data"},
                         _bounds(0, 0, sw_, sh_), "Slider")]))
                kids.append(_placed(key, name, i, w["cx"] - sw_ // 2, w["cy"] - sh_ // 2, sw_, sh_, focus="No" if vu else "Yes"))
            elif kind in ("slider_v", "slider_h"):
                sw_, sh_ = w["w"], w["h"]''')
patch('''    if not lab or k in ("knob", "toggle", "slider_v", "slider_h"):
        return []''', '''    if lab and k == "slider_v" and w.get("narrow"):   # (page_lists.py: a narrow slider's label, printed)
        return ["text|%d|%d|%s|%s|%s" % (w["cx"], w["cy"] + w["h"] // 2 + 18, 0.95, INK, lab)]
    if not lab or k in ("knob", "toggle", "slider_v", "slider_h"):
        return []''')

# the < > arrows: a touch also points the data wheel (and Q-Link) at the arrow (its Data, as a knob does: an action on
# another handle did nothing on the MPC); the plugins' arrows rest at 0.5 and step by the wheel's direction
# (tools/picklist.h)
patch('''                    defs.setdefault(akey, _local(akey, [_action("Enter Pressed", "Toggle Switch")],
                                                 [_button(aimg, aimg, 1, 1, aw, ah)]))
                    side_key = w.get(side, w["key"] + "_" + side)
                    kids.append(_placed(akey, "%s %s" % (name, side), index[side_key], ax, ay, aw, ah, focus="No"))''', '''                    defs.setdefault(akey, _local(akey, [_action("Enter Pressed", "Toggle Switch"),
                                                        _action("Mouse Down", "Q-Link")],
                                                 [_button(aimg, aimg, 1, 1, aw, ah)]))
                    side_key = w.get(side, w["key"] + "_" + side)
                    kids.append(_placed(akey, "%s %s" % (name, side), index[side_key], ax, ay, aw, ah))''')

# a list stepper's field: its background cropped as its button's picture (as the arrows' are)
patch('''                script.append("crop|%s|%d|%d|%d|%d" % (art(img), ax, ay, aw, ah))''', '''                script.append("crop|%s|%d|%d|%d|%d" % (art(img), ax, ay, aw, ah))
            if w["key"] + "__sel" in index:
                fh = w["h"]
                script.append("crop|%s|%d|%d|%d|%d" % (art("sh_field_%d_%s" % (t, w["key"])), w["cx"] - w["w"] // 2 + fh + 3,
                                                       w["cy"] - w["h"] // 2, w["w"] - 2 * fh - 6, fh))
            if cond(w):   # (back to the base screen for the next stepper)
                script.append("clear|" + PLATE)
                for b_ in base:
                    script += baked_cmds(b_, TITLE_FONT, base_dir)''')

# a stepper shown only in one mode (when=): its arrows / field are cut from the base screen redrawn with the stepper
# on it (the base screen alone has no arrows there, so the tap images hid the mode's arrows)
patch('''            for side, (ax, ay, aw, ah) in zip(("prev", "next"), stepper_arrows(w)):
                img = "sh_arrow_%d_%s_%s" % (t, w["key"], side)''', '''            if cond(w):
                script.append("clear|" + PLATE)
                for b_ in base + [w]:
                    script += baked_cmds(b_, TITLE_FONT, base_dir)
            for side, (ax, ay, aw, ah) in zip(("prev", "next"), stepper_arrows(w)):
                img = "sh_arrow_%d_%s_%s" % (t, w["key"], side)''')

# list options (TONE3000's page): font=<px> for the rows' text; button=1 draws each tile as a filled button
# (theme_btn_bg, lighter while its value is 1) with its live text centred in theme_btn_text: big tap targets whose
# words the plugin sets (the kit's own buttons are 39 px tall with baked words); fill=RRGGBB its own colour; imgs="a.png,b.png"
# a picture per tile drawn on the button (icons: no font glyphs needed); card=<px>: each tile a card, a `picture`
# line shows above and a name bar of that height below (lit while its value is 1), the whole card takes the tap
patch('''            elif kind == "list":
                for slot, ((x, y, tw, th), sk) in enumerate(zip(list_tiles(w), list_keys(w))):
                    img = "sh_tile_%dx%d" % (tw, th)
                    for state, border in (("on", 3), ("off", 0)):
                        script += ["clear|" + under(), "tile|%d|%d|%d|%d|%s|%s|%d" % (x, y, tw, th, LCD, SEG_ON if border else LINE, border),
                                   "crop|%s|%d|%d|%d|%d" % (art("%s_%s" % (img, state)), x, y, tw, th)]
                    key = "shRow_%dx%d" % (tw, th)''', '''            elif kind == "list":
                lbtn, lfont = bool(w.get("button")), float(w.get("font") or 24.0)
                icons = [f.strip() for f in w.get("imgs", "").split(",")] if w.get("imgs") else []
                card = int(w.get("card") or 0)
                for slot, ((x, y, tw, th), sk) in enumerate(zip(list_tiles(w), list_keys(w))):
                    if card:   # a card: a picture above (a `picture` line), the name bar below; the whole card takes the tap
                        img = "sh_card_%s_%d" % (w["key"], slot)
                        for state, border in (("on", 3), ("off", 0)):
                            script += ["clear|" + under(), "tile|%d|%d|%d|%d|%s|%s|%d" % (x, y + th - card, tw, card, LCD, SEG_ON if border else LINE,
                                                                                           border or 1),
                                       "crop|%s|%d|%d|%d|%d" % (art("%s_%s" % (img, state)), x, y + th - card, tw, card)]
                        key = "shCard_%s_%d" % (w["key"], slot)
                        defs.setdefault(key, _local(key, [_action("Mouse Down", "Toggle Switch"), _action("Enter Pressed", "Toggle Switch")],
                                                    [_focus(tw, th), _button(img + "_on.png", img + "_off.png", 1, 1, tw, card, 0, th - card),
                                                     _value_label(12, 0, tw - 24, th - (card - lfont * 1.2) // 2, lfont, INK,
                                                                  "horizontallyCentred bottom")]))
                        # (the label covers the card, so a tap on the picture counts too; its text sits in the name bar)
                        kids.append(_placed(key, "%s %d" % (name, slot + 1), index[sk], x, y, tw, th, focus="Yes" if slot == 0 else "No"))
                        continue
                    img = "sh_%stile_%dx%d" % ("b" if lbtn else "", tw, th)
                    if icons:
                        img = "sh_itile_%s_%d" % (w["key"], slot)
                    for state, border in (("on", 3), ("off", 0)):
                        if lbtn:
                            fill = shade(w.get("fill") or BTN_BG or ACCENT, 1.25 if border else 1.0)
                            script += ["clear|" + under(), "tile|%d|%d|%d|%d|%s|%s|2" % (x, y, tw, th, fill, shade(fill, 0.7))]
                            if slot < len(icons) and icons[slot]:   # an icon drawn on the button (no font glyphs needed)
                                script.append("image|%s|%d|%d|%d|%d|contain" % (os.path.abspath(os.path.join(base_dir, icons[slot])),
                                                                                 x + 4, y + 4, tw - 8, th - 8))
                            script.append("crop|%s|%d|%d|%d|%d" % (art("%s_%s" % (img, state)), x, y, tw, th))
                            continue
                        script += ["clear|" + under(), "tile|%d|%d|%d|%d|%s|%s|%d" % (x, y, tw, th, LCD, SEG_ON if border else LINE, border),
                                   "crop|%s|%d|%d|%d|%d" % (art("%s_%s" % (img, state)), x, y, tw, th)]
                    if lbtn:
                        key = "shBtnTile_%s_%d" % (img, lfont)
                        defs.setdefault(key, _local(key, [_action("Mouse Down", "Toggle Switch"), _action("Enter Pressed", "Toggle Switch")],
                                                    [_focus(tw, th), _button(img + "_on.png", img + "_off.png", 1, 1, tw, th),
                                                     _value_label(12, 0, tw - 24, th, lfont, BTN_TEXT or INK,
                                                                  "horizontallyCentred verticallyCentred")]))
                        kids.append(_placed(key, "%s %d" % (name, slot + 1), index[sk], x, y, tw, th, focus="Yes" if slot == 0 else "No"))
                        continue
                    key = "shRow_%dx%d" % (tw, th)''')
patch('''                                                 _value_label(12, 0, tw - 24, th, 24.0, ACCENT, "left verticallyCentred")]))''',
      '''                                                 _value_label(12, 0, tw - 24, th, lfont, ACCENT, "left verticallyCentred")]))''')
patch('''                    key = "shRow_%dx%d" % (tw, th)''', '''                    key = "shRow_%dx%d_%d" % (tw, th, lfont)''')


open(path, "w").write(s)