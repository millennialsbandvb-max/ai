/* Enz0 X: a guitar synth for MPC OS (VST2, stereo), in the spirit of the tracking synth pedals (our own code and
 * sound, on vocal-mpc's core: parameter table, YIN tracker, biquads, FDN reverb). Four modes:
 *   Mono  the tracked note plays an oscillator (saw -> square -> thin pulse, + a sub an octave down), with glide
 *   Poly  the whole input (chords too) is pitch-shifted by Pitch and squared up into a synth tone
 *   Arp   the tracked note steps through a pattern (up, up-down, octaves, fifths, random) at Arp Rate
 *   Dry   the input itself through the filter, modulation, delay and reverb
 * then a filter (low / band / high pass or a 24 dB ladder-ish low pass) moved by an envelope (following the playing
 * or triggered by each new note), modulation (vibrato, ring mod, tremolo or a filter LFO), a delay and a reverb.
 * The synth's loudness follows the playing; Sustain is how long it rings after; Hold freezes the note (MIDI CC 64).
 * MIDI (any channel / MIDI Ch): Program Change n = program n + 1; CC 64 = Hold. Programs: PROGS[].
 * Safety as everywhere here: input cleaned, reset if the output goes bad or past +36 dB, capped at +12 dB. */
#include "vocal_core.h"

static const char *const MODES[] = {"Mono", "Poly", "Arp", "Dry"};
static const char *const FTYPES[] = {"Low Pass", "Band Pass", "High Pass", "Ladder"};
static const char *const ETYPES[] = {"Follower", "Triggered"};
static const char *const MTYPES[] = {"Vibrato", "Ring Mod", "Tremolo", "Filter LFO"};
static const char *const ARPS[] = {"Up", "Up Down", "Octaves", "Fifths", "Random"};

enum { kProgram, kProgPrev, kProgNext, kMode, kPitch, kWave, kSub, kGlide, kFType, kCutoff, kReso, kEnvAmt, kEnvType,
       kEnvAtk, kEnvDec, kModType, kModDepth, kModRate, kSustain, kArp, kArpRate, kDlyTime, kDlyFb, kDlyLvl, kVerb,
       kMix, kLevel, kHold, kNumParams };
static const ParamDef P[kNumParams] = {
    PRESET("program", "Program"), BUTTON("program_prev", "Program <"), BUTTON("program_next", "Program >"),
    OPT("mode", "Mode", MODES, 0), INTP("pitch", "Pitch", -24, 24, 0, "st"), PCT("wave", "Wave", 30), PCT("sub", "Sub", 30),
    EXPP("glide", "Glide", 1, 1000, 15, "ms"),
    OPT("ftype", "Filter Type", FTYPES, 3), EXPP("cutoff", "Cutoff", 60, 12000, 900, "Hz"), PCT("reso", "Resonance", 40),
    LIN("env_amt", "Env Amount", -100, 100, 50, "%"), OPT("env_type", "Env Type", ETYPES, 0),
    EXPP("env_atk", "Env Attack", 1, 2000, 5, "ms"), EXPP("env_dec", "Env Decay", 20, 4000, 400, "ms"),
    OPT("mod_type", "Mod Type", MTYPES, 0), PCT("mod_depth", "Mod Depth", 0), EXPP("mod_rate", "Mod Rate", 0.1f, 20, 5, "Hz"),
    EXPP("sustain", "Sustain", 10, 4000, 150, "ms"),
    OPT("arp", "Arp Pattern", ARPS, 0), EXPP("arp_rate", "Arp Rate", 1, 20, 6, "Hz"),
    EXPP("dly_time", "Delay Time", 20, 1500, 380, "ms"), PCT("dly_fb", "Delay Feedback", 35), PCT("dly_lvl", "Delay Level", 0),
    PCT("reverb", "Reverb", 0), PCT("mix", "Mix", 100), LIN("level", "Level", -24, 12, 0, "dB"), TOGGLE("hold", "Hold", 0),
};

/* ---- programs: starting points of ours (values in the knobs' own units, from kMode on) ------------------------ */
typedef struct { const char *name; float v[kNumParams - kMode]; } Prog;
#define PV(mode, pitch, wave, sub, glide, ft, cut, reso, ea, et, eat, edc, mt, md, mr, sus, arp, ar, dt, dfb, dl, rv, mix, lvl) \
    {mode, pitch, wave, sub, glide, ft, cut, reso, ea, et, eat, edc, mt, md, mr, sus, arp, ar, dt, dfb, dl, rv, mix, lvl, 0}
static const Prog PROGS[] = {
    {"Fat Lead", PV(0, 0, 20, 30, 25, 3, 1400, 45, 40, 0, 5, 400, 0, 15, 5.5f, 200, 0, 6, 380, 30, 20, 15, 100, 0)},
    {"Square Bass", PV(0, -12, 55, 60, 10, 3, 500, 30, 60, 1, 2, 250, 0, 0, 5, 120, 0, 6, 380, 35, 0, 0, 100, 0)},
    {"Saw Bass Wah", PV(0, -12, 0, 40, 10, 0, 300, 70, 85, 0, 5, 400, 0, 0, 5, 100, 0, 6, 380, 35, 0, 0, 100, 0)},
    {"Poly Strings", PV(1, 0, 10, 0, 1, 0, 2500, 15, 20, 0, 400, 1500, 0, 20, 4.5f, 1200, 0, 6, 420, 30, 15, 45, 85, -2)},
    {"Poly Brass", PV(1, 0, 25, 0, 1, 3, 900, 25, 70, 1, 60, 500, 0, 10, 5, 300, 0, 6, 380, 25, 10, 25, 100, -2)},
    {"Octave Down Organ", PV(1, -12, 50, 0, 1, 0, 4000, 10, 0, 0, 5, 400, 2, 25, 6.5f, 200, 0, 6, 380, 30, 0, 20, 80, 0)},
    {"Arp Up", PV(2, 0, 50, 20, 1, 3, 1800, 50, 50, 1, 2, 180, 0, 0, 5, 150, 0, 8, 375, 40, 30, 20, 100, 0)},
    {"Arp Octaves", PV(2, 0, 20, 30, 1, 0, 1500, 60, 60, 1, 2, 160, 0, 0, 5, 130, 2, 6, 333, 45, 30, 25, 100, 0)},
    {"Fifths Sequence", PV(2, 0, 70, 0, 1, 3, 1200, 55, 70, 1, 2, 220, 3, 30, 0.3f, 180, 3, 5, 400, 40, 25, 30, 100, 0)},
    {"Random Bleeps", PV(2, 12, 80, 0, 1, 1, 2000, 70, 40, 1, 1, 120, 0, 0, 5, 100, 4, 9, 300, 50, 35, 30, 100, -2)},
    {"Sync-ish Lead", PV(0, 12, 85, 0, 40, 3, 2400, 75, 50, 1, 3, 600, 1, 35, 3, 250, 0, 6, 420, 35, 25, 20, 100, -2)},
    {"Ring Bells", PV(0, 0, 50, 0, 1, 0, 6000, 10, 0, 0, 5, 400, 1, 70, 3, 1500, 0, 6, 500, 45, 30, 40, 90, -3)},
    {"Envelope Filter", PV(3, 0, 0, 0, 1, 1, 400, 70, 90, 0, 3, 300, 0, 0, 5, 150, 0, 6, 380, 30, 0, 0, 100, 6)},
    {"Hold Pad", PV(1, 0, 30, 20, 1, 0, 1500, 20, 15, 0, 800, 2000, 0, 25, 4, 3000, 0, 6, 600, 50, 30, 60, 70, -3)},
    {"Lo-Fi Lead", PV(0, 0, 60, 10, 60, 1, 1300, 40, 30, 0, 5, 400, 0, 40, 6.5f, 250, 0, 6, 250, 55, 35, 20, 100, -1)},
    {"Space Synth", PV(1, 12, 20, 0, 1, 3, 3000, 35, 30, 0, 200, 1500, 0, 20, 4, 2000, 0, 6, 700, 60, 40, 70, 75, -4)},
};
#define NPROGS ((int)(sizeof PROGS / sizeof *PROGS))

/* ---- a two-head pitch shifter (Poly): heads half a window apart, Hann-crossfaded ------------------------------- */
typedef struct { Line l; float d; int win; } Shift;
static float shift_run(Shift *s, float x, float ratio) {
    line_push(&s->l, x);
    const float W = (float)s->win;
    s->d += 1 - ratio;   /* (the read point moves against the write point) */
    while (s->d < 0) s->d += W;
    while (s->d >= W) s->d -= W;
    float y = 0;
    for (int h = 0; h < 2; h++) {
        float d = s->d + (float)h * W * 0.5f;
        if (d >= W) d -= W;
        const float w = 0.5f - 0.5f * cosf(2 * PI * d / W);
        y += line_read(&s->l, d + 2) * w;
    }
    return y;
}

/* a TPT state-variable filter (stays stable while swept) */
typedef struct { float ic1, ic2; } Svf;
static void svf_run(Svf *f, float g, float k, float x, float *lo, float *bp, float *hi) {
    const float a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const float v3 = x - f->ic2, v1 = a1 * f->ic1 + a2 * v3, v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
    f->ic1 = 2 * v1 - f->ic1;
    f->ic2 = 2 * v2 - f->ic2;
    *lo = v2; *bp = v1; *hi = x - k * v1 - v2;
}
/* band-limited saw / pulse (polyBLEP) */
static float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}

/* ---- a fast pitch tracker for a guitar: YIN at half rate (24 kHz at 48), a fresh reading every 64 half-rate
 * samples (~2.7 ms), window 256 (~11 ms), 60 .. 1400 Hz; the first dip under the threshold (not the deepest: a strong
 * 2nd harmonic doesn't drag it an octave up), parabolic interpolation. (vocal_core's tracker, at quarter rate every
 * 11 ms with a median of three, landed notes in ~40 ms.) */
#define FT_N 2048
#define FT_W 256
typedef struct { float buf[FT_N], d[430]; int w, cnt, half, tmax; float acc, hz, conf; OnePole a1, a2; int fresh; } FastTrack;
static void ft_push(FastTrack *t, float x, float fs) {
    const float c = lp_coef(1400, fs);   /* (two poles at 1.4 kHz: the harmonics down, the fundamental kept) */
    const float y = lp(&t->a2, lp(&t->a1, x, c), c);
    t->acc += y;
    if (++t->half < 2) return;
    t->buf[t->w] = t->acc * 0.5f;
    t->w = (t->w + 1) & (FT_N - 1);
    t->acc = 0; t->half = 0;
    if (++t->cnt < 64) return;
    t->cnt = 0;
    const float fsd = fs * 0.5f;
    const int tmin = (int)(fsd / 1400), tmax = (int)(fsd / 60) < 420 ? (int)(fsd / 60) : 420;
    static float x0[FT_W + 430];
    float *d = t->d;
    t->tmax = tmax;
    for (int i = 0; i <= tmax; i++) d[i] = 1;
    for (int i = 0; i < FT_W + tmax + 1; i++) x0[i] = t->buf[(t->w - (FT_W + tmax + 1) + i) & (FT_N - 1)];
    /* the window is the newest FT_W samples (x0[tmax + 1 ..]), each compared with tau samples before it */
    const float *cur = x0 + tmax + 1;
    float energy = 0;
    for (int i = 0; i < FT_W; i++) energy += cur[i] * cur[i];
    t->fresh = 1;
    if (energy < 1e-6f) { t->conf = 0; return; }
    float run = 0;
    d[0] = 1;
    int best = -1, gmin = -1;
    for (int tau = 1; tau <= tmax; tau++) {
        float sum = 0;
        for (int i = 0; i < FT_W; i++) { const float e = cur[i] - cur[i - tau]; sum += e * e; }
        run += sum;
        d[tau] = sum * (float)tau / (run + 1e-12f);
        if (tau > tmin + 1) {
            if (gmin < 0 || d[tau - 1] < d[gmin]) gmin = tau - 1;
            if (best < 0 && d[tau - 1] < 0.15f && d[tau - 1] <= d[tau] && d[tau - 1] <= d[tau - 2]) best = tau - 1;   /* (and on: the whole curve is kept) */
        }
    }
    if (best < 0) best = gmin;
    if (best < 2 || best >= tmax || d[best] > 0.45f) { t->conf = 0; return; }
    t->conf = 1;   /* (set below) */
    const float a = d[best - 1], b = d[best], c2 = d[best + 1], den = a - 2 * b + c2;
    const float tau = (float)best + (fabsf(den) > 1e-9f ? 0.5f * (a - c2) / den : 0);
    t->hz = fsd / tau;
    t->conf = 1 - b;
}

/* how clearly a note (Hz) is in the last reading: the lowest of the curve within 3% of its period (0 = clear, 1 = not) */
static float ft_has(const FastTrack *t, float hz, float fs) {
    const float tau = fs * 0.5f / hz;
    const int lo = (int)(tau * 0.97f), hi = (int)(tau * 1.03f) + 1;
    float m = 1;
    for (int i = lo < 1 ? 1 : lo; i <= hi && i < t->tmax; i++) if (t->d[i] < m) m = t->d[i];
    return m;
}

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    int ready, program;
    volatile int update_display;
    volatile unsigned char automate[kNumParams];
    FastTrack trk;
    float env_slow, env_fast, cand;             /* onsets; a big jump waits for a second reading that agrees */
    int cand_n, note_ok, note_ok_pending_guard;
    float since_onset;
    Shift sh;
    Line dly[2];
    Verb verb;
    OnePole dcl, sq_lp, env_in, dmp[2];
    Svf f1, f2;
    float ph, ph_sub, ph_ring, lfo;   /* oscillator, sub, ring carrier, LFO phases (0..1) */
    float note;                       /* the note the oscillator plays (MIDI number, glides) */
    float tracked;                    /* the last confident tracked note */
    float env, amp, fenv, fstage;     /* input envelope, synth loudness, filter envelope (+ stage: 1 attack, 0 decay) */
    float gate_prev, arp_ph;
    int arp_step;
    uint32_t rng;
    float mix_s, out_s;
} Plugin;

static float val(const Plugin *s, int i) { return pv(&P[i], s->norm[i]); }

static void reset(Plugin *s) {
    line_clear(&s->sh.l); line_clear(&s->dly[0]); line_clear(&s->dly[1]); verb_clear(&s->verb);
    memset(&s->f1, 0, sizeof s->f1); memset(&s->f2, 0, sizeof s->f2);
    s->dcl.z = s->sq_lp.z = s->env_in.z = s->dmp[0].z = s->dmp[1].z = 0;
    s->env = s->amp = s->fenv = 0;
}
static int setup(Plugin *s, float fs) {
    s->ready = 0;
    s->fs = fs;
    memset(&s->trk, 0, sizeof s->trk);
    s->env_slow = s->env_fast = 0; s->note_ok = 0; s->cand_n = 0;
    s->sh.win = (int)(fs * 0.04f);
    int ok = line_init(&s->sh.l, s->sh.win * 2 + 8) && line_init(&s->dly[0], (int)(fs * 1.6f)) && line_init(&s->dly[1], (int)(fs * 1.6f)) &&
             verb_init(&s->verb, fs);
    if (!ok) return 0;
    verb_set(&s->verb, 1.4f, 3.0f, 6000, fs);
    reset(s);
    s->ready = 1;
    return 1;
}

static float rnd01(Plugin *s) { s->rng = s->rng * 1664525u + 1013904223u; return (float)(s->rng >> 8) / 16777216.0f; }
static float arp_interval(Plugin *s, int pat) {   /* semitones above the tracked note for this step */
    static const float UP[4] = {0, 7, 12, 19}, UPDN[6] = {0, 4, 7, 12, 7, 4}, OCT[2] = {0, 12}, FIF[4] = {0, 7, 12, 7};
    const int k = s->arp_step;
    switch (pat) {
    case 1: return UPDN[k % 6];
    case 2: return OCT[k % 2];
    case 3: return FIF[k % 4];
    case 4: { static const float R[5] = {0, 7, 12, 3, 19}; return R[(int)(rnd01(s) * 4.99f)]; }
    default: return UP[k % 4];
    }
}

static void process(AEffect *e, float **in, float **out, int32_t n) {
    VC_FLUSH_DENORMALS_BEGIN;
    Plugin *s = (Plugin *)e->object;
    if (s->master) {
        for (int i = 0; i < kNumParams; i++)
            if (s->automate[i]) { s->automate[i] = 0; s->master(e, audioMasterAutomate, i, 0, NULL, s->norm[i]); s->update_display = 1; }
        if (s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    }
    const float *il = in && in[0] ? in[0] : NULL, *ir = in && in[1] ? in[1] : il;
    if (!s->ready) { for (int k = 0; k < n; k++) out[0][k] = out[1][k] = 0; goto done; }
    {
    const float fs = s->fs;
    const int mode = (int)val(s, kMode), ftype = (int)val(s, kFType), etype = (int)val(s, kEnvType), mtype = (int)val(s, kModType);
    const int hold = s->norm[kHold] > 0.5f, pat = (int)val(s, kArp);
    const float pitch = val(s, kPitch), wave = val(s, kWave) / 100, sub = val(s, kSub) / 100;
    const float glide = 1 - expf(-1 / (val(s, kGlide) * 0.001f * fs));
    const float cut = val(s, kCutoff), reso = val(s, kReso) / 100, eamt = val(s, kEnvAmt) / 100;
    const float eatk = 1 - expf(-1 / (val(s, kEnvAtk) * 0.001f * fs)), edec = 1 - expf(-1 / (val(s, kEnvDec) * 0.001f * fs));
    const float mdep = val(s, kModDepth) / 100, mrate = val(s, kModRate) / fs;
    const float rel = 1 - expf(-1 / (val(s, kSustain) * 0.001f * fs)), att = 1 - expf(-1 / (0.003f * fs));
    const float arate = val(s, kArpRate) / fs;
    const int dsamp = (int)(val(s, kDlyTime) * 0.001f * fs);
    const float dfb = val(s, kDlyFb) / 100 * 0.9f, dlv = val(s, kDlyLvl) / 100, vlv = val(s, kVerb) / 100;
    const float mix = val(s, kMix) / 100, og = db(val(s, kLevel));
    const float k_res = 2 - 1.9f * reso;   /* (SVF damping: 2 = none, 0.1 = ringing) */
    for (int k = 0; k < n; k++) {
        float a = il ? il[k] : 0, b = ir ? ir[k] : a;
        if (!finite_bits(a) || a > 8 || a < -8) a = 0;
        if (!finite_bits(b) || b > 8 || b < -8) b = 0;
        const float x = hp(&s->dcl, 0.5f * (a + b), lp_coef(30, fs));
        /* the playing: its envelope, its note, and new notes */
        const float lvl = fabsf(x);
        s->env += (lvl > s->env ? 0.01f : 0.0005f) * (lvl - s->env);
        /* a new pick: a fast envelope jumping well over a slow one (~100 ms); re-armed once the burst has passed */
        s->env_fast += (lvl > s->env_fast ? 0.05f : 0.002f) * (lvl - s->env_fast);
        s->env_slow += 0.0002f * (s->env_fast - s->env_slow);
        const int gate = s->env > 0.004f;
        const int onset = gate && s->env_fast > s->env_slow * 2 + 0.002f && !s->note_ok_pending_guard;
        int trig = gate && s->gate_prev < 0.5f;
#ifdef ENZO_DEBUG
        static long dbg_t; dbg_t++;
        if (onset) fprintf(stderr, "%7.3f onset (note_ok %d) fast %.3f slow %.3f\n", (double)dbg_t / fs, s->note_ok, s->env_fast, s->env_slow);
#endif
        if (onset) { s->note_ok = 0; s->cand_n = 0; s->since_onset = 0; trig = 1; }   /* a new pick: wait for its note */
        s->since_onset += 1;
        if (!hold) s->gate_prev = (float)gate;
        ft_push(&s->trk, x, fs);
        if (s->trk.fresh && !hold) {
            s->trk.fresh = 0;
#ifdef ENZO_DEBUG
            fprintf(stderr, "%7.3f read %.2f conf %.2f tracked %.2f ok %d gate %d\n", (double)dbg_t / fs, midi_of(s->trk.hz), s->trk.conf, s->tracked, s->note_ok, gate);
#endif
            if (gate && s->trk.conf > 0.82f) {
                const float r = midi_of(s->trk.hz);
                if (!s->note_ok) {   /* a new pick: two readings that agree (not the first 3 ms: the old string's) */
                    if (s->since_onset < 0.003f * fs) {}
                    else if (s->cand_n && fabsf(r - s->cand) < 0.5f) { s->tracked = r; s->note_ok = 1; s->note = r + pitch; s->cand_n = 0; }
                    else { s->cand = r; s->cand_n = 1; }
                }
                else if (fabsf(r - s->tracked) < 0.8f) { s->tracked = r; s->cand_n = 0; }               /* bends, vibrato */
                else if (ft_has(&s->trk, 440 * powf(2, (s->tracked - 69) / 12), fs) < 0.2f) s->cand_n = 0;   /* (still clearly playing: stay) */
                else if ((fabsf(s->tracked - r - 12) < 0.6f || fabsf(s->tracked - r - 19) < 0.6f || fabsf(s->tracked - r - 24) < 0.6f) &&
                         ft_has(&s->trk, 440 * powf(2, (s->tracked - 69) / 12), fs) < 0.45f) s->cand_n = 0;
                    /* (an octave, a 12th or two octaves below, while the note is still there: two strings ringing make a
                     * common lower period; not a new note) */
                else {   /* a jump with no new pick (a hammer-on, a slide): readings that agree; big or octave jumps: more */
                    if (s->cand_n && fabsf(r - s->cand) < 0.5f) s->cand_n++; else { s->cand = r; s->cand_n = 1; }
                    const float j = fabsf(r - s->tracked);
                    const int need = fabsf(j - 12) < 0.6f || fabsf(j - 24) < 0.6f ? 6 : j > 7 ? 4 : 2;
                    if (s->cand_n >= need) { s->tracked = r; s->cand_n = 0; }
                }
            }
            if (!gate) { s->note_ok = 0; s->cand_n = 0; }
        }
        if (s->env_fast < s->env_slow * 1.3f) s->note_ok_pending_guard = 0;
        if (onset) s->note_ok_pending_guard = 1;
        /* the synth's loudness: follows the playing (Sustain: how long it rings); silent until a new pick's note is
         * known (a few ms) rather than the old note sounding; Hold keeps it */
        if (!hold) {
            const float want = gate && (s->note_ok || mode == 3 || mode == 1) ? fminf(s->env * 4, 1) : 0;
            s->amp += (want > s->amp ? att : rel) * (want - s->amp);
        }
        /* the note: Mono / Arp from the tracker (+ Pitch, + the arp step), gliding */
        float target = s->tracked + pitch;
        if (mode == 2) {
            s->arp_ph += arate;
            if (s->arp_ph >= 1) { s->arp_ph -= 1; s->arp_step++; trig = gate || hold; }
            target += arp_interval(s, pat);
        }
        if (trig && mode != 2) s->arp_step = 0;
        if (!hold || mode == 2) s->note += (mode == 2 ? 0.5f : glide) * (target - s->note);
        /* modulation */
        s->lfo += mrate; if (s->lfo >= 1) s->lfo -= 1;
        const float lfo = sinf(2 * PI * s->lfo);
        float note = s->note + (mtype == 0 ? lfo * mdep * 0.6f : 0);
        const float hz = clampf(440 * powf(2, (note - 69) / 12), 20, fs * 0.4f), dt = hz / fs;
        /* the source */
        float y;
        if (mode == 0 || mode == 2) {
            s->ph += dt; if (s->ph >= 1) s->ph -= 1;
            s->ph_sub += dt * 0.5f; if (s->ph_sub >= 1) s->ph_sub -= 1;
            const float saw = 2 * s->ph - 1 - blep(s->ph, dt);
            const float pw = wave < 0.5f ? 0.5f : 0.5f - (wave - 0.5f) * 0.8f;   /* (past half: a thinner pulse) */
            float pulse = s->ph < pw ? 1 : -1;
            pulse += blep(s->ph, dt);
            float t2 = s->ph - pw; if (t2 < 0) t2 += 1;
            pulse -= blep(t2, dt);
            const float m = clampf(wave * 2, 0, 1);
            const float sq_sub = (s->ph_sub < 0.5f ? 1 : -1) + blep(s->ph_sub, dt * 0.5f) - blep(s->ph_sub < 0.5f ? s->ph_sub + 0.5f : s->ph_sub - 0.5f, dt * 0.5f);
            y = (saw * (1 - m) + pulse * m + sq_sub * sub * 0.7f) * 0.35f * s->amp;
        } else if (mode == 1) {   /* poly: shift, then square it up (level-independent), then smooth the edges */
            const float shifted = fabsf(pitch) > 0.01f ? shift_run(&s->sh, x, powf(2, pitch / 12)) : x;
            const float norm = 1 / (s->env * 3 + 1e-3f);
            const float hard = tanh_fast(shifted * norm * (1 + wave * 8));
            y = lp(&s->sq_lp, hard, lp_coef(9000, fs)) * 0.35f * s->amp * (1 + sub * 0.3f);
        } else y = x;
        if (mtype == 1 && mdep > 0) {   /* ring: a carrier a ratio above the note */
            s->ph_ring += hz * (1 + mdep * 3.2f) / fs; if (s->ph_ring >= 1) s->ph_ring -= 1;
            y = y * (1 - mdep) + y * sinf(2 * PI * s->ph_ring) * mdep * 1.4f;
        }
        /* the filter envelope */
        if (etype == 1) {
            if (trig) s->fstage = 1;
            if (s->fstage > 0.5f) { s->fenv += eatk * (1.05f - s->fenv); if (s->fenv >= 1) { s->fenv = 1; s->fstage = 0; } }
            else s->fenv += edec * (0 - s->fenv);
        } else {
            const float want = fminf(s->env * 6, 1);
            s->fenv += (want > s->fenv ? eatk : edec) * (want - s->fenv);
        }
        float fc = cut * powf(2, eamt * s->fenv * 5 + (mtype == 3 ? lfo * mdep * 2 : 0));
        fc = clampf(fc, 30, fs * 0.42f);
        const float g = tanf(PI * fc / fs);
        float lo, bp, hi, lo2, bp2, hi2;
        svf_run(&s->f1, g, ftype == 3 ? 1.4f : k_res, y, &lo, &bp, &hi);
        if (ftype == 3) { svf_run(&s->f2, g, k_res * 0.5f + 0.05f, tanh_fast(lo * 1.2f) / 1.2f, &lo2, &bp2, &hi2); y = lo2; }
        else y = ftype == 0 ? lo : ftype == 1 ? bp * k_res : hi;
        if (mtype == 2) y *= 1 - mdep * 0.5f * (1 - lfo);
        /* mix with the dry, then delay and reverb */
        s->mix_s += 0.002f * (mix - s->mix_s);
        float l = mode == 3 ? y : x * (1 - s->mix_s) + y * s->mix_s, r = l;
        if (dlv > 0.001f) {
            const float dl = line_tap(&s->dly[0], dsamp), dr = line_tap(&s->dly[1], dsamp + (int)(0.013f * fs));
            line_push(&s->dly[0], l + tanh_fast(lp(&s->dmp[0], dr, 0.3f) * dfb));   /* (ping-pong) */
            line_push(&s->dly[1], tanh_fast(lp(&s->dmp[1], dl, 0.3f) * dfb));
            l += dl * dlv; r += dr * dlv;
        }
        if (vlv > 0.001f) { float vl, vr; verb_run(&s->verb, l * 0.3f, r * 0.3f, &vl, &vr); l += vl * vlv; r += vr * vlv; }
        s->out_s += 0.002f * (og - s->out_s);
        l *= s->out_s; r *= s->out_s;
        if (!finite_bits(l) || !finite_bits(r) || fabsf(l) > 63 || fabsf(r) > 63) { reset(s); l = r = 0; }
        out[0][k] = clampf(l, -4, 4);
        out[1][k] = clampf(r, -4, 4);
    }
    }
done:
    VC_FLUSH_DENORMALS_END;
}

static void load_program(Plugin *s, int idx) {
    if (idx < 0 || idx >= NPROGS) return;
    for (int i = kMode; i < kNumParams; i++) { s->norm[i] = pn(&P[i], PROGS[idx].v[i - kMode]); s->automate[i] = 1; }
    s->program = idx;
    s->norm[kProgram] = (float)idx / (float)(NPROGS - 1);
    s->automate[kProgram] = 1;
    s->update_display = 1;
}
static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams) return;
    v = clampf(v, 0, 1);
    if (i == kProgPrev || i == kProgNext) {
        if (v > 0.5f) load_program(s, s->program < 0 ? (i == kProgPrev ? NPROGS - 1 : 0) : (s->program + (i == kProgPrev ? -1 : 1) + NPROGS) % NPROGS);
        s->norm[i] = 0;
        return;
    }
    if (i == kProgram) {   /* exact: a project restoring (the name only); a Q-Link nudge loads the next / previous */
        const float pos = v * (float)(NPROGS - 1), cur = s->program >= 0 ? (float)s->program : s->norm[i] * (float)(NPROGS - 1);
        if (fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1) { load_program(s, ((int)lrintf(cur) + (pos > cur ? 1 : -1) + NPROGS) % NPROGS); return; }
        s->program = (int)lrintf(pos);
        s->norm[i] = v;
        s->update_display = 1;
        return;
    }
    s->norm[i] = snap(&P[i], v, s->norm[i]);
    if (P[i].kind == K_OPT || P[i].kind == K_TOGGLE || P[i].kind == K_INT) s->update_display = 1;
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }
static void display(const Plugin *s, int i, char *t) {
    if (i == kProgram) { snprintf(t, 24, "%s", s->program >= 0 && s->program < NPROGS ? PROGS[s->program].name : "-"); return; }
    if (i == kProgPrev || i == kProgNext) { snprintf(t, 24, "%s", i == kProgPrev ? "<" : ">"); return; }
    if (i == kHold) { snprintf(t, 24, "%s", s->norm[i] > 0.5f ? "Held" : "Off"); return; }
    pdisplay(&P[i], s->norm[i], t);
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        line_free(&s->sh.l); line_free(&s->dly[0]); line_free(&s->dly[1]); verb_free(&s->verb);
        free(s);
        return 1;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) copy_str(p, P[idx].name, 32); return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay: if (idx >= 0 && idx < kNumParams) { char t[32] = {0}; display(s, idx, t); copy_str(p, t, 24); } return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "Enz0 X", 32); return 1;
    case effGetVendorString: copy_str(p, VC_VENDOR, 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate: if (opt > 1000 && fabsf(opt - s->fs) > 0.5f) setup(s, opt); return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (v) reset(s); return 1;
    case effProcessEvents: {
        const VstEvents *ev = (const VstEvents *)p;
        for (int k = 0; ev && k < ev->numEvents; k++) {
            if (!ev->events[k] || ev->events[k]->type != 1) continue;
            const unsigned char *m = ev->events[k]->midiData;
            if ((m[0] & 0xf0) == 0xc0) load_program(s, m[1]);
            else if ((m[0] & 0xf0) == 0xb0 && m[1] == 64) { s->norm[kHold] = m[2] >= 64 ? 1.0f : 0.0f; s->automate[kHold] = 1; }
        }
        return 1;
    }
    case effCanDo: return p && (!strcmp((const char *)p, "receiveVstEvents") || !strcmp((const char *)p, "receiveVstMidiEvent")) ? 1 : -1;
    default: return 0;
    }
}

static int enzo_pick(AEffect *e, int list, int op, int i, char *out, int max) {
    Plugin *s = (Plugin *)e->object;
    (void)list;
    if (op == PL_COUNT) return NPROGS;
    if (op == PL_CURRENT) return s->program;
    if (op == PL_HEADING || i < 0 || i >= NPROGS) return 0;
    if (op == PL_NAME) snprintf(out, (size_t)max, "%s", PROGS[i].name);
    else if (op == PL_CHOOSE) load_program(s, i);
    return 0;
}
static const char *const LIST_KEYS[] = {"program"};
static const char *const LIST_TITLES[] = {"Programs"};
static const PickLists LISTS = {1, LIST_KEYS, LIST_TITLES, enzo_pick, 1};   /* (1: MIDI Ch) */

static AEffect *make(audioMasterCallback master) {
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    s->rng = 777u;
    s->program = -1;
    s->note = s->tracked = 57;
    for (int i = 0; i < kNumParams; i++) s->norm[i] = pn(&P[i], P[i].def);
    if (!setup(s, 44100)) { free(s); return NULL; }
    load_program(s, 0);
    memset((void *)s->automate, 0, sizeof s->automate);
    s->out_s = db(val(s, kLevel));
    s->mix_s = val(s, kMix) / 100;
    AEffect *e = &s->fx;
    e->magic = VST_MAGIC;
    e->dispatcher = dispatcher;
    e->process = process;
    e->processReplacing = process;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->numPrograms = 1;
    e->numParams = kNumParams;
    e->numInputs = e->numOutputs = 2;
    e->flags = effFlagsCanReplacing;
    e->object = s;
    e->uniqueID = 0x454e5a58;   /* 'ENZX' */
    e->version = 1000;
    return e;
}
__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) { return pl_wrap(master, make, &LISTS); }

#ifdef VC_PARAMS
int main(void) { write_params_json("Enz0 X", P, kNumParams, &LISTS); return 0; }
#endif
