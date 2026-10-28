#!/usr/bin/env python3
"""Guess the H90 factory programs a backup didn't have, from a play-through recording's measurements:
   make_video_programs.py <h90_video_measures.json> > h90_video_programs.json
h90_video_measures.json holds, per program (one chapter of the play-through video "Eventide H90 all factory programs
(No Talking)"), what abtest/analyze_video.py measured in its audio: echo lags and strengths (whitened autocorrelation;
L-R for ping-pong), decay times, the strongest level modulation, spectral flatness (drive), brightness, width and
which pitch intervals stand out. From that and the program's name we pick two of our algorithms and set their knobs
(in their own units; make_presets.py converts). These are guesses: the backup's programs (real settings) take
precedence over these wherever both exist."""
import json
import re
import sys

M = json.load(open(sys.argv[1]))
avg = [sum(m["intervals"][i] for m in M) / len(M) for i in range(48)]


def has(name, rx):
    return re.search(rx, name.lower()) is not None


def echo_info(m):
    """(time ms, feedback %, ping-pong, multitap spacing ms or None), or None"""
    ec = [e for e in m["echo"] if e[2] > 100 and e[0] > 120]
    lr = [e for e in (m.get("lr_echo") or []) if e[1] > 120 and abs(e[0]) > 120]
    if not ec and not lr:
        return None
    if ec:
        t, s = min(ec, key=lambda e: e[0])[0], max(e[2] for e in ec)
    else:
        t, s = min(abs(e[0]) for e in lr), max(e[1] for e in lr)
    two = [e for e in m["echo"] if abs(e[0] - 2 * t) < 8]
    one = [e for e in m["echo"] if abs(e[0] - t) < 8]
    fb = 25.0
    if two and one:
        fb = max(10.0, min(70.0, 100 * two[0][2] / one[0][2] * 0.8))
    ping = bool(lr) and max(e[1] for e in lr) > 1.2 * (max(e[2] for e in ec) if ec else 0)
    taps = sorted(e[0] for e in ec)
    multi = None
    if len(taps) >= 4:
        gaps = [b - a for a, b in zip(taps, taps[1:])]
        if max(gaps) - min(gaps) < 0.25 * min(gaps):
            multi = sum(gaps) / len(gaps)
    return t, fb, ping, multi, s


def standout(m):
    ex = {i: m["intervals"][i] - avg[i] for i in range(1, 30)}
    return {i for i, v in ex.items() if v > 0.02}


def program(m):
    n = m["name"]
    name = n
    for x, y in (("Black Hole", "Blackhol3"),):
        name = name.replace(x, y)
    rt = m.get("rt60_p80") or 0
    am_rate, am_str = m.get("am_rate"), m.get("am_strength") or 0
    drive = m["flatness"] > 0.02 or has(n, r"drive|dirt|distort|grunge|crunch|djent(?!le)|slasher|metal|stone|rusty|fuzz|ham|tight")
    if has(n, r"clean|gentle|djentle"):
        drive = False
    ivs = standout(m)
    echo = echo_info(m)
    slots = []   # (algorithm, mix, {knob: value}) in the order A, B

    # -- the first slot: drive, pitch, modulation, filter
    if has(n, r"wah|clav|funk|thumb|autowah"):
        slots.append(("Aggravat3", 100, {"Drive": 8 if not drive else 55, "Octave": 0, "Envelope": 85, "Env Q": 70,
                                         "Level": -6}))
    elif drive:
        if has(n, r"fuzz|ham|robocat"):
            slots.append(("PtchFuzz", 100, {"Fuzz": 70, "Pitch A": -12 if m["sub70"] > -40 else 0, "Pitch B": 12, "Pitch Mix": 30}))
        elif has(n, r"slasher|metal|djent|heavy|stone|power"):
            slots.append(("CrushStatn", 100, {"Drive": 75, "Sag": 20, "Comp": 50, "Tone": 55}))
        else:
            slots.append(("WeedWackr", 100, {"Drive": 60 if m["flatness"] > 0.04 else 40, "Tone": 50}))
    if has(n, r"arp"):
        slots.append(("HarPeggiatr", 60, {"Rate": 1000 / echo[0] if echo else 4}))
    elif has(n, r"prism"):
        slots.append(("PrismShft", 55, {"Rate": 1000 / echo[0] if echo else 3, "Delay": echo[0] if echo else 1}))
    elif has(n, r"12 string|stringy|power 12"):
        slots.append(("Polyphny", 45, {"Pitch A": 12, "Fine A": 6, "Pitch B": 0, "Fine B": -8, "Level A": 70, "Level B": 50}))
    elif has(n, r"fifth|harmonic|polyphon|chime") or (7 in ivs and 12 not in ivs):
        slots.append(("Polyphny", 45, {"Pitch A": 7, "Pitch B": 12 if 12 in ivs or has(n, r"chime") else -5, "Level B": 60}))
    elif has(n, r"robocat"):
        slots.append(("PolySynt", 60, {"Wave": "Square", "Octave": 0}))
    if has(n, r"vibe"):
        slots.append(("Even-Vib3", 50, {"Speed": am_rate if am_rate and am_rate < 8 else 2, "Intensity": 70}))
    elif has(n, r"phase|water wheel"):
        slots.append(("Phaser", 50, {"Rate": am_rate if am_rate and am_rate < 6 else 0.4, "Feedback": 50}))
    elif has(n, r"flang|twister|whip"):
        slots.append(("Flanger", 50, {"Rate": 0.3, "Feedback": 55}))
    elif has(n, r"lush|wide|warm|chorus|dream|paradise|indie|bittersweet") and not any(a == "Polyphny" for a, _, _ in slots):
        slots.append(("Chorus", 50, {"Depth": 50, "Rate": 0.7}))
    elif am_str > 20 and am_rate:
        slots.append(("Tremolo", 100, {"Depth": min(100, 60 + am_str), "Rate": am_rate}))
    if has(n, r"rewind|ghost"):
        slots.append(("Reverse", 45, {"Time": echo[0] * 2 if echo else 600, "Feedback": 30}))
    if has(n, r"asteroid|glitch"):
        slots.append(("Glitch", 50, {}))

    # -- echoes
    if echo:
        t, fb, ping, multi, s = echo
        mix = 30 if s < 200 else 40
        if multi and multi < t + 1:
            slots.append(("MultiTp", mix, {"Span": multi * 6, "Taps": 6, "Feedback": 15}))
        elif has(n, r"tape|twister|slap|van|tidal|wobbly"):
            slots.append(("Tape Echo", mix, {"Time": t, "Feedback": fb}))
        elif ping:
            slots.append(("Digital Delay", mix, {"Time": t, "Feedback": fb, "Ping-Pong": "On", "Mod": 15}))
        elif has(n, r"wah|lush|mod|wobbl|swim"):
            slots.append(("Mod Delay", mix, {"Time": t, "Feedback": fb, "Depth": 40}))
        else:
            slots.append(("Digital Delay", mix, {"Time": t, "Feedback": fb}))

    # -- space
    shimmer = 12 in ivs or 19 in ivs or has(n, r"celestial|awaken|transcend|nova|heaven|angel")
    if has(n, r"black|universe|space|andromeda|stargaz|galax|knife|epic|thieves|asteroid") or rt > 11:
        slots.append(("Blackhol3", 35, {"Size": 80, "Decay": max(8, min(60, rt * 2.5)), "Mod": 30}))
    elif shimmer and rt > 3:
        slots.append(("ShimmrVerb", 35, {"Decay": max(3, min(15, rt)), "Pitch A": 12, "Pitch B": 12 if 19 not in ivs else 19,
                                         "Shimmer": 45}))
    elif has(n, r"spring|surf|cowboy|twin pines"):
        slots.append(("Spring", 30, {"Decay": max(1, min(4, rt)), "Drip": 40}))
    elif has(n, r"room|bedroom|standby"):
        slots.append(("Room", 25, {"Decay": max(0.4, min(3, rt / 2)), "Size": 45}))
    elif has(n, r"plate|mosaic|recital|balance"):
        slots.append(("Plate", 28, {"Decay": max(1, min(6, rt)), "Size": 55}))
    elif rt > 6 or has(n, r"ambient|fields|fog|ocean|wave|warm|embrace|lost|city|wobbly|swim|aura"):
        slots.append(("Hall", 35, {"Decay": max(2.5, min(12, rt)), "Size": 70, "Mod": 40}))
    elif rt > 2.2:
        slots.append(("SP2O16", 30, {"Decay": max(1, min(6, rt * 0.8))}))

    # two slots: keep the first effect and the last (space or echo), as the H90's own programs mostly do
    if len(slots) > 2:
        slots = [slots[0], slots[-1]]
    if not slots:
        slots = [("Chorus", 40, {}), ("SP2O16", 25, {"Decay": 1.5})]
    if len(slots) == 1 and slots[0][0] not in ("Blackhol3", "ShimmrVerb", "Hall", "Room", "Plate", "Spring", "SP2O16"):
        # (the H90's programs nearly always use both slots: a small room after a drive, a short plate after the rest)
        slots.append(("Room", 20, {"Decay": 0.8}) if slots[0][0] in ("WeedWackr", "CrushStatn", "PtchFuzz", "Aggravat3")
                     else ("SP2O16", 22, {"Decay": 1.6}))
    while len(slots) < 2:
        slots.append(("Off", 0, {}))
    return {"name": n[:22], "parallel": False, "slots": [{"algorithm": a, "mix": mx, "knobs": k} for a, mx, k in slots]}


out = []
for m in M:
    out.append(program(m))
json.dump(out, sys.stdout, indent=1)
print()
