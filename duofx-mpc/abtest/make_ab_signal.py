#!/usr/bin/env python3
"""Write the A/B test signal: ab_signal.py <out.wav>  (48 kHz, 16-bit, mono, ~30 s)
Play it through an H90 program and record the pedal's stereo output, then run it through Duo FX the same way;
abtest compares the two. Sections (each followed by silence so tails can be measured):
  0 s  three clicks 4 s apart       -> echo times, feedback, reverb decay (impulse responses)
 12 s  440 Hz sine, 4 s             -> pitch shifting, modulation rate and depth, tremolo
 18 s  log sweep 50 Hz-16 kHz, 4 s  -> frequency response, filters
 24 s  plucked A2 / E3 / A3 notes   -> how it behaves on a guitar-like note (drive, harmonies)"""
import math, struct, sys, wave, random
FS = 48000
x = [0.0] * (FS * 31)
for k in range(3):
    x[FS * 4 * k] = 0.9
for i in range(4 * FS):
    t = i / FS
    x[12 * FS + i] = 0.4 * math.sin(2 * math.pi * 440 * t) * min(1, t * 50, (4 - t) * 50)
    f0, f1 = 50, 16000
    ph = 2 * math.pi * f0 * 4 / math.log(f1 / f0) * (math.exp(t / 4 * math.log(f1 / f0)) - 1)
    x[18 * FS + i] = 0.4 * math.sin(ph) * min(1, t * 50, (4 - t) * 50)
random.seed(1)
for n, hz in enumerate((110.0, 164.81, 220.0)):   # Karplus-Strong plucks
    p = int(FS / hz); buf = [random.uniform(-1, 1) for _ in range(p)]
    s = 24 * FS + n * 2 * FS
    for i in range(2 * FS):
        v = buf[i % p]; buf[i % p] = 0.996 * 0.5 * (v + buf[(i + 1) % p])
        x[s + i] += 0.5 * v
w = wave.open(sys.argv[1], "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS)
w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, v)) * 32767)) for v in x))
w.close()
