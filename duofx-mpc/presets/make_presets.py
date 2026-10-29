#!/usr/bin/env python3
"""Make Duo FX's preset bank: our versions of the H90's factory algorithm presets.
   make_presets.py <h90_preset_names.json> <table.json> <out presets.h> [h90_backup.json [h90_video_programs.json]]
h90_preset_names.json: the factory preset names per H90 algorithm, as the H90 manual's Algorithm Guide lists them
(names only: the manual gives no settings). table.json: Duo FX's algorithm table (build/gen-table, -DDUO_FX_TABLE).
Each preset starts from its algorithm's own defaults and is shaped by what its name says (slap, long, dark, bright,
slow, octave, 5th, freeze, ...), plus a small variation from the name so no two are the same. These are guesses
from the names, made to A/B against the real thing; the settings are ours.
With h90_backup.json (h90_backup.py, from an H90 backup): every preset found there gets its real settings (converted
by h90_convert.py), missing ones are added, and the backup's factory programs become PROGRAMS[] (both slots and
the routing). With h90_video_programs.json (make_video_programs.py: guesses from the play-through video), the
factory programs the backup didn't have are added too."""
import hashlib
import json
import re
import sys

names, table = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
ALG = {a["name"]: i for i, a in enumerate(table)}
# the H90's algorithm -> ours (H90 algorithms we have no counterpart for are left out)
MAP = {"Band Delay": "Band Delay", "Bouquet Delay": "Bouqet Delay", "Digital Delay": "Digital Delay",
       "Ducked Delay": "Ducked Delay", "Filter Pong": "FiltrPong", "Head Space": "HeadSpce", "Mod Delay": "Mod Delay",
       "MultiTap": "MultiTp", "Reverse": "Reverse", "Tape Echo": "Tape Echo", "UltraTap": "UltraTp",
       "Vintage Delay": "Vintage Delay", "CrushStation": "CrushStatn", "PitchFuzz": "PtchFuzz", "Sculpt": "Sculpt3",
       "WeedWacker": "WeedWackr", "Crystals": "Crystalz", "Diatonic": "Diatonik", "H910 H949": "H91O",
       "HarModulator": "HarModulatr", "HarPeggiator": "HarPeggiatr", "MicroPitch": "MicroPtch", "Octaver": "Octaver",
       "PitchFlex": "PitchFlx", "Polyphony": "Polyphny", "Prism Shift": "PrismShft", "Quadravox": "Quadravx",
       "Resonator": "Resonator", "VocalTune": None, "Chorus": "Chorus", "Even-Vibe": "Even-Vib3", "Flanger": "Flanger",
       "Instant Flanger": "InstFlangr", "Instant Phaser": "InstPhasr", "Phaser": "Phaser", "TremoloPan": "Tremolo",
       "Tricerachorus": "TricraChrs", "Vibrato": "Even-Vib3", "Blackhole": "Blackhol3", "Hall": "Hall",
       "MangledVerb": "MangldVerb", "Plate": "Plate", "Room": "Room", "SP2016 Reverb": "SP2O16", "Spring": "Spring",
       "Shimmer": "ShimmrVerb", "TremoloVerb": "TremoloVrb", "Wormhole": "Wormhol3", "HotSawz": "PolySynt"}
# their trademarks inside preset names -> our spellings
RENAME = [("TriceraChorus", "TricraChrs"), ("Blackhole", "Blackhol3"), ("UltraTap", "UltraTp"), ("H910", "H91O"),
          ("H949", "H94G"), ("H9", "H-9"), ("MicroPitch", "MicroPtch"), ("Crystals", "Crystalz"), ("Wormhole", "Wormhol3"),
          ("Polyphony", "Polyphny"), ("Quadravox", "Quadravx"), ("Instant Flanger", "InstFlangr"),
          ("Instant Phaser", "InstPhasr"), ("MangledVerb", "MangldVerb"), ("SP2016", "SP2O16"), ("Even-Vibe", "Even-Vib3"),
          ("WeedWacker", "WeedWackr"), ("CrushStation", "CrushStatn"), ("Prism Shift", "PrismShft"),
          ("HarModulator", "HarModulatr"), ("Sculpt", "Sculpt3")]

KIND = [  # knob name -> its role
    ("time", r"^(time|spacing|span|length|delay|delay [ab]|delay [lr]|slice|grain|predelay)$"),
    ("fb", r"^(feedback|decay|repeats)$"),
    ("tone", r"^(tone|high cut|cutoff|filter|band|center|mid)$"),
    ("mod", r"^(mod|depth|wow|flutter|drift|warp|ensemble|intensity|sweep|pitch mod|pos mod|throb|trem)$"),
    ("rate", r"^(rate|speed|trem rate|mod rate|lfo rate|chop)$"),
    ("size", r"^(size)$"),
    ("drive", r"^(drive|fuzz|saturate|distortion|low drive|high drive|age|comp)$"),
    ("pitch", r"^(pitch|pitch [ab]|step \d|range [ab]|toe)$"),
    ("width", r"^(width|spread)$"),
]
def kind(n):
    for k, rx in KIND:
        if re.match(rx, n.lower()):
            return k
    return None

RULES = [  # (words in the name, {role: change in 0..1}, mix change in %)
    (r"slap", {"time": -0.45, "fb": -0.3, "size": -0.3}, -10),
    (r"long|vast|epic|big|huge|giant|cathedral|hall|space|galax|star|nebul|cosmic|planet|universe|orbit|drone|pad|swell"
     r"|ambien|dream|ethereal|ghost|heaven|angel|mountain|ocean|cloud|haze|fog|infinite|vapor|aurora|pulsar|quasar|centaur",
     {"size": 0.25, "fb": 0.2, "time": 0.15, "mod": 0.1}, 12),
    (r"short|tight|small|tiny|nano|box|bedroom|little|thin|quick|shorty|tick", {"size": -0.25, "time": -0.25, "fb": -0.15}, -5),
    (r"dark|deep|low|warm|bottom|doom|cave|cavern|black|night|shadow|murk|submerg|drown|under|demon|hell|abyss|matter",
     {"tone": -0.25}, 0),
    (r"bright|sparkl|glitter|shine|diamond|crystal|air|fairy|hi fi|sun|summer|gold|glimmer|twinkl|chime|bell|ice|glass",
     {"tone": 0.2}, 0),
    (r"slow|lazy|drift|float|ebb|glacial|crawl|creep", {"rate": -0.3}, 0),
    (r"fast|hyper|nervous|quiver|jitter|buzz|flutter|insan|rapid|zipper|mosquito|spin|whirl", {"rate": 0.3, "mod": 0.1}, 0),
    (r"subtle|nice|gentle|soft|sweet|clean|pristine|classic|basic|simple|light", {"mod": -0.2, "drive": -0.2}, -8),
    (r"wow|warp|wobbl|seasick|wonk|melt|liquid|bend|swirl|syrup|lush|wide|thick", {"mod": 0.3, "width": 0.2}, 0),
    (r"drive|crush|fuzz|metal|distort|scream|dirt|hairy|stank|nasty|grit|power|heavy|punk|muscle|mangl|overload|rumble|boost",
     {"drive": 0.3}, 0),
    (r"vocal|vox|voice|sing", {"mod": -0.1, "drive": -0.2}, -6),
    (r"chorus", {"mod": 0.15}, 0),
    (r"trem|chop|pulse|pulsing|pulser|stutter|studder|throb|beat", {"rate": 0.2, "mod": 0.2}, 0),
    (r"freeze|forever|endless|eternal", {"fb": 1.0}, 15),
]

def h(name, salt):   # a stable small variation, -1..1
    return int(hashlib.md5((name + salt).encode()).hexdigest()[:6], 16) / 0x7fffff - 1

def norm(k, v):
    if k["map"] == 1:
        import math
        return max(0, min(1, math.log(v / k["lo"]) / math.log(k["hi"] / k["lo"])))
    return max(0, min(1, (v - k["lo"]) / (k["hi"] - k["lo"]) if k["hi"] != k["lo"] else 0))

def set_units(k, v):
    return norm(k, min(max(v, k["lo"]), k["hi"]))

def interval(name):
    n = name.lower()
    down = re.search(r"down|bass|low|sub|-\s*\d|-1oct|-2oct|drop|bottom", n) is not None
    if re.search(r"2\s*oct|3 octave|two oct", n): return -24 if down else 24
    if re.search(r"oct", n): return -12 if down else 12
    if re.search(r"5th|fifth|power", n): return -5 if down else 7
    if re.search(r"4th|fourth", n): return -7 if down else 5
    if re.search(r"3rd|third", n): return -8 if down else 4
    if re.search(r"2nd|second", n): return -2 if down else 2
    if re.search(r"unison", n): return 0
    return None

KEYS = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]
out = []
seen = set()
for h90, plist in names.items():
    ours = MAP.get(h90)
    if not ours:
        continue
    a = ALG[ours]
    alg = table[a]
    for raw in plist:
        name = raw.rstrip(":").strip()
        if not name or name.endswith(":"):
            continue
        for x, y in RENAME:
            name = name.replace(x, y)
        name = name[:22]
        key = (a, name.lower())
        if key in seen:
            continue
        seen.add(key)
        n = name.lower()
        knobs = [norm(k, k["def"]) if k else 0 for k in alg["knobs"]]
        mix = alg["mix"]
        for rx, ch, dm in RULES:
            if re.search(rx, n):
                mix += dm
                for i, k in enumerate(alg["knobs"]):
                    if k and k["map"] != 3 and kind(k["name"]) in ch:
                        r = kind(k["name"])
                        if r == "fb" and ch[r] >= 1.0: knobs[i] = 1.0
                        else: knobs[i] += ch[r]
        # the name's own variation, on the continuous knobs
        for i, k in enumerate(alg["knobs"]):
            if k and k["map"] in (0, 1) and kind(k["name"]) != "pitch":
                knobs[i] += 0.07 * h(name, k["name"])
        # intervals and keys named in the preset
        iv = interval(n)
        for i, k in enumerate(alg["knobs"]):
            if not k: continue
            kn = k["name"].lower()
            if iv is not None and kind(k["name"]) == "pitch" and k["unit"] == "st":
                knobs[i] = set_units(k, iv if i % 2 == 0 or "b" not in kn else (iv + 7 if abs(iv) < 12 else iv // 2))
            if k["map"] == 3 and kn == "key":
                m = re.search(r"\b([a-g](?:#|b)?)\s*(maj|min|m\b)", n)
                if m:
                    kk = m.group(1).capitalize().replace("Db", "C#").replace("Gb", "F#").replace("A#", "Bb").replace("D#", "Eb").replace("G#", "Ab")
                    if kk in KEYS: knobs[i] = KEYS.index(kk) / 11
            if k["map"] == 3 and kn == "mode" and "vibrato" in [c.lower() for c in k["choices"]] and ("vibrato" in n or h90 == "Vibrato" or "tribrato" in n):
                knobs[i] = 1.0
            if k["map"] == 3 and kn.startswith("voice") and iv is not None and "Unison" in k["choices"]:
                deg = {12: "Oct Up", -12: "Oct Down", 7: "5th Up", -5: "4th Down", 5: "4th Up", 4: "3rd Up", -8: "6th Down", 2: "2nd Up", -2: "2nd Down", 0: "Unison"}.get(iv)
                if deg and kn == "voice 1": knobs[i] = k["choices"].index(deg) / (len(k["choices"]) - 1)
            if kind(k["name"]) == "fb" and knobs[i] < 1.0:
                knobs[i] = min(knobs[i], 0.9)   # (full Feedback / Decay only where the name asks to hold)
            knobs[i] = max(0.0, min(1.0, knobs[i]))
        mix = max(10, min(100, mix + 4 * h(name, "mix")))
        if h90 == "Vibrato": mix = 100
        out.append((a, name, mix, knobs))

def from_h90(slot):   # a backup slot -> (algo, name, mix, knobs), or None
    import h90_convert
    r = h90_convert.convert(slot["algorithm"], slot["params"])
    if r is None:
        return None
    ours, mix, vals = r
    a = ALG[ours]
    name = slot["preset"]
    for x, y in RENAME:
        name = name.replace(x, y)
    name = name[:22]
    knobs = [norm(k, k["def"]) if k else 0 for k in table[a]["knobs"]]
    for i, k in enumerate(table[a]["knobs"]):
        if k and k["name"] in vals:
            v = vals[k["name"]]
            knobs[i] = k["choices"].index(v) / (len(k["choices"]) - 1) if k["map"] == 3 else set_units(k, v)
    return a, name, max(0, min(100, mix)), knobs

programs = []
if len(sys.argv) > 4:
    backup = json.load(open(sys.argv[4]))
    # a preset's real settings: from the program named like it if there is one (the factory default), else the first
    real = {}
    for prog in sorted(backup, key=lambda p: not p["factory"]):
        for slot in (prog["A"], prog["B"]):
            r = slot and from_h90(slot)
            if not r:
                continue
            key = (r[0], r[1].lower())
            if key not in real or prog["name"].lower() == slot["preset"].lower():
                real[key] = r
    idx = {(a, n.lower()): i for i, (a, n, _, _) in enumerate(out)}
    for key, r in real.items():
        if key in idx: out[idx[key]] = r
        else: out.append(r)
    print("%d presets with real settings (%d added)" % (len(real), len([k for k in real if k not in idx])))
    seen_p = set()
    for prog in backup:
        if not prog["factory"] or prog["name"].lower() in seen_p:
            continue
        seen_p.add(prog["name"].lower())
        slots = []
        for slot in (prog["A"], prog["B"]):
            r = slot and from_h90(slot)
            slots.append(r if r else (ALG["Off"], "", 0, [0] * 8))
        if all(r[0] == ALG["Off"] for r in slots):
            continue
        programs.append((prog["name"][:22], int(prog["parallel"]), slots))
    if len(sys.argv) > 5:   # the rest of the factory programs, guessed from the video
        for prog in json.load(open(sys.argv[5])):
            if prog["name"].lower() in seen_p:
                continue
            seen_p.add(prog["name"].lower())
            slots = []
            for sl in prog["slots"]:
                a = ALG[sl["algorithm"]]
                knobs = [norm(k, k["def"]) if k else 0 for k in table[a]["knobs"]]
                for i, k in enumerate(table[a]["knobs"]):
                    if k and k["name"] in sl["knobs"]:
                        v = sl["knobs"][k["name"]]
                        knobs[i] = k["choices"].index(v) / (len(k["choices"]) - 1) if k["map"] == 3 else set_units(k, v)
                slots.append((a, "", sl["mix"], knobs))
            programs.append((prog["name"], int(prog["parallel"]), slots))
    programs.sort(key=lambda p: p[0].lower())

# DrivPdls: one preset per pedal, at its sweet spot (not H90 presets: our pedal collection)
if "DrivPdls" in ALG:
    a = ALG["DrivPdls"]
    pk = table[a]["knobs"]
    for i, pedal in enumerate(pk[0]["choices"]):
        for suffix, drive in (("", 55), (" Max", 90)):
            knobs = [norm(k, k["def"]) if k else 0 for k in pk]
            knobs[0] = i / (len(pk[0]["choices"]) - 1)
            knobs[1] = drive / 100
            if (a, (pedal + suffix).lower()) not in {(x[0], x[1].lower()) for x in out}:
                out.append((a, pedal + suffix, 100, knobs))

out.sort(key=lambda p: (p[0], p[1].lower()))
with open(sys.argv[3], "w") as f:
    f.write("/* generated by presets/make_presets.py: our versions of the H90's factory algorithm presets (names from the\n"
            " * H90 manual's Algorithm Guide, settings guessed from the names; ours to A/B). {algorithm, name, mix 0..1,\n"
            " * knobs 0..1}, in algorithm order. */\n")
    f.write("static const struct { unsigned char algo; const char *name; float mix, k[8]; } PRESETS[] = {\n")
    for a, name, mix, knobs in out:
        f.write('    {%d, "%s", %.3ff, {%s}},\n' % (a, name.replace('"', "'"), mix / 100, ", ".join("%.3ff" % v for v in knobs)))
    f.write("};\n#define NPRESETS ((int)(sizeof PRESETS / sizeof *PRESETS))\n")
    if programs:
        f.write("\n/* the H90 factory programs found in a backup (presets/h90_backup.json): the real settings, converted to our\n"
                " * algorithms by presets/h90_convert.py. {name, parallel, {slot A, slot B}}, a slot as a preset. */\n")
        f.write("typedef struct { unsigned char algo; const char *name; float mix, k[8]; } ProgSlot;\n")
        f.write("static const struct { const char *name; int parallel; ProgSlot s[2]; } PROGRAMS[] = {\n")
        for name, par, slots in programs:
            f.write('    {"%s", %d, {%s}},\n' % (name.replace('"', "'"), par, ", ".join(
                '{%d, "%s", %.3ff, {%s}}' % (a, n.replace('"', "'"), m / 100, ", ".join("%.3ff" % v for v in k)) for a, n, m, k in slots)))
        f.write("};\n#define NPROGRAMS ((int)(sizeof PROGRAMS / sizeof *PROGRAMS))\n")
print("%d presets for %d algorithms, %d programs" % (len(out), len({p[0] for p in out}), len(programs)))
