#!/usr/bin/env python3
"""Make 0dd Rig's setlist banks (rigs_setlist.h) from the user's band setlist (Hoochie Daddy, Nov 2026).
One bank per setlist section (A..G), two rigs per song in the sheet's order: the song's signature tone, then its
rhythm tone under it ("<song> Rhy": the same amp, the reverb, and for clean / acoustic songs their chorus / tremolo;
song n = Program Change 2n-2 and 2n-1). A signature with nothing beyond the rhythm gets a lead lift (a boost and a
delay, +1 dB). Each tone is a
base sound (clean / edge / crunch / heavy / fuzz / acoustic: amp words, the amp's EQ, what goes before the amp) plus
the song's signature touches (chorus, rotary, vibe, tremolo, phaser, octave, whammy, slap, delay, reverb), after
what's known of the records' gear. Our approximations; the amp model is the user's own capture, found by its words.
  make_setlist.py > ../src/rigs_setlist.h"""

# base sounds: amp words, cab words, NAM input/gate/bass/mid/treble/presence, output; slots before the amp
BASE = {
    "clean":  ("twin,deluxe,jc120,ac30,fender", "2x12,1x12", (-2, -100, 5, 5, 6.5, 6.5), 3, [("Compressor", "Opto Smooth", None, None)]),
    "edge":   ("ac30,deluxe,plexi,bassman,vox,fender", "2x12,greenback", (0, -100, 5, 5.5, 6.5, 6.5), 3,
               [("Compressor", "Clean Studio", 0.7, None), ("DrivPdls", "Kng o Tone", None, [None, 0.25, 0.55, 0.65])]),
    "crunch": ("plexi,jcm800,1959,jcm,marshall", "greenback,4x12", (0, -85, 5, 6, 6.5, 6.5), 3,
               [("DrivPdls", "Tube Scrmr", None, [None, 0.3, 0.55, 0.75])]),
    "heavy":  ("rectifier,dual rec,mesa,5150,jcm800,marshall", "v30,4x12", (2, -60, 5.5, 5, 5.5, 5.5), 2,
               [("DrivPdls", "Tube Scrmr", None, [None, 0.12, 0.6, 0.85])]),
    "fuzz":   ("plexi,hiwatt,marshall,fender", "4x12,greenback", (0, -80, 5.5, 6, 6, 5.5), 2,
               [("DrivPdls", "Big Muf", None, [None, 0.55, 0.5, 0.7])]),
    "acoustic": ("twin,deluxe,jc120,fender", "2x12,1x12", (-3, -100, 5, 5, 5.5, 5), 3,
               [("Compressor", "Clean Studio", None, None), ("Acoustic Sim", "Concert Clean", None, None)]),
}
# touches: (effect, preset, mix, knobs) after the amp, or "pre:" ones before it
T = {
    "chorus":  ("Chorus", "Warmth", 0.25, None),
    "lush":    ("TricraChrs", "Summer", 0.3, None),
    "rotary":  ("TricraChrs", "Leslie Falsa", 0.45, None),
    "vibe":    ("pre", "Even-Vib3", "Slowly Drifting", None, None),
    "univibe": ("pre", "Even-Vib3", "VibraVibe", None, None),
    "trem":    ("Tremolo", "Tumbleweed Trem", None, None),
    "phaser":  ("pre", "Phaser", "Phat Phase", 0.4, None),
    "octave":  ("pre", "Octaver", "Classic Octaver", None, None),
    "whammy":  ("pre", "PitchFlx", "Pitch Flex +1oct", None, [0]),   # (Bend at 0: dry until moved)
    "wah":     ("pre", "Sculpt3", "Wahcraft", None, None),
    "boost":   ("pre", "DrivPdls", "Klone", None, [None, 0.2, 0.55, 0.8]),
    "slap":    ("Tape Echo", "Classic Slap", 0.2, None),
    "delay":   ("Digital Delay", "Pristine Digital Delay", 0.22, None),
    "bigdelay": ("Digital Delay", "Big Warm Digital", 0.3, None),
    "tape":    ("Tape Echo", "Echo", 0.25, None),
    "room":    ("Room", "Guitar Room", 0.15, None),
    "plate":   ("Plate", "GuitPlate", 0.2, None),
    "spring":  ("Spring", "Deluxe", 0.25, None),
    "hall":    ("Hall", "Hall", 0.22, None),
}
T = {k: (v if v[0] == "pre" else ("post",) + v) for k, v in T.items()}

SECTIONS = [
 ("A Grunge Core", [
  ("Even Flow", "crunch", "room"), ("Alive", "crunch", "delay room"), ("Corduroy", "crunch", "room"),
  ("Rearviewmirror", "crunch", "room"), ("Jeremy", "edge", "chorus room"), ("Better Man", "edge", "delay plate"),
  ("Interstate Love Song", "edge", "lush room"), ("Plush", "crunch", "lush room"), ("Wicked Garden", "crunch", "room"),
  ("Creep", "edge", "chorus plate"), ("Big Empty", "edge", "trem spring"), ("Rooster", "heavy", "chorus room"),
  ("Would?", "heavy", "room"), ("No Excuses", "clean", "chorus delay plate"), ("Comedown", "crunch", "chorus room"),
  ("Shine", "heavy", "room"), ("Vasoline", "crunch", "room"), ("Dead & Bloated", "fuzz", "chorus room"),
  ("Them Bones", "heavy", "room"), ("Machinehead", "heavy", "chorus room"), ("Plowed", "fuzz", "room"),
  ("Bound for the Floor", "fuzz", "octave room"), ("Touch, Peel and Stand", "acoustic", "plate"),
  ("Far Behind", "edge", "chorus delay plate")]),
 ("B Cornell", [
  ("Fell on Black Days", "crunch", "chorus room"), ("Black Hole Sun", "crunch", "rotary room"),
  ("Outshined", "heavy", "room"), ("Spoonman", "heavy", "room"), ("Pretty Noose", "crunch", "wah room"),
  ("Like a Stone", "edge", "whammy delay plate"), ("Show Me How to Live", "heavy", "whammy room"),
  ("Cochise", "heavy", "whammy room"), ("Be Yourself", "edge", "delay plate"),
  ("I Am the Highway", "edge", "chorus delay plate"), ("Hunger Strike", "clean", "chorus delay plate"),
  ("Say Hello 2 Heaven", "clean", "bigdelay hall")]),
 ("C Southern", [
  ("Simple Man", "edge", "plate"), ("Gimme Three Steps", "crunch", "room"), ("Tuesday's Gone", "clean", "lush plate"),
  ("Call Me the Breeze", "crunch", "slap room"), ("Midnight Rider", "acoustic", "plate"), ("Ramblin' Man", "edge", "slap room"),
  ("One Way Out", "crunch", "slap room"), ("Can't You See", "edge", "plate"), ("Flirtin' w/ Disaster", "crunch", "room"),
  ("Train, Train", "heavy", "room"), ("Hold On Loosely", "crunch", "chorus delay room"),
  ("Caught Up in You", "crunch", "chorus delay room"), ("Copperhead Road", "crunch", "room"),
  ("Soulshine", "edge", "delay hall"), ("Fly Me Courageous", "crunch", "room"), ("Got No Shame", "crunch", "room"),
  ("Peace Pipe", "crunch", "univibe room"), ("One Horse Town", "crunch", "slap room"),
  ("Ain't Much Left of Me", "heavy", "room"), ("Alabama", "crunch", "room"),
  ("Simple Man (Shinedown)", "edge", "chorus delay plate"), ("Lonely Train", "heavy", "room"),
  ("And Fools Shine On", "edge", "delay plate"), ("My Sacrifice", "heavy", "delay room"),
  ("With Arms Wide Open", "clean", "chorus delay plate"), ("Higher", "heavy", "delay room")]),
 ("D Classic Rock", [
  ("Rockin' in Free World", "fuzz", "room"), ("Hey Hey, My My", "fuzz", "room"),
  ("Mary Jane's Last Dance", "edge", "chorus room"), ("Runnin' Down a Dream", "crunch", "slap room"),
  ("You Wreck Me", "crunch", "room"), ("Fortunate Son", "crunch", "spring"), ("Born on the Bayou", "edge", "trem spring"),
  ("Green River", "edge", "spring"), ("Sharp Dressed Man", "crunch", "slap room"), ("Turn the Page", "clean", "chorus hall"),
  ("Shooting Star", "edge", "plate"), ("All Right Now", "crunch", "room"), ("Highway to Hell", "crunch", "room"),
  ("Knockin' on Heaven's", "crunch", "delay plate"), ("Born to Be Wild", "fuzz", "room"), ("Roadhouse Blues", "edge", "room"),
  ("Pride and Joy", "edge", "boost spring"), ("All Along Watchtower", "fuzz", "univibe tape room"),
  ("Baba O'Riley", "crunch", "room"), ("Ramble On", "edge", "plate"), ("War Pigs", "fuzz", "room")]),
 ("E Butt Rock", [
  ("Kryptonite", "crunch", "room"), ("Here Without You", "clean", "chorus delay plate"), ("When I'm Gone", "heavy", "chorus room"),
  ("Hemorrhage", "edge", "chorus delay plate"), ("Shimmer", "clean", "chorus delay plate"),
  ("Lips of an Angel", "clean", "chorus delay plate"), ("Broken", "acoustic", "plate"), ("Paralyzer", "heavy", "room"),
  ("Through Glass", "acoustic", "plate"), ("Blurry", "heavy", "chorus room"), ("Remedy", "heavy", "room"),
  ("How You Remind Me", "heavy", "room"), ("Slither", "heavy", "wah room")]),
 ("F Foo Fighters", [
  ("My Hero", "heavy", "room"), ("Learn to Fly", "crunch", "room"), ("Monkey Wrench", "heavy", "room"),
  ("Best of You", "crunch", "room"), ("Times Like These", "edge", "chorus room"), ("The Pretender", "heavy", "delay room"),
  ("Everlong", "crunch", "chorus room"), ("All My Life", "heavy", "room")]),
 ("G Slow Ones", [
  ("Black", "edge", "delay plate"), ("Yellow Ledbetter", "clean", "vibe spring"), ("Daughter", "acoustic", "plate"),
  ("Last Kiss", "clean", "trem spring"), ("Nutshell", "acoustic", "chorus plate"), ("Sour Girl", "clean", "trem chorus plate"),
  ("Shelf in the Room", "acoustic", "plate"), ("Drift and Die", "clean", "chorus delay plate"),
  ("Doesn't Remind Me", "edge", "chorus delay room")]),
]

# the Rig display shows 23 characters: shorter names for the long ones' rhythm rigs
SHORT = {"Interstate Love Song": "Interstate Love", "Touch, Peel and Stand": "Touch Peel Stand",
         "Flirtin' w/ Disaster": "Flirtin w/ Disaster", "Ain't Much Left of Me": "Ain't Much Left",
         "Simple Man (Shinedown)": "Simple Man (Shine)", "Rockin' in Free World": "Rockin Free World",
         "Mary Jane's Last Dance": "Mary Jane's Last", "Runnin' Down a Dream": "Runnin Down a Dream",
         "Knockin' on Heaven's": "Knockin on Heaven", "All Along Watchtower": "Watchtower"}

def f(v): return ("%gf" % v) if float(v) != int(v) else "%d" % v
def slot(fx, pre, mix, knobs):
    ks = "NOK" if not knobs else "{" + ", ".join("K_" if k is None else f(k) for k in (list(knobs) + [None] * 8)[:8]) + "}"
    return '{"%s", %s, %s, %s}' % (fx, '"%s"' % pre if pre else "NULL", "M_" if mix is None else f(mix), ks)

print("/* generated by setlist/make_setlist.py: 0dd Rig's setlist banks (the user's band setlist, one rig per song) */")
def rig(name, base, touches, up):
    amp, cab, (inp, gate, b, m, t, p), out, pre = BASE[base]
    pre = list(pre); post = []
    for tk in touches:
        x = T[tk]
        if x[0] == "pre": pre.append(x[1:])
        else: post.append(x[1:])
    slots = pre + post
    assert len(slots) <= 6, name
    assert len(name) <= 23, name
    s = [slot(*x) for x in slots] + ["OFF"] * (6 - len(slots))
    print('    {"%s", "%s", "%s", %d, %s, %s, %s, %s, %s, %s, %s,' % (name, amp, cab, len(pre), f(inp), f(gate), f(b), f(m), f(t), f(p), f(out + up)))
    print("     {%s}}," % ", ".join(s))

banks = []
for bi, (bank, songs) in enumerate(SECTIONS):
    arr = "SET_%d" % bi
    banks.append((bank, arr))
    print("static const Rig %s[] = {" % arr)
    for name, base, touches in songs:
        tk = touches.split()
        keep = {"room", "plate", "spring", "hall"} | ({"chorus", "lush", "trem"} if base in ("clean", "acoustic") else set())
        rhy = [x for x in tk if x in keep]
        sig, lift = tk, 0
        if sig == rhy:
            sig = (["boost"] if base in ("edge", "crunch", "heavy") else []) + ["delay"] + tk
            lift = 1
        for nm, tks, up in ((name, sig, lift), (SHORT.get(name, name) + " Rhy", rhy, 0)):
            rig(nm, base, tks, up)
    print("};")
print("#define SETLIST_BANKS \\")
print(" \\\n".join('    {"%s", %s, (int)(sizeof %s / sizeof *%s)},' % (n, a, a, a) for n, a in banks))
