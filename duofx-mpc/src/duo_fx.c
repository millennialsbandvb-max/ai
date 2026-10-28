/* Duo FX: an H90-style dual effect for MPC OS (VST2, stereo). Two slots, A and B, each running one of 11
 * algorithms (delays, reverse, pitch, modulation, reverbs), in series (A then B) or in parallel. Every algorithm
 * reads the same seven knobs (Mix, Time, Feedback, Tone, Mod, Rate, Pitch); what each one does depends on the
 * algorithm, and the knob's value text says so. The algorithms are our own, written for this plugin.
 * Safety, as in every plugin here: input cleaned, a slot whose output goes bad or past +36 dB is reset, the output
 * is capped at +12 dB (one NaN reaching MPC's mixer silences it until restart). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vst2.h"

#define PI 3.14159265358979f

/* ---- parameters (the order is the VST index: saved projects and Q-Links refer to them by it) ---------------- */
enum { P_ALGO, P_MIX, P_TIME, P_FEEDBACK, P_TONE, P_MOD, P_RATE, P_PITCH, P_SLOT };   /* per slot */
enum { kRouting = 2 * P_SLOT, kOutput, kNumParams };
enum { A_OFF, A_DELAY, A_ECHO, A_REVERSE, A_DETUNE, A_PITCH, A_CHORUS, A_PHASER, A_TREMOLO, A_ROOM, A_SHIMMER,
       A_ABYSS, kNumAlgos };
static const char *const ALGOS[kNumAlgos] = {"Off", "Delay", "Echo", "Reverse", "Detune", "Pitch", "Chorus",
                                             "Phaser", "Tremolo", "Room", "Shimmer", "Abyss"};
static const char *const SLOT_NAMES[P_SLOT] = {"Algorithm", "Mix", "Time", "Feedback", "Tone", "Mod", "Rate", "Pitch"};
static const float SLOT_DEFAULTS[P_SLOT] = {0, 0.4f, 0.5f, 0.4f, 0.5f, 0.25f, 0.3f, 0.75f};   /* pitch 0.75 = +12 */

static int option_of(float v, int n) { return (int)lrintf((v < 0 ? 0 : v > 1 ? 1 : v) * (n - 1)); }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float expmap(float lo, float hi, float t) { return lo * powf(hi / lo, t); }
static int finite_bits(float v) { uint32_t u; memcpy(&u, &v, 4); return (u & 0x7f800000u) != 0x7f800000u; }

/* ---- building blocks ---------------------------------------------------------------------------------------- */
typedef struct { float *b; int mask, w; } Line;   /* a delay line, a power of two long */

static int line_init(Line *l, int min_len) {
    int n = 1;
    while (n < min_len) n <<= 1;
    l->b = calloc((size_t)n, sizeof(float));
    l->mask = n - 1;
    l->w = 0;
    return l->b != NULL;
}
static void line_clear(Line *l) { if (l->b) memset(l->b, 0, (size_t)(l->mask + 1) * sizeof(float)); l->w = 0; }
static void line_push(Line *l, float x) { l->b[l->w] = x; l->w = (l->w + 1) & l->mask; }
static float line_tap(const Line *l, int d) { return l->b[(l->w - 1 - d) & l->mask]; }
static float line_read(const Line *l, float d) {   /* d samples ago (fractional; cubic) */
    if (d < 1) d = 1;
    if (d > (float)(l->mask - 3)) d = (float)(l->mask - 3);
    const int i = (int)d;
    const float f = d - (float)i;
    const float y0 = line_tap(l, i - 1), y1 = line_tap(l, i), y2 = line_tap(l, i + 1), y3 = line_tap(l, i + 2);
    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

static float SINE[1025];
static void sine_init(void) { for (int i = 0; i <= 1024; i++) SINE[i] = sinf(2 * PI * (float)i / 1024); }
static float sine01(float ph) {   /* sin(2 pi ph), ph in [0, 1) */
    const float x = (ph - floorf(ph)) * 1024;
    const int i = (int)x;
    return SINE[i] + (SINE[i + 1] - SINE[i]) * (x - (float)i);
}

typedef struct { float z; } OnePole;   /* lowpass: y += a (x - y) */
static float lp(OnePole *f, float x, float a) { f->z += a * (x - f->z); return f->z; }
static float lp_coef(float hz, float fs) { const float a = 1 - expf(-2 * PI * hz / fs); return a > 1 ? 1 : a; }

/* the shared Tone knob for delays: below the middle a lowpass closes (20 kHz -> 600 Hz), above it a highpass opens
 * (20 Hz -> 1.5 kHz) */
typedef struct { OnePole l, h; } Tilt;
static void tilt_coef(float tone, float fs, float *a, int *hp) {
    *hp = tone >= 0.5f;
    *a = *hp ? lp_coef(expmap(20, 1500, (tone - 0.5f) * 2), fs) : lp_coef(expmap(600, 20000, tone * 2), fs);
}
static float tilt(Tilt *t, float x, float a, int hp) { return hp ? x - lp(&t->h, x, a) : lp(&t->l, x, a); }

/* a pitch shifter: two grains (Hann windows, half a grain apart, so their gains add to one) reading a delay line
 * at `ratio` times the speed it's written. Each new grain starts where its sound best matches what the other grain
 * is playing (a correlation search over about 12 ms), so the splices line up instead of cancelling the note. */
typedef struct { Line l; float d[2], ph[2]; int search, init; } Shifter;
static float hann01(float ph) { const float v = sine01(ph * 0.5f); return v * v; }
static float grain_start(Shifter *s, float ratio, float win, int other) {
    const float base = 8 + (float)s->search + (ratio > 1 ? (ratio - 1) * win : 0);
    /* what the other grain will play next (every 4th of 256 samples), against each candidate start */
    float ref[64];
    for (int i = 0; i < 64; i++) ref[i] = line_tap(&s->l, (int)(s->d[other] - (float)(i * 4) * ratio));
    float best = base, best_v = -1e30f;
    for (int o = -s->search; o <= s->search; o++) {
        const float c = base + (float)o;
        float xy = 0, yy = 1e-9f;
        for (int i = 0; i < 64; i++) {
            const float y = line_tap(&s->l, (int)(c - (float)(i * 4) * ratio));
            xy += ref[i] * y;
            yy += y * y;
        }
        const float v = xy / sqrtf(yy);
        if (v > best_v) { best_v = v; best = c; }
    }
    return best;
}
static float shift(Shifter *s, float x, float ratio, float win) {
    line_push(&s->l, x);
    if (!s->init) { s->ph[0] = 0; s->ph[1] = 0.5f; s->d[0] = s->d[1] = 8 + (float)s->search + (ratio > 1 ? (ratio - 1) * win : 0); s->init = 1; }
    float y = 0;
    for (int k = 0; k < 2; k++) {
        s->ph[k] += 1 / win;
        if (s->ph[k] >= 1) { s->ph[k] -= floorf(s->ph[k]); s->d[k] = grain_start(s, ratio, win, k ^ 1); }
        s->d[k] += 1 - ratio;
        y += line_read(&s->l, s->d[k]) * hann01(s->ph[k]);
    }
    return y;
}

/* an allpass diffuser */
typedef struct { Line l; int d; } Allpass;
static float allpass(Allpass *a, float x, float g) {
    const float v = line_tap(&a->l, a->d - 1), u = x + g * v;
    line_push(&a->l, u);
    return v - g * u;
}

/* ---- one slot ------------------------------------------------------------------------------------------------- */
#define NFDN 8
static const float FDN_MS[NFDN] = {29.7f, 37.1f, 41.1f, 43.7f, 47.9f, 53.3f, 59.1f, 67.3f};
static const float DIFF_MS[4] = {4.7f, 3.6f, 12.7f, 9.3f};

typedef struct {
    int algo;                       /* what's running (a change clears the state first) */
    float p[P_SLOT];                /* the knobs, 0..1 */
    float mix_s, time_s;            /* smoothed */
    float lfo;                      /* 0..1 */
    /* delays, reverse, pitch, chorus */
    Line dl[2];
    Tilt tl[2];
    OnePole echo_lp[2];
    float fbv[2];
    /* pitch shifters (pitch, detune, shimmer) */
    Shifter sh[2];
    /* reverse: grains reading backwards */
    float rv_ph;
    /* phaser */
    float ap_z[2][8], ph_fb[2];
    /* reverbs */
    Line fdn[NFDN];
    OnePole damp[NFDN];
    Allpass diff[2][4];
    float shim;                     /* the shimmer's pitched feedback */
    OnePole shim_lp;
    /* control values, worked out every 16 samples (slot_control) rather than per sample */
    int tick;
    float tilt_a, lp5k, lp9k; int tilt_hp;
    float base, wow, fbg;           /* delays */
    float grain, ratio;             /* reverse, pitch, shimmer */
    float ratio_c[2], pdelay;       /* detune, pitch: per side */
    float centre, tanh_k, k;        /* phaser, tremolo */
    float lfo_inc;
    float size, damp_a, depth, g[NFDN], shim_g; int ndiff, dd[2][4];   /* reverbs */
} Slot;

static int slot_init(Slot *s, float fs) {
    int ok = 1;
    for (int c = 0; c < 2; c++) {
        ok &= line_init(&s->dl[c], (int)(fs * 4.2f));            /* delay 2 s, reverse grains up to 4 s */
        ok &= line_init(&s->sh[c].l, (int)(fs * 0.3f));   /* +24 st: 3 grains of 60 ms, + the search */
        s->sh[c].search = (int)(fs * 0.012f);
        for (int k = 0; k < 4; k++) {
            ok &= line_init(&s->diff[c][k].l, (int)(fs * DIFF_MS[k] * 3 / 1000) + 4);
        }
    }
    for (int k = 0; k < NFDN; k++) ok &= line_init(&s->fdn[k], (int)(fs * FDN_MS[k] * 4.2f / 1000) + 8);
    return ok;
}
static void slot_free(Slot *s) {
    for (int c = 0; c < 2; c++) {
        free(s->dl[c].b); free(s->sh[c].l.b);
        for (int k = 0; k < 4; k++) free(s->diff[c][k].l.b);
    }
    for (int k = 0; k < NFDN; k++) free(s->fdn[k].b);
}
static void slot_clear(Slot *s) {   /* everything back to silence (keeps the knobs) */
    for (int c = 0; c < 2; c++) {
        line_clear(&s->dl[c]); line_clear(&s->sh[c].l); s->sh[c].init = 0;
        for (int k = 0; k < 4; k++) line_clear(&s->diff[c][k].l);
        memset(&s->tl[c], 0, sizeof s->tl[c]);
        s->echo_lp[c].z = 0; s->fbv[c] = 0; s->ph_fb[c] = 0;
        memset(s->ap_z[c], 0, sizeof s->ap_z[c]);
    }
    for (int k = 0; k < NFDN; k++) { line_clear(&s->fdn[k]); s->damp[k].z = 0; }
    s->rv_ph = 0; s->shim = 0; s->shim_lp.z = 0;
}

static float semis(const Slot *s) { return (float)lrintf((s->p[P_PITCH] - 0.5f) * 48); }   /* -24..+24 */
static float delay_ms(float t) { return expmap(20, 2000, t); }
static float rate_hz(float r) { return expmap(0.05f, 10, r); }
static float reverse_ms(float t) { return lerp(150, 1500, t); }

/* the slow-changing values each algorithm needs, from the knobs (every 16 samples) */
static void slot_control(Slot *s, float fs) {
    const float t = s->time_s, fb = s->p[P_FEEDBACK], mod = s->p[P_MOD];
    tilt_coef(s->p[P_TONE], fs, &s->tilt_a, &s->tilt_hp);
    s->lp5k = lp_coef(5000, fs);
    s->lp9k = lp_coef(9000, fs);
    s->lfo_inc = rate_hz(s->p[P_RATE]) / fs;
    s->ratio = powf(2, semis(s) / 12);
    const float maxd = (float)s->dl[0].mask - 8;
    switch (s->algo) {
    case A_DELAY: case A_ECHO:
        s->base = delay_ms(t) * fs / 1000;
        s->wow = mod * (s->algo == A_ECHO ? 0.004f : 0.002f) * fs;   /* tape wow: up to 4 / 2 ms */
        s->fbg = s->algo == A_ECHO ? fb * 1.05f : fb * 0.98f;       /* Echo can run away a little (it saturates) */
        break;
    case A_REVERSE:
        s->grain = reverse_ms(t) * fs / 1000;
        if (s->grain * (1 + s->ratio) + 0.004f * fs > maxd) s->grain = (maxd - 0.004f * fs) / (1 + s->ratio);
        break;
    case A_DETUNE: {
        const float cents = t * 50;
        s->ratio_c[0] = powf(2, cents / 1200);
        s->ratio_c[1] = 1 / s->ratio_c[0];
        break;
    }
    case A_PITCH: {
        const float spread = mod * 15 / 100;   /* Mod: left and right voices apart by up to 30 cents */
        s->ratio_c[0] = powf(2, (semis(s) + spread) / 12);
        s->ratio_c[1] = powf(2, (semis(s) - spread) / 12);
        s->pdelay = lerp(0.002f, 1.0f, t * t) * fs;
        break;
    }
    case A_CHORUS: s->base = lerp(0.004f, 0.03f, t) * fs; s->wow = mod * 0.006f * fs; break;
    case A_PHASER: s->centre = expmap(200, 3000, t); break;
    case A_TREMOLO: s->k = 1 + t * 20; s->tanh_k = tanhf(s->k); break;
    case A_ROOM: case A_SHIMMER: case A_ABYSS: {
        const int abyss = s->algo == A_ABYSS;
        s->size = abyss ? lerp(1.5f, 4.0f, t) : lerp(0.35f, 2.0f, t);
        const float rt60 = abyss ? expmap(4, 60, fb) : expmap(0.3f, 12, fb);
        s->damp_a = lp_coef(expmap(abyss ? 700 : 1200, 18000, s->p[P_TONE]), fs);
        s->depth = mod * (abyss ? 0.004f : 0.0015f) * fs;   /* up to 4 / 1.5 ms of wobble */
        for (int k = 0; k < NFDN; k++)
            s->g[k] = powf(10, -3 * FDN_MS[k] * s->size / 1000 / rt60) * 0.35355339f;   /* 1/sqrt(8): lossless mix */
        s->ndiff = abyss ? 4 : 2;
        for (int c = 0; c < 2; c++)
            for (int k = 0; k < 4; k++) {
                int d = (int)(DIFF_MS[k] * (c ? 1.13f : 1) * (abyss ? 2.5f : 1) * fs / 1000);
                s->dd[c][k] = d > s->diff[c][k].l.mask - 2 ? s->diff[c][k].l.mask - 2 : d;
            }
        /* how much pitched sound goes back in: less the longer the decay, so the loop always dies away */
        s->shim_g = s->algo == A_SHIMMER ? mod * 0.22f * (1 - 0.55f * fb) : 0;
        break;
    }
    default: break;
    }
}

/* the reverbs (Room, Shimmer, Abyss): an 8-line feedback delay network with input diffusion */
static void reverb(Slot *s, float fs, float inl, float inr, float *ol, float *or_) {
    float x[2] = {inl + s->shim, inr + s->shim};
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < s->ndiff; k++) { s->diff[c][k].d = s->dd[c][k]; x[c] = allpass(&s->diff[c][k], x[c], 0.6f); }
    float v[NFDN];
    for (int k = 0; k < NFDN; k++) {
        float d = FDN_MS[k] * s->size * fs / 1000;
        if (k & 1) d += s->depth * (1 + sine01(s->lfo + 0.125f * (float)k));
        v[k] = line_read(&s->fdn[k], d);
    }
    *ol = (v[0] - v[2] + v[4] - v[6] + v[1] + v[5]) * 0.35f;
    *or_ = (v[1] - v[3] + v[5] - v[7] + v[2] + v[6]) * 0.35f;
    for (int h = 1; h < NFDN; h <<= 1)   /* Hadamard mix */
        for (int i = 0; i < NFDN; i += h << 1)
            for (int j = i; j < i + h; j++) { const float a = v[j], b = v[j + h]; v[j] = a + b; v[j + h] = a - b; }
    for (int k = 0; k < NFDN; k++)
        line_push(&s->fdn[k], lp(&s->damp[k], v[k] * s->g[k], s->damp_a) + ((k & 1) ? x[1] : x[0]) * 0.5f);
    if (s->shim_g > 0) {   /* Shimmer: the reverb's sound, pitched (Pitch knob), fed back in and mixed in (Mod = how much) */
        const float y = lp(&s->shim_lp, shift(&s->sh[0], (*ol + *or_) * 0.5f, s->ratio, 0.06f * fs), s->lp9k);
        s->shim = tanhf(y * s->shim_g);
        *ol += y * s->p[P_MOD] * 0.8f;   /* and heard directly (outside the loop: no effect on its stability) */
        *or_ += y * s->p[P_MOD] * 0.8f;
    } else s->shim = 0;
}

static void slot_param_update(Slot *s, int algo) {
    if (algo != s->algo) { slot_clear(s); s->algo = algo; s->tick = 0; }
}

/* one sample through the slot: its wet sound out */
static void slot_run(Slot *s, float fs, float l, float r, float *wl, float *wr) {
    s->mix_s += 0.002f * (s->p[P_MIX] - s->mix_s);
    s->time_s += 0.0005f * (s->p[P_TIME] - s->time_s);   /* slow: turning Time bends delays like tape */
    if (!(s->tick++ & 15)) slot_control(s, fs);
    const float mod = s->p[P_MOD], fb = s->p[P_FEEDBACK];
    s->lfo += s->lfo_inc;
    s->lfo -= floorf(s->lfo);
    const float in[2] = {l, r};
    float out[2] = {0, 0};
    switch (s->algo) {
    case A_DELAY: case A_ECHO: {
        const int echo = s->algo == A_ECHO;
        for (int c = 0; c < 2; c++) {
            const float d = s->base * (c ? 1.0f : 0.993f) + s->wow * (1 + sine01(s->lfo + (c ? 0.25f : 0))) +
                            (echo ? 0.0004f * fs * sine01(s->lfo * 7.3f + 0.5f * (float)c) * mod : 0);
            float y = line_read(&s->dl[c], d);
            out[c] = y;
            y = tilt(&s->tl[c], y, s->tilt_a, s->tilt_hp);
            if (echo) { y = lp(&s->echo_lp[c], y, s->lp5k); y = tanhf(y * 1.2f) / 1.2f; }
            line_push(&s->dl[c], in[c] + y * s->fbg);
        }
        break;
    }
    case A_REVERSE: {   /* the input played backwards in grains of Time ms, pitched (Pitch), with feedback */
        s->rv_ph += 1 / s->grain;
        s->rv_ph -= floorf(s->rv_ph);
        for (int c = 0; c < 2; c++) {
            float y = 0;
            for (int k = 0; k < 2; k++) {
                float ph = s->rv_ph + 0.5f * (float)k;
                ph -= floorf(ph);
                /* the read head runs backwards at `ratio` while the write head runs forwards */
                const float d = 4 + ph * s->grain * (1 + s->ratio) + mod * 0.002f * fs * (1 + sine01(s->lfo));
                y += line_read(&s->dl[c], d) * sine01(ph * 0.5f);
            }
            out[c] = y;
            line_push(&s->dl[c], in[c] + tilt(&s->tl[c], y, s->tilt_a, s->tilt_hp) * fb * 0.9f);
        }
        break;
    }
    case A_DETUNE:   /* left up, right down by Time (0..50 cents), with a short offset; Mod/Rate drift it */
        for (int c = 0; c < 2; c++) {
            const float drift = 1 + 0.003f * mod * sine01(s->lfo + 0.5f * (float)c);
            line_push(&s->dl[c], in[c]);
            out[c] = shift(&s->sh[c], line_read(&s->dl[c], (c ? 0.011f : 0.007f) * fs), s->ratio_c[c] * drift, 0.03f * fs);
        }
        break;
    case A_PITCH:   /* harmonizer: the input delayed (Time) and shifted (Pitch); Feedback repeats it, stepping */
        for (int c = 0; c < 2; c++) {
            const float y = shift(&s->sh[c], line_read(&s->dl[c], s->pdelay), s->ratio_c[c], 0.05f * fs);
            out[c] = y;
            line_push(&s->dl[c], in[c] + tilt(&s->tl[c], y, s->tilt_a, s->tilt_hp) * fb * 0.95f);
        }
        break;
    case A_CHORUS:   /* three modulated taps a side; Feedback towards a flanger */
        for (int c = 0; c < 2; c++) {
            float y = 0;
            for (int k = 0; k < 3; k++)
                y += line_read(&s->dl[c], s->base + s->wow * 0.5f * (1 + sine01(s->lfo + (float)k / 3 + 0.17f * (float)c)));
            y *= 0.45f;
            out[c] = y;
            line_push(&s->dl[c], in[c] + tilt(&s->tl[c], y, s->tilt_a, s->tilt_hp) * fb * 0.85f);
        }
        break;
    case A_PHASER:   /* 8 allpass stages swept around Time's frequency; Feedback = resonance */
        for (int c = 0; c < 2; c++) {
            const float sw = sine01(s->lfo + (c ? 0.25f : 0));
            float f = s->centre * expf(sw * mod * 2.5f * 0.693147f);
            if (f > fs * 0.45f) f = fs * 0.45f;
            const float w = PI * f / fs, a = (1 - w) / (1 + w);
            float y = in[c] + s->ph_fb[c] * fb * 0.85f;
            for (int k = 0; k < 8; k++) {   /* first-order allpass, one state each */
                const float o = -a * y + s->ap_z[c][k];
                s->ap_z[c][k] = y + a * o;
                y = o;
            }
            s->ph_fb[c] = y;
            out[c] = (in[c] + y) * 0.5f;
        }
        break;
    case A_TREMOLO:   /* Mod = depth, Time = shape (sine -> square), Feedback = stereo spread (to auto-pan) */
        for (int c = 0; c < 2; c++) {
            const float w = tanhf(sine01(s->lfo + (c ? fb * 0.5f : 0)) * s->k) / s->tanh_k;
            out[c] = in[c] * (1 - mod * 0.5f * (1 - w));
        }
        break;
    case A_ROOM: case A_SHIMMER: case A_ABYSS:
        reverb(s, fs, l, r, &out[0], &out[1]);
        break;
    default: break;
    }
    *wl = out[0];
    *wr = out[1];
}

/* ---- the plugin --------------------------------------------------------------------------------------------- */
typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    Slot slot[2];
    int ready;
    float out_s;
    volatile int update_display;
} Plugin;

static void copy_str(void *dst, const char *src, size_t max) { strncpy((char *)dst, src, max - 1); ((char *)dst)[max - 1] = 0; }

static void apply_params(Plugin *s) {   /* knobs -> slots (on the audio thread) */
    for (int k = 0; k < 2; k++) {
        Slot *sl = &s->slot[k];
        for (int i = 0; i < P_SLOT; i++) sl->p[i] = s->norm[k * P_SLOT + i];
        slot_param_update(sl, option_of(sl->p[P_ALGO], kNumAlgos));
    }
}

static int setup(Plugin *s, float fs) {
    s->ready = 0;
    for (int k = 0; k < 2; k++) slot_free(&s->slot[k]);
    memset(s->slot, 0, sizeof s->slot);
    s->fs = fs;
    s->ready = slot_init(&s->slot[0], fs) && slot_init(&s->slot[1], fs);
    for (int k = 0; k < 2; k++) { s->slot[k].algo = -1; s->slot[k].mix_s = s->norm[k * P_SLOT + P_MIX]; s->slot[k].time_s = s->norm[k * P_SLOT + P_TIME]; }
    return s->ready;
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams) return;
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    const int n = i == kRouting ? 2 : (i % P_SLOT == P_ALGO && i < kRouting) ? kNumAlgos : 0;
    if (n) {   /* an option; a small move off one (a Q-Link nudge) steps one; others snap */
        const float pos = v * (float)(n - 1), cur = (float)option_of(s->norm[i], n);
        int opt = n > 2 && fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1 ? (int)cur + (pos > cur ? 1 : -1)
                                                                                    : (int)lrintf(pos);
        opt = opt < 0 ? 0 : opt > n - 1 ? n - 1 : opt;
        v = (float)opt / (float)(n - 1);
        s->update_display = 1;
    }
    s->norm[i] = v;
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *s = (Plugin *)e->object;
    return i >= 0 && i < kNumParams ? s->norm[i] : 0;
}

static void param_display(Plugin *s, int i, char *t) {
    const float v = s->norm[i];
    if (i == kRouting) { snprintf(t, 24, "%s", option_of(v, 2) ? "Parallel" : "Series"); return; }
    if (i == kOutput) { snprintf(t, 24, "%+.1f dB", lerp(-24, 12, v)); return; }
    const int k = i / P_SLOT, which = i % P_SLOT, algo = option_of(s->norm[k * P_SLOT + P_ALGO], kNumAlgos);
    const float semi = (float)lrintf((s->norm[k * P_SLOT + P_PITCH] - 0.5f) * 48);
    switch (which) {
    case P_ALGO: snprintf(t, 24, "%s", ALGOS[algo]); return;
    case P_MIX: snprintf(t, 24, "%.0f%%", v * 100); return;
    case P_TONE: snprintf(t, 24, v < 0.48f ? "Dark %.0f" : v > 0.52f ? "Bright %.0f" : "Flat", fabsf(v - 0.5f) * 200); return;
    case P_RATE: snprintf(t, 24, algo == A_OFF ? "-" : "%.2f Hz", rate_hz(v)); return;
    case P_PITCH:
        if (algo == A_REVERSE || algo == A_PITCH || algo == A_SHIMMER) snprintf(t, 24, "%+.0f st", semi);
        else snprintf(t, 24, "-");
        return;
    }
    switch (algo) {
    case A_DELAY: case A_ECHO:
        if (which == P_TIME) snprintf(t, 24, "%.0f ms", delay_ms(v));
        else if (which == P_FEEDBACK) snprintf(t, 24, "%.0f%%", v * 100);
        else snprintf(t, 24, "Wow %.0f%%", v * 100);
        return;
    case A_REVERSE:
        if (which == P_TIME) snprintf(t, 24, "%.0f ms", reverse_ms(v));
        else snprintf(t, 24, "%.0f%%", v * 100);
        return;
    case A_DETUNE:
        if (which == P_TIME) snprintf(t, 24, "%.0f cents", v * 50);
        else if (which == P_FEEDBACK) snprintf(t, 24, "-");
        else snprintf(t, 24, "Drift %.0f%%", v * 100);
        return;
    case A_PITCH:
        if (which == P_TIME) snprintf(t, 24, "%.0f ms", lerp(2, 1000, v * v));
        else if (which == P_FEEDBACK) snprintf(t, 24, "%.0f%%", v * 100);
        else snprintf(t, 24, "Spread %.0f c", v * 30);
        return;
    case A_CHORUS:
        if (which == P_TIME) snprintf(t, 24, "%.1f ms", lerp(4, 30, v));
        else snprintf(t, 24, "%.0f%%", v * 100);
        return;
    case A_PHASER:
        if (which == P_TIME) snprintf(t, 24, "%.0f Hz", expmap(200, 3000, v));
        else snprintf(t, 24, "%.0f%%", v * 100);
        return;
    case A_TREMOLO:
        if (which == P_TIME) snprintf(t, 24, v < 0.1f ? "Smooth" : v > 0.9f ? "Square" : "Shape %.0f", v * 100);
        else if (which == P_FEEDBACK) snprintf(t, 24, v > 0.95f ? "Auto-pan" : "Spread %.0f%%", v * 100);
        else snprintf(t, 24, "Depth %.0f%%", v * 100);
        return;
    case A_ROOM: case A_SHIMMER: case A_ABYSS:
        if (which == P_TIME) snprintf(t, 24, "Size %.0f%%", v * 100);
        else if (which == P_FEEDBACK) snprintf(t, 24, "%.1f s", algo == A_ABYSS ? expmap(4, 60, v) : expmap(0.3f, 12, v));
        else snprintf(t, 24, algo == A_SHIMMER ? "Shimmer %.0f%%" : "%.0f%%", v * 100);
        return;
    default: snprintf(t, 24, "-"); return;
    }
}

static void process(AEffect *e, float **in, float **out, int32_t n) {
#if defined(__arm__)
    uint32_t fpscr;   /* flush denormals (slow on ARMv7's VFP) while we run */
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr | (1u << 24)));
#endif
    Plugin *s = (Plugin *)e->object;
    if (s->update_display && s->master) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    const float *il = in && in[0] ? in[0] : NULL, *ir = in && in[1] ? in[1] : il;
    if (!s->ready) {
        for (int k = 0; k < n; k++) { out[0][k] = 0; out[1][k] = 0; }
        goto done;
    }
    apply_params(s);
    const int parallel = option_of(s->norm[kRouting], 2);
    const float og = powf(10, lerp(-24, 12, s->norm[kOutput]) / 20);
    for (int k = 0; k < n; k++) {
        float l = il ? il[k] : 0, r = ir ? ir[k] : 0;
        if (!finite_bits(l) || l > 8 || l < -8) l = 0;   /* only clean audio in */
        if (!finite_bits(r) || r > 8 || r < -8) r = 0;
        float yl, yr;
        if (!parallel) {
            yl = l; yr = r;
            for (int c = 0; c < 2; c++) {
                Slot *sl = &s->slot[c];
                if (sl->algo == A_OFF) continue;
                float wl, wr;
                slot_run(sl, s->fs, yl, yr, &wl, &wr);
                const float m = sl->mix_s, dg = m < 0.5f ? 1 : 2 * (1 - m), wg = m > 0.5f ? 1 : 2 * m;
                yl = yl * dg + wl * wg;
                yr = yr * dg + wr * wg;
            }
        } else {
            float dg = 0, sum_l = 0, sum_r = 0;
            for (int c = 0; c < 2; c++) {
                Slot *sl = &s->slot[c];
                if (sl->algo == A_OFF) { dg += 0.5f; continue; }
                float wl, wr;
                slot_run(sl, s->fs, l, r, &wl, &wr);
                const float m = sl->mix_s;
                dg += 0.5f * (m < 0.5f ? 1 : 2 * (1 - m));
                const float wg = m > 0.5f ? 1 : 2 * m;
                sum_l += wl * wg; sum_r += wr * wg;
            }
            yl = l * dg + sum_l;
            yr = r * dg + sum_r;
        }
        s->out_s += 0.002f * (og - s->out_s);
        yl *= s->out_s; yr *= s->out_s;
        if (!finite_bits(yl) || !finite_bits(yr) || fabsf(yl) > 63 || fabsf(yr) > 63) {   /* past +36 dB: start over */
            slot_clear(&s->slot[0]); slot_clear(&s->slot[1]);
            yl = yr = 0;
        }
        out[0][k] = yl > 4 ? 4 : yl < -4 ? -4 : yl;   /* +12 dB ceiling */
        out[1][k] = yr > 4 ? 4 : yr < -4 ? -4 : yr;
    }
done:
#if defined(__arm__)
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#endif
    return;
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    (void)val;
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: for (int k = 0; k < 2; k++) slot_free(&s->slot[k]); free(s); return 1;
    case effGetParamName:
        if (idx >= 0 && idx < kNumParams) {
            char t[32];
            if (idx == kRouting) snprintf(t, sizeof t, "Routing");
            else if (idx == kOutput) snprintf(t, sizeof t, "Output");
            else snprintf(t, sizeof t, "%c %s", idx < P_SLOT ? 'A' : 'B', SLOT_NAMES[idx % P_SLOT]);
            copy_str(p, t, 32);
        }
        return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay: if (idx >= 0 && idx < kNumParams) { char t[24]; param_display(s, idx, t); copy_str(p, t, 24); } return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "Duo FX", 32); return 1;
    case effGetVendorString: copy_str(p, "Duo-FX", 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate: if (opt > 1000 && fabsf(opt - s->fs) > 0.5f) setup(s, opt); return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (val) { slot_clear(&s->slot[0]); slot_clear(&s->slot[1]); } return 1;
    case effCanDo: return -1;
    default: return 0;
    }
}

__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    sine_init();
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < P_SLOT; i++) s->norm[k * P_SLOT + i] = SLOT_DEFAULTS[i];
    s->norm[P_ALGO] = (float)A_DELAY / (kNumAlgos - 1);              /* A: delay into B: room */
    s->norm[P_SLOT + P_ALGO] = (float)A_ROOM / (kNumAlgos - 1);
    s->norm[P_SLOT + P_FEEDBACK] = 0.55f;
    s->norm[kRouting] = 0;
    s->norm[kOutput] = 24.0f / 36;                                   /* 0 dB */
    s->out_s = 1;
    setup(s, 44100);
    AEffect *e = &s->fx;
    e->magic = VST_MAGIC;
    e->dispatcher = dispatcher;
    e->process = process;
    e->processReplacing = process;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->numParams = kNumParams;
    e->numInputs = e->numOutputs = 2;
    e->flags = effFlagsCanReplacing;
    e->object = s;
    e->uniqueID = 0x44554f46;   /* 'DUOF' */
    e->version = 1000;
    return e;
}
