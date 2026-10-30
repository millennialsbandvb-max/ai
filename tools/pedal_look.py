#!/usr/bin/env python3
"""The "pedal look" for our pages, patched into a copy of mpc-vst-plugins' html_art.py (the kit's browser renderer).
   pedal_look.py <html_art.py>

Adds what tools/pedal_look.css needs: the page background (a "clear" in the theme's bg colour) gets class "plate"
(an enclosure gradient with a hammertone finish), gradients/filters for it and for metal knob faces, and two theme
keys, plate_hi / plate_lo (the enclosure's top and bottom colours; a layout sets them with theme_plate_hi= ...).
Chromium: $PW_CHROMIUM if set (this container's /opt/pw-browsers/chromium), else Playwright's own."""
import sys

path = sys.argv[1]
s = open(path).read()


def patch(old, new):
    global s
    if s.count(old) != 1:
        raise SystemExit("pedal_look.py: no longer applies to %s: %r" % (path, old[:60]))
    s = s.replace(old, new)


patch('''"display_ink": "cdeb63", "display_off": "2c3a1d", "display_bezel": "0d1108"}''',
      '''"display_ink": "cdeb63", "display_off": "2c3a1d", "display_bezel": "0d1108",
         "plate_hi": "2a2a2a", "plate_lo": "141414"}''')
patch('''</defs>"""''', '''<linearGradient id="enclosure" x1="0" y1="0" x2="0.25" y2="1">
 <stop offset="0" style="stop-color:var(--plate-hi)"/><stop offset="1" style="stop-color:var(--plate-lo)"/>
</linearGradient>
<radialGradient id="knob-metal" cx="0.38" cy="0.3" r="0.85">
 <stop offset="0" stop-color="#fbfbf9"/><stop offset="0.4" stop-color="#cfcfcb"/><stop offset="0.8" stop-color="#85857f"/>
 <stop offset="1" stop-color="#4d4d48"/>
</radialGradient>
<filter id="hammer" x="0" y="0" width="100%" height="100%">
 <feTurbulence type="fractalNoise" baseFrequency="0.035 0.045" numOctaves="3" seed="7" result="n"/>
 <feColorMatrix in="n" type="saturate" values="0" result="g"/>
 <feComponentTransfer in="g" result="soft"><feFuncA type="linear" slope="0" intercept="0.22"/></feComponentTransfer>
 <feBlend in="soft" in2="SourceGraphic" mode="soft-light" result="dimpled"/>
 <feTurbulence type="fractalNoise" baseFrequency="0.9" numOctaves="1" seed="3" result="f"/>
 <feColorMatrix in="f" type="matrix" values="0 0 0 0 1  0 0 0 0 1  0 0 0 0 1  0 0 0 0.6 -0.25" result="fine"/>
 <feComposite in="fine" in2="dimpled" operator="in" result="fine2"/>
 <feBlend in="fine2" in2="dimpled" mode="overlay"/>
</filter>
</defs>"""''')
patch('''            self.ops = ['<rect x="0" y="0" width="%d" height="%d" fill="%s"/>' % (W, H, hexc(a[1]))]''',
      '''            plate = a[1].strip().lstrip("#").lower() == self.theme["bg"].lower()
            self.ops = ['<rect%s x="0" y="0" width="%d" height="%d" fill="%s"/>' % (' class="plate"' if plate else "", W, H, hexc(a[1]))]''')
patch('''            browser = pw.chromium.launch()''',
      '''            browser = pw.chromium.launch(executable_path=os.environ.get("PW_CHROMIUM") or None)''')
open(path, "w").write(s)
