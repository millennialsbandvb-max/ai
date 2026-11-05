import numpy as np, wave, io, base64, json, re
from scipy import signal
SR = 32000; rng = np.random.default_rng(21)
def t(sec): return np.arange(int(SR * sec)) / SR
def bp(x, lo, hi): b, a = signal.butter(2, [lo/(SR/2), hi/(SR/2)], 'band'); return signal.lfilter(b, a, x)
def norm(x, pk=0.95): return x / np.max(np.abs(x)) * pk
def fade(x, ms=6): n = int(SR*ms/1000); x[-n:] *= np.linspace(1, 0, n); return x
# Shaker: beads hitting the shell = dense filtered noise, quick swell then decay
sec = 0.16; x = t(sec); n = rng.standard_normal(len(x))
env = np.minimum(1, x / 0.012) * np.exp(-np.maximum(0, x - 0.012) / 0.045)
grains = (rng.random(len(x)) < 0.25) * rng.standard_normal(len(x)) * 0.6   # bead clatter
shaker = fade(norm(bp(n * 0.7 + grains, 3500, 11000) * env))
# Tambourine: drum-skin slap + many small jingles (inharmonic, bright, ringing)
sec = 0.45; x = t(sec)
slap = bp(rng.standard_normal(len(x)), 300, 2000) * np.exp(-x / 0.012) * 0.5
jing = np.zeros_like(x)
for f in [4120, 5230, 6170, 7020, 7940, 8810, 9650, 10400]:
    jing += np.sin(2 * np.pi * f * x + rng.random() * 6.28) * np.exp(-x / (0.09 + rng.random() * 0.08)) * (0.6 + rng.random() * 0.4)
jing += bp(rng.standard_normal(len(x)), 5000, 12000) * np.exp(-x / 0.05) * 0.6
jing *= 1 + 0.35 * np.sin(2 * np.pi * 28 * x)        # jingles rattling
tamb = fade(norm(slap + jing))
def wav_bytes(y):
    b = io.BytesIO(); w = wave.open(b, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes((np.clip(y, -1, 1) * 32767).astype('<i2').tobytes()); w.close(); return b.getvalue()
for name, y in [('shaker', shaker), ('tamb', tamb)]:
    open(f'/home/claude/perc_{name}.wav', 'wb').write(wav_bytes(y))
# phone samples.js: add to both kits
p = '/home/claude/ai/firmware/web/samples.js'; s = open(p).read()
kits = json.loads(s[s.index('{'):s.rindex('}') + 1])
for k in kits:
    kits[k]['shaker'] = base64.b64encode(wav_bytes(shaker)).decode()
    kits[k]['tamb'] = base64.b64encode(wav_bytes(tamb)).decode()
open(p, 'w').write('window.PB_KITS = ' + json.dumps(kits) + ';\n')
# board samples.h: 16 kHz int16 arrays appended
p = '/home/claude/ai/firmware/pocketbeat_sticks3/samples.h'; s = open(p).read()
if 'SMP_shaker' not in s:
    add = ['', '// Percussion for Shaker mode']
    for name, y in [('shaker', shaker), ('tamb', tamb)]:
        z = signal.resample_poly(y, 1, 2); z = np.clip(z / np.max(np.abs(z)) * 0.98, -1, 1)
        arr = (z * 32767).astype(np.int16)
        add.append(f'static const int16_t SMP_{name}[{len(arr)}] = {{')
        add.append(',\n'.join(','.join(str(v) for v in arr[i:i+24]) for i in range(0, len(arr), 24)))
        add.append('};')
    open(p, 'w').write(s + '\n'.join(add) + '\n')
print('shaker', len(shaker)/SR, 's  tamb', len(tamb)/SR, 's')
