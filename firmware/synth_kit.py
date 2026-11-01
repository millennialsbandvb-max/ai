"""Render drum samples for PocketBeat. Two kits: 'acoustic' (modelled shells) and 'electro' (808-style).
Outputs 32 kHz 16-bit mono WAVs and a samples.js with base64 data."""
import numpy as np, base64, io, wave, json, sys
from scipy import signal

SR = 32000
rng = np.random.default_rng(7)

def t(sec): return np.arange(int(SR * sec)) / SR
def env(sec, a=0.001, d=0.2, curve=4.0):
    x = t(sec); e = np.exp(-x / d * curve)
    att = np.minimum(1, x / max(a, 1e-5)); return e * att
def hp(x, f, order=2): b, a = signal.butter(order, f / (SR / 2), 'high'); return signal.lfilter(b, a, x)
def lp(x, f, order=2): b, a = signal.butter(order, f / (SR / 2), 'low'); return signal.lfilter(b, a, x)
def bp(x, lo, hi, order=2): b, a = signal.butter(order, [lo / (SR / 2), hi / (SR / 2)], 'band'); return signal.lfilter(b, a, x)
def sat(x, k=2.0): return np.tanh(x * k) / np.tanh(k)
def norm(x, peak=0.95):
    x = x - np.mean(x[:8]) * 0  # keep DC-free anyway
    m = np.max(np.abs(x)); return x / m * peak if m > 0 else x
def fade_out(x, ms=8):
    n = int(SR * ms / 1000); x[-n:] *= np.linspace(1, 0, n); return x
def mode(sec, f0, f1, decay, amp=1.0, phase=0.0):
    """Sine whose pitch glides f0->f1 with exponential decay."""
    x = t(sec); f = f1 + (f0 - f1) * np.exp(-x / 0.045)
    ph = 2 * np.pi * np.cumsum(f) / SR + phase
    return amp * np.sin(ph) * np.exp(-x / decay)

# ---------- Acoustic kit ----------
def kick_acoustic():
    sec = 0.7
    body = mode(sec, 165, 52, 0.26, 1.0)                 # head pitch drop
    sub  = mode(sec, 90, 52, 0.40, 0.45)                 # shell fundamental (same pitch as body tail, no beating)
    knock = mode(sec, 400, 180, 0.03, 0.35)              # beater knock
    click = hp(rng.standard_normal(len(t(sec))), 2500) * env(sec, 0.0005, 0.006, 3) * 0.6
    x = body + sub + knock + click
    x = sat(x, 1.6)
    x = lp(x, 6000, 1)
    return norm(fade_out(x))

def snare_acoustic():
    sec = 0.42
    x_t = t(sec)
    shell = mode(sec, 235, 190, 0.11, 0.9) + mode(sec, 330, 300, 0.07, 0.45) + mode(sec, 180, 170, 0.14, 0.35)
    n = rng.standard_normal(len(x_t))
    wires = bp(n, 1500, 9000, 2) * (np.exp(-x_t / 0.075) * 0.8 + np.exp(-x_t / 0.025) * 0.9)
    crack = hp(n, 4000) * env(sec, 0.0003, 0.012, 3) * 1.1
    x = shell * 1.0 + wires * 0.5 + crack * 0.45
    x = sat(x, 1.8)
    return norm(fade_out(x))

def hat_metal(sec=0.14, decay=0.045, open_=False):
    x_t = t(sec)
    ratios = [1.0, 1.4471, 1.6170, 1.9265, 2.5028, 2.6637]   # classic inharmonic set
    f0 = 320.0
    x = np.zeros_like(x_t)
    for r in ratios:
        x += signal.square(2 * np.pi * f0 * r * x_t) * 0.18
    x = bp(x, 6500, 13500, 2)
    x += hp(rng.standard_normal(len(x_t)), 7000) * 0.25
    e = np.exp(-x_t / (0.30 if open_ else decay)) * (1 + 1.5 * np.exp(-x_t / 0.004))
    x = x * e
    x = hp(x, 5000, 2)
    return norm(fade_out(x), 0.8)

# ---------- Electro (808-ish) kit ----------
def kick_electro():
    sec = 0.9
    x = mode(sec, 160, 46, 0.45, 1.0)
    x += hp(rng.standard_normal(len(t(sec))), 3000) * env(sec, 0.0003, 0.004, 3) * 0.3
    x = sat(x, 2.2)
    return norm(fade_out(x))

def snare_electro():
    sec = 0.35; x_t = t(sec)
    tone = mode(sec, 200, 180, 0.08, 0.7) + mode(sec, 330, 320, 0.06, 0.4)
    n = rng.standard_normal(len(x_t))
    noise = hp(n, 1800) * np.exp(-x_t / 0.065) * 0.8
    x = tone + noise
    x = sat(x, 1.5)
    return norm(fade_out(x))

def hat_electro():
    return hat_metal(0.1, 0.03)

def wav_bytes(x):
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype('<i2').tobytes())
    return buf.getvalue()

kits = {
    'acoustic': {'kick': kick_acoustic(), 'snare': snare_acoustic(), 'hat': hat_metal(), 'hatOpen': hat_metal(0.45, 0.2, True)},
    'electro':  {'kick': kick_electro(),  'snare': snare_electro(),  'hat': hat_electro(), 'hatOpen': hat_metal(0.4, 0.18, True)},
}
out = {}
total = 0
for k, v in kits.items():
    out[k] = {}
    for name, x in v.items():
        b = wav_bytes(x); total += len(b)
        out[k][name] = base64.b64encode(b).decode()
        with open(f'/home/claude/kit_{k}_{name}.wav', 'wb') as f: f.write(b)
with open('/home/claude/samples.js', 'w') as f:
    f.write('window.PB_KITS = ' + json.dumps(out) + ';\n')
print('total wav bytes', total, 'js bytes', len(open('/home/claude/samples.js').read()))
