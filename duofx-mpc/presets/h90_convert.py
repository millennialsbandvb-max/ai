"""H90 preset settings (as h90_backup.py reads them from a backup) -> Duo FX: (our algorithm, mix %, {knob: value}).
The units are worked out from the values themselves and from the play-through video's audio (measured echo times):
 - H9-family delay and predelay settings below 60 aren't milliseconds: 2.45 * v^2 is (Shoegazer's Blackhole
   predelay 11.36 -> a 315 ms echo, Fading Fields' ModEchoVerb 12.37 -> 375 ms); 60 and up are milliseconds;
 - pitches are cents (Crystals, Shimmer), or 25 = unison (PitchFuzz);
 - Zero Gravity's Polyphony shifts 22 / 20 sound as +14 / +9 semitones (the audio's interval histogram).
Knobs not set here keep the algorithm's own default. Algorithms we have no counterpart for return None."""
import math


def ms(v):
    return v if v >= 60 else 2.45 * v * v


def g(p, *keys, d=0.0):
    for k in keys:
        if k in p and p[k] is not None:
            return float(p[k])
    return d


def cl(v, lo, hi):
    return max(lo, min(hi, v))


def tone_hz(hz, lo=800.0, hi=18000.0):   # a lowpass frequency -> our Tone % (tone_cut: 800 Hz .. 18 kHz, exponential)
    return 100 * math.log(cl(hz, lo, hi) / lo) / math.log(hi / lo)


SP_SIZE = {0: 45, 1: 35, 2: 30, 3: 70, 4: 55, 5: 55}   # SP2016's algorithm (Stereo Room, Room, ..., Plate) -> Size
POLY_SHIFT = {22: 14, 20: 9}


def convert(algorithm, p):
    mix = g(p, "mmix", "mix", "Mix", d=50)
    if algorithm == "Digital Delay":
        a, b = g(p, "dlya"), g(p, "dlyb")
        t, fb = (a, g(p, "fbka")) if a >= b else (b, g(p, "fbkb"))
        return "Digital Delay", mix, {"Time": ms(t), "Feedback": fb, "Mod": g(p, "mdpt"), "Rate": cl(g(p, "mspd", d=0.5), 0.05, 8)}
    if algorithm == "Vintage Delay":
        return "Vintage Delay", mix, {"Time": ms(g(p, "dlya")), "Feedback": g(p, "fbka"), "Bits": round(g(p, "bits", d=12)),
                                      "Mod": g(p, "mdpt") * 2, "Mod Rate": cl(g(p, "mspd"), 0.05, 8)}
    if algorithm == "Band Delay":
        return "Band Delay", mix, {"Time": ms(max(g(p, "dlya"), g(p, "dlyb"))), "Feedback": max(g(p, "fbka"), g(p, "fbkb")) * 0.85,
                                   "Resonance": 30 + g(p, "reso") * 2, "Sweep": g(p, "mdpt") * 2, "Rate": cl(g(p, "mspd"), 0.05, 8)}
    if algorithm == "Tape Echo":
        return "Tape Echo", mix, {"Time": ms(g(p, "dlya", d=350)), "Feedback": g(p, "fbka", d=45), "Wow": g(p, "wow_") * 5,
                                  "Flutter": g(p, "flut") * 5, "Saturate": g(p, "satr") * 4,
                                  "Tone": 1000 * 10 ** (g(p, "fltr", d=60) / 100)}
    if algorithm == "UltraTap":
        return "UltraTp", mix, {"Length": g(p, "lnth", d=500), "Taps": min(32, round(g(p, "taps", d=12))),
                                "Predelay": max(1, ms(g(p, "pdly"))), "Spread": g(p, "sprd"), "Taper": g(p, "tapr"),
                                "Smear": g(p, "slrm"), "Chop": max(0.1, g(p, "chop")), "Tone": 50 + 2 * g(p, "tone")}
    if algorithm == "Crystals":
        return "Crystalz", g(p, "pmix", d=50), {"Pitch A": round(g(p, "pcha") / 100), "Pitch B": round(g(p, "pchb") / 100),
                                                "Delay A": g(p, "dlya"), "Delay B": g(p, "dlyb"),
                                                "Feedback": (g(p, "fbka") + g(p, "fbkb")) / 2 * 0.8, "Reverb": g(p, "vmix")}
    if algorithm == "MicroPitch":   # (its delays are milliseconds)
        return "MicroPtch", g(p, "pmix", d=mix), {"Detune": g(p, "pcha", d=9), "Delay L": max(1, g(p, "dlya")),
                                                  "Delay R": max(1, g(p, "dlyb")), "Feedback": g(p, "fdbk"),
                                                  "Tone": 50 + g(p, "tone")}
    if algorithm == "H910 H949":   # (pitch as a ratio)
        return "H91O", mix, {"Pitch": round(12 * math.log2(max(0.25, g(p, "pcha", d=1)))), "Delay": max(1, g(p, "dlya")),
                             "Feedback": g(p, "fbka")}
    if algorithm == "Quadravox":
        deg = {12: "Oct Up", 11: "7th Up", 9: "6th Up", 7: "5th Up", 5: "4th Up", 4: "3rd Up", 3: "3rd Up", 2: "2nd Up",
               0: "Unison", -2: "2nd Down", -3: "3rd Down", -4: "3rd Down", -5: "4th Down", -7: "5th Down", -12: "Oct Down"}
        def voice(st):
            st = round(st)
            while st > 12: st -= 12
            while st < -12: st += 12
            return deg[min(deg, key=lambda k: abs(k - st))]
        return "Quadravx", g(p, "pmix", d=50), {"Voice 1": voice(g(p, "pcha")), "Voice 2": voice(g(p, "pchb")),
                                                "Voice 3": voice(g(p, "pchc")), "Voice 4": voice(g(p, "pchd")),
                                                "Delay": cl(g(p, "dlyl", d=1), 1, 300)}
    if algorithm == "Polyphony":
        s1, s2 = round(g(p, "Shift1", d=12)), round(g(p, "Shift2", d=12))
        return "Polyphny", mix, {"Pitch A": POLY_SHIFT.get(s1, s1 - 12), "Pitch B": POLY_SHIFT.get(s2, s2 - 12),
                                 "Fine A": g(p, "Detune1"), "Fine B": g(p, "Detune2"), "Level A": g(p, "LevelA", d=100),
                                 "Level B": g(p, "LevelB", d=70), "Width": abs(g(p, "Pan1")) / 2 + abs(g(p, "Pan2")) / 2}
    if algorithm == "TriceraChorus":
        return "TricraChrs", 50, {"Rate": cl(g(p, "rate", d=0.6), 0.05, 8), "Depth": cl(g(p, "ldep", d=50), 0, 100),
                                  "Ensemble": cl(g(p, "rdep", d=35), 0, 100)}
    if algorithm == "Flanger":
        return "Flanger", 50, {"Delay": 0.3 * (10 / 0.3) ** (g(p, "dlbs", d=40) / 100), "Depth": cl(g(p, "dpth") * 2, 0, 100),
                               "Rate": cl(g(p, "sped", d=0.25), 0.02, 8), "Feedback": cl(g(p, "itsy") * 6, -95, 95)}
    if algorithm == "Even-Vibe":
        return "Even-Vib3", mix, {"Intensity": cl(g(p, "intensity", d=7) * 10, 0, 100), "Speed": cl(g(p, "speed", d=2), 0.3, 10)}
    if algorithm == "Spring":
        return "Spring", mix, {"Decay": g(p, "dcay", d=2), "Tension": g(p, "tnsn", d=50), "Tone": g(p, "tint") or 50,
                               "Springs": ["1", "2", "3"][int(cl(g(p, "nspr", d=2), 1, 3)) - 1], "Drip": 20 + g(p, "reso") * 0.8}
    if algorithm == "SP2016 Reverb":
        return "SP2O16", mix, {"Size": SP_SIZE.get(int(g(p, "algo")), 55), "Decay": g(p, "decy", d=2.5),
                               "Predelay": max(1, g(p, "pdly")), "Position": g(p, "pstn"), "Diffusion": g(p, "dffn"),
                               "Tone": cl(70 + 4 * g(p, "eqhs"), 0, 100), "Low": g(p, "eqls")}
    if algorithm == "Room":
        return "Room", mix, {"Size": g(p, "size", d=40), "Decay": g(p, "dcay", d=0.8), "Predelay": max(1, ms(g(p, "pdly"))),
                             "Tone": g(p, "hilv", d=50), "Early": g(p, "ermx", d=50), "Mod": g(p, "mod_", d=15)}
    if algorithm == "Hall":
        return "Hall", mix, {"Size": g(p, "size", d=65), "Decay": g(p, "dcay", d=3), "Predelay": max(1, ms(g(p, "pdly"))),
                             "Tone": g(p, "hilv", d=55), "Low": (g(p, "lolv", d=50) - 50) / 50 * 6, "Mod": g(p, "mod_", d=30)}
    if algorithm == "ModEchoVerb":   # (we have no echo-into-reverb: a hall with its predelay as the echo)
        return "Hall", mix, {"Size": g(p, "size", d=65), "Decay": cl(g(p, "dcay", d=3), 0.5, 20),
                             "Predelay": max(1, ms(g(p, "pdly"))), "Tone": cl(55 + 0.3 * g(p, "hilv"), 0, 100),
                             "Low": g(p, "lolv"), "Mod": 30}
    if algorithm == "DualVerb":
        return "Hall", mix, {"Size": g(p, "size", d=65), "Decay": max(g(p, "adcy"), g(p, "bdcy"), 0.5),
                             "Tone": (g(p, "aton", d=55) + g(p, "bton", d=55)) / 2}
    if algorithm == "Shimmer":
        return "ShimmrVerb", mix, {"Size": g(p, "size", d=70), "Decay": 20 ** (g(p, "dcay", d=60) / 100),
                                   "Pitch A": round(g(p, "apch", d=1200) / 100), "Pitch B": round(g(p, "bpch", d=1900) / 100),
                                   "Shimmer": g(p, "pdcy", d=50), "Tone": g(p, "hdcy", d=60)}
    if algorithm == "Blackhole":
        return "Blackhol3", mix, {"Size": g(p, "size", d=75), "Decay": 4 + 0.4 * g(p, "fdbk", d=40),
                                  "Swell": cl(100 - g(p, "grav", d=100), 0, 100), "Tone": cl(45 + 0.6 * g(p, "hilv"), 0, 100),
                                  "Low": g(p, "lolv"), "Mod": g(p, "mdpt"), "Rate": 0.05 * 80 ** (g(p, "mrat", d=50) / 100),
                                  "Predelay": max(1, ms(g(p, "pdly")))}
    if algorithm == "TremoloVerb":
        return "TremoloVrb", mix, {"Size": g(p, "size", d=55), "Decay": g(p, "dcay", d=2.5), "Tone": tone_hz(g(p, "hifq", d=8000)),
                                   "Trem": g(p, "dpth", d=120) / 2, "Trem Rate": g(p, "sped", d=4), "Shape": g(p, "shap") * 100}
    if algorithm == "WeedWacker":
        return "WeedWackr", 100, {"Drive": g(p, "drive1", d=45), "Tone": 30 + 0.7 * g(p, "tone1Control"),
                                  "Level": -3 + g(p, "level1Out")}
    if algorithm == "CrushStation":
        return "CrushStatn", 100, {"Drive": g(p, "drve", d=55), "Sag": g(p, "ssag"), "Comp": g(p, "sstn", d=40),
                                   "Tone": 55 + 2 * g(p, "trbl"), "Bass": (g(p, "bass", d=50) - 50) / 50 * 12, "Gate": g(p, "gthr", d=-90)}
    if algorithm == "Sculpt":
        return "Sculpt3", mix, {"Split": g(p, "xovr", d=400), "Low Drive": g(p, "ldrv", d=30), "High Drive": g(p, "hdrv", d=60),
                                "Comp": g(p, "cmpr", d=30)}
    if algorithm == "PitchFuzz":
        dly = g(p, "dlya_denormalized_pretaper", d=-1)
        dly = dly if dly > 0 else ms(g(p, "dlya", d=1))
        fb = g(p, "fbck_denormalized_pretaper", d=-1)
        fb = fb if fb >= 0 else g(p, "fbck")
        return "PtchFuzz", 100, {"Fuzz": g(p, "fuzz", d=60), "Tone": 50 + 0.5 * g(p, "fztn"),
                                 "Pitch A": round(g(p, "pita", d=13) - 25), "Pitch B": round(g(p, "pitb", d=37) - 25),
                                 "Delay": cl(dly, 1, 1000), "Feedback": cl(fb * 0.6, 0, 90), "Pitch Mix": 50}
    return None
