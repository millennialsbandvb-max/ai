"""Measure each H90 program in the play-through (numpy + scipy): analyze_video.py <wav> <info.json> <out.json> [names]
Per chapter: echo lags (whitened waveform autocorrelation; echoes are exact copies, playing rhythm is not),
L/R cross echoes (ping-pong), decay rate after stops, AM (tremolo) and FM (chorus/vibrato) rate+depth,
interval histogram of spectral peaks (pitch shifting), sub-octave energy, drive (crest, flatness), centroid, width."""
import json
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import find_peaks, stft

sr, x = wavfile.read(sys.argv[1])
x = x.astype(np.float32) / 32768.0
info = json.load(open(sys.argv[2]))
ch = info["chapters"]
out = []


def whiten_ac(sig, n=1 << 16, hop=1 << 15, maxlag=2.2):
    """median over frames of the spectrally whitened autocorrelation"""
    L = int(maxlag * sr)
    acs = []
    w = np.hanning(n)
    for s in range(0, len(sig) - n, hop):
        f = sig[s:s + n] * w
        if np.sqrt(np.mean(f * f)) < 0.003:
            continue
        X = np.fft.rfft(f, 2 * n)
        P = np.abs(X) ** 2
        env = np.convolve(P, np.ones(64) / 64, mode="same") + 1e-12  # smooth spectral envelope
        r = np.fft.irfft(P / env)[:L]
        r /= r[0] + 1e-12
        acs.append(r)
    if not acs:
        return None
    return np.median(np.array(acs), axis=0)


def xcorr_lr(l, r, n=1 << 16, hop=1 << 15, maxlag=2.2):
    L = int(maxlag * sr)
    acs = []
    w = np.hanning(n)
    for s in range(0, len(l) - n, hop):
        a, b = l[s:s + n] * w, r[s:s + n] * w
        if np.sqrt(np.mean(a * a + b * b)) < 0.003:
            continue
        A, B = np.fft.rfft(a, 2 * n), np.fft.rfft(b, 2 * n)
        C = A * np.conj(B)
        env = np.convolve(np.abs(C), np.ones(64) / 64, mode="same") + 1e-12
        c = np.fft.irfft(C / env)
        acs.append(np.concatenate([c[-L:], c[:L]]))  # lags -L..L-1 (positive: L later than R? see below)
    if not acs:
        return None
    return np.median(np.array(acs), axis=0)


def echoes(ac, lo=0.045):
    a = ac.copy()
    a[: int(lo * sr)] = 0
    a = np.abs(a)
    base = np.median(a[int(lo * sr):]) + 1e-9
    pk, pr = find_peaks(a, height=base * 6, distance=int(0.02 * sr))
    pk = pk[np.argsort(-a[pk])][:6]
    return [(round(p / sr * 1000, 1), round(float(a[p]), 4), round(float(a[p] / base), 1)) for p in sorted(pk)]


only = set(sys.argv[4].split(",")) if len(sys.argv) > 4 else None
for i, c in enumerate(ch):
    if c["title"] == "Intro" or (only and c["title"] not in only):
        continue
    s, e = int(c["start_time"] * sr), int(c["end_time"] * sr)
    seg = x[s:e]
    L, R = seg[:, 0], seg[:, 1]
    M, S = (L + R) / 2, (L - R) / 2
    d = {"n": i, "name": c["title"], "t": c["start_time"], "len": round(c["end_time"] - c["start_time"])}
    # stereo
    d["width"] = round(float(np.sqrt(np.mean(S * S) / (np.mean(M * M) + 1e-12))), 3)
    d["lr_corr"] = round(float(np.corrcoef(L, R)[0, 1]), 3)
    # echoes
    ac = whiten_ac(M)
    d["echo"] = echoes(ac) if ac is not None else []
    xc = xcorr_lr(L, R)
    if xc is not None:
        Lg = len(xc) // 2
        a = np.abs(xc)
        a[Lg - int(0.03 * sr):Lg + int(0.03 * sr)] = 0
        base = np.median(a) + 1e-9
        pk, _ = find_peaks(a, height=base * 8, distance=int(0.02 * sr))
        pk = pk[np.argsort(-a[pk])][:4]
        d["lr_echo"] = [(round((p - Lg) / sr * 1000, 1), round(float(a[p] / base), 1)) for p in sorted(pk)]
    # spectra
    f, t, Z = stft(M, sr, nperseg=4096, noverlap=3072)
    P = np.abs(Z) ** 2
    fr = P.sum(0)
    act = fr > np.percentile(fr, 40)
    Pa = P[:, act]
    spec = Pa.mean(1)
    d["centroid"] = round(float((f * spec).sum() / spec.sum()), 1)
    cum = np.cumsum(spec) / spec.sum()
    d["rolloff85"] = round(float(f[np.searchsorted(cum, 0.85)]), 1)
    d["sub70"] = round(float(10 * np.log10(spec[(f > 30) & (f < 70)].sum() / spec.sum() + 1e-12)), 1)
    d["hi5k"] = round(float(10 * np.log10(spec[f > 5000].sum() / spec.sum() + 1e-12)), 1)
    flat = np.exp(np.mean(np.log(Pa[(f > 200) & (f < 6000)] + 1e-12), 0)) / (np.mean(Pa[(f > 200) & (f < 6000)], 0) + 1e-12)
    d["flatness"] = round(float(np.median(flat)), 4)
    # crest factor over 50 ms windows (drive squashes it)
    w = int(0.05 * sr)
    seg2 = M[: len(M) // w * w].reshape(-1, w)
    rms = np.sqrt((seg2 ** 2).mean(1)) + 1e-9
    pkv = np.abs(seg2).max(1)
    loud = rms > np.percentile(rms, 60)
    d["crest"] = round(float(np.median(pkv[loud] / rms[loud])), 2)
    # envelope (10 ms) -> AM rate and decay
    w = int(0.01 * sr)
    env = np.sqrt((M[: len(M) // w * w].reshape(-1, w) ** 2).mean(1)) + 1e-7
    edb = 20 * np.log10(env)
    # AM: spectrum of the envelope over 4 s windows, peak 1..15 Hz relative to neighbours
    am = []
    for s2 in range(0, len(env) - 400, 200):
        e2 = env[s2:s2 + 400]
        if e2.mean() < 0.004:
            continue
        e2 = e2 / e2.mean() - 1
        E = np.abs(np.fft.rfft(e2 * np.hanning(400), 4096))
        fe = np.fft.rfftfreq(4096, 0.01)
        band = (fe > 0.8) & (fe < 15)
        k = np.argmax(E * band)
        am.append((fe[k], E[k] / (np.median(E[band]) + 1e-9), np.std(e2)))
    if am:
        am = np.array(am)
        good = am[:, 1] > 6
        d["am_rate"] = round(float(np.median(am[good, 0])), 2) if good.sum() > 2 else None
        d["am_strength"] = round(float(np.median(am[:, 1])), 1)
        d["am_depth"] = round(float(np.median(am[:, 2])), 3)
    # decays: after a level max, fall >= 18 dB within 6 s; slope from -3 to -18 dB below that peak
    rates = []
    sm = np.convolve(edb, np.ones(5) / 5, mode="same")
    k = 0
    while k < len(sm) - 600:
        if sm[k] > -40 and sm[k] >= sm[max(0, k - 30):k + 30].max():
            seg3 = sm[k:k + 600]
            j3 = np.argmax(seg3 < sm[k] - 3)
            j18 = np.argmax(seg3 < sm[k] - 18)
            if j3 and j18 > j3 and not (np.diff(seg3[j3:j18]) > 1.5).any():
                rates.append(15.0 / ((j18 - j3) * 0.01))  # dB/s
                k += j18
                continue
        k += 5
    if rates:
        rt = 60.0 / np.array(rates)
        d["rt60_med"] = round(float(np.median(rt)), 2)
        d["rt60_p80"] = round(float(np.percentile(rt, 80)), 2)
        d["ndecays"] = len(rates)
    # pitch intervals between strong spectral peaks (in semitones, 1..36), weighted
    hist = np.zeros(48)
    fine = np.zeros(200)  # detune around unison partner, cents 0..199
    for j in np.nonzero(act)[0][::4]:
        col = P[:, j]
        pk, _ = find_peaks(col, height=col.max() * 0.05, distance=3)
        pk = pk[(f[pk] > 70) & (f[pk] < 3000)]
        if len(pk) < 2:
            continue
        pk = pk[np.argsort(-col[pk])][:8]
        # parabolic refine
        fp = []
        for q in pk:
            a, b, cc = np.log(col[q - 1] + 1e-12), np.log(col[q] + 1e-12), np.log(col[q + 1] + 1e-12)
            off = 0.5 * (a - cc) / (a - 2 * b + cc + 1e-12)
            fp.append(f[q] + off * (f[1] - f[0]))
        fp = np.array(fp)
        wts = np.sqrt(col[pk] / col[pk].max())
        for u in range(len(fp)):
            for v in range(len(fp)):
                if fp[v] > fp[u]:
                    st = 12 * np.log2(fp[v] / fp[u])
                    if st < 47.5:
                        hist[int(round(st))] += wts[u] * wts[v]
    hist /= hist.sum() + 1e-12
    d["intervals"] = [round(float(h), 3) for h in hist]
    out.append(d)
    print(d["n"], d["name"], d["echo"][:3], d.get("lr_echo"), d.get("rt60_med"), d.get("am_rate"), d["crest"],
          d["centroid"], d["width"], flush=True)

json.dump(out, open(sys.argv[3], "w"), indent=1)
