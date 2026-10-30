/* vocal_core.h: what the vocal plugins share (VoxLive 3X, HedRush Harmony, Vocal Rack). Our own code.
 *  - a parameter table (knobs, options, toggles, buttons, preset steppers) with its display text and params.json;
 *  - filters (one-pole, biquads), delay lines, a small FDN reverb;
 *  - a pitch tracker (YIN on a 4x decimated copy) and scale maths (keys, scales, diatonic steps, chords);
 *  - held MIDI notes;
 *  - the voice: TD-PSOLA. Grains two source periods long, taken one source period apart and laid down one target
 *    period apart, so the pitch moves and the formants (the vowel, the "who") stay; reading each grain faster or
 *    slower moves the formants too (Gender). Unvoiced sound passes through unshifted grains (Hann at its own
 *    period sums back to the input). Latency: about 30 ms (the longest period it follows, 80 Hz, twice).
 * Safety, as in every plugin here: input cleaned, reset if the output goes bad or past +36 dB, output capped at
 * +12 dB (one NaN reaching MPC's mixer silences it until restart). */
#ifndef VOCAL_CORE_H
#define VOCAL_CORE_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vst2.h"
#include "../../tools/picklist.h"

#define PI 3.14159265358979f
#define VC_VENDOR "Other Things"

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float expmap(float lo, float hi, float t) { return lo * powf(hi / lo, t); }
static int option_of(float v, int n) { return n < 2 ? 0 : (int)lrintf(clampf(v, 0, 1) * (float)(n - 1)); }
static int finite_bits(float v) { uint32_t u; memcpy(&u, &v, 4); return (u & 0x7f800000u) != 0x7f800000u; }
static float db(float d) { return powf(10, d / 20); }
static float tanh_fast(float x) { x = clampf(x, -3, 3); const float x2 = x * x; return x * (27 + x2) / (27 + 9 * x2); }
static void copy_str(void *dst, const char *src, size_t max) { const size_t n = strnlen(src, max - 1); memcpy(dst, src, n); ((char *)dst)[n] = 0; }
static uint32_t vc_rng = 22222u;
static float rnd(void) { vc_rng = vc_rng * 1664525u + 1013904223u; return (float)(vc_rng >> 8) / 16777216.0f; }   /* 0..1 */

/* ==== parameters ============================================================================================ */
enum { K_LIN, K_EXP, K_INT, K_OPT, K_TOGGLE, K_BUTTON, K_PRESET };
typedef struct { const char *key, *name; int kind; float lo, hi, def; const char *unit; const char *const *opts; int nopts; } ParamDef;
#define LIN(k, n, lo, hi, d, u) {k, n, K_LIN, lo, hi, d, u, NULL, 0}
#define PCT(k, n, d) LIN(k, n, 0, 100, d, "%")
#define EXPP(k, n, lo, hi, d, u) {k, n, K_EXP, lo, hi, d, u, NULL, 0}
#define INTP(k, n, lo, hi, d, u) {k, n, K_INT, lo, hi, d, u, NULL, 0}
#define OPT(k, n, list, d) {k, n, K_OPT, 0, (float)(sizeof list / sizeof *list - 1), d, "", list, (int)(sizeof list / sizeof *list)}
#define TOGGLE(k, n, d) {k, n, K_TOGGLE, 0, 1, d, "", NULL, 2}
#define BUTTON(k, n) {k, n, K_BUTTON, 0, 1, 0, "", NULL, 2}
#define PRESET(k, n) {k, n, K_PRESET, 0, 1, 0, "", NULL, 0}

static float pv(const ParamDef *p, float n) {   /* 0..1 -> its own units (an option: its index) */
    n = clampf(n, 0, 1);
    switch (p->kind) {
    case K_EXP: return expmap(p->lo, p->hi, n);
    case K_INT: case K_OPT: return roundf(lerp(p->lo, p->hi, n));
    case K_TOGGLE: case K_BUTTON: return n > 0.5f ? 1 : 0;
    default: return lerp(p->lo, p->hi, n);
    }
}
static float pn(const ParamDef *p, float v) {   /* the reverse */
    if (p->kind == K_EXP) return clampf(logf(v / p->lo) / logf(p->hi / p->lo), 0, 1);
    return p->hi > p->lo ? clampf((v - p->lo) / (p->hi - p->lo), 0, 1) : 0;
}
/* a new value from the host for an option / whole-number knob: a small move off one (a Q-Link nudge) steps one */
static float snap(const ParamDef *p, float v, float cur_n) {
    if (p->kind == K_TOGGLE) return v > 0.5f ? 1 : 0;
    if (p->kind != K_OPT && p->kind != K_INT) return v;
    const int n = (int)(p->hi - p->lo) + 1;
    if (n < 2) return 0;
    const float pos = v * (float)(n - 1), cur = (float)option_of(cur_n, n);
    int opt = n > 2 && fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1 ? (int)cur + (pos > cur ? 1 : -1) : (int)lrintf(pos);
    opt = opt < 0 ? 0 : opt > n - 1 ? n - 1 : opt;
    return (float)opt / (float)(n - 1);
}
static void pdisplay(const ParamDef *p, float n, char *t) {   /* (not K_PRESET: the plugin names its presets) */
    const float v = pv(p, n);
    if (p->kind == K_OPT) { snprintf(t, 24, "%s", p->opts[(int)v]); return; }
    if (p->kind == K_TOGGLE) { snprintf(t, 24, "%s", v > 0.5f ? "On" : "Off"); return; }
    if (p->kind == K_BUTTON) { snprintf(t, 24, "%s", v > 0.5f ? "On" : "Off"); return; }
    if (!strcmp(p->unit, "ms") && v >= 1000) { snprintf(t, 24, "%.2f s", v / 1000); return; }
    if (!strcmp(p->unit, "Hz") && v >= 1000) { snprintf(t, 24, "%.1f kHz", v / 1000); return; }
    if (!strcmp(p->unit, "dB")) { snprintf(t, 24, "%+.1f dB", v); return; }
    if (!strcmp(p->unit, "c") || !strcmp(p->unit, "st")) { snprintf(t, 24, "%+.0f %s", v, p->unit); return; }
    const int pct = !strcmp(p->unit, "%");
    if (p->kind == K_INT || pct || fabsf(v) >= 100) snprintf(t, 24, "%.0f%s%s", v, p->unit[0] && !pct ? " " : "", p->unit);
    else snprintf(t, 24, fabsf(v) >= 10 ? "%.1f %s" : "%.2f %s", v, p->unit);
}
/* params.json for the page kit, from the table */
/* (pl: the plugin's full-page lists, tools/picklist.h, or NULL) */
static void write_params_json(const char *plugin, const ParamDef *P, int n, const PickLists *pl) {
    printf("{\n  \"name\": \"%s\",\n  \"params\": [\n", plugin);
    for (int i = 0; i < n; i++) {
        const ParamDef *p = &P[i];
        printf("    {\"key\": \"%s\", \"name\": \"%s\"", p->key, p->name);
        if (p->kind == K_OPT) {
            printf(", \"options\": [");
            for (int k = 0; k < p->nopts; k++) printf("%s\"%s\"", k ? ", " : "", p->opts[k]);
            printf("], \"default\": %d", (int)p->def);
        } else if (p->kind == K_TOGGLE) printf(", \"options\": [\"Off\", \"On\"], \"default\": %d", (int)p->def);
        else if (p->kind == K_BUTTON) printf(", \"options\": [\"Off\", \"On\"], \"momentary\": true");
        else if (p->kind == K_PRESET) printf(", \"min\": 0, \"max\": 1, \"display\": \"string\", \"type\": \"stepper\"");
        else printf(", \"min\": %g, \"max\": %g, \"default\": %g%s%s%s", p->lo, p->hi, p->def, p->unit[0] ? ", \"unit\": \"" : "",
                    p->unit, p->unit[0] ? "\"" : "");
        printf("}%s", i < n - 1 ? ",\n" : "");
    }
    if (pl) pl_params_json(pl, stdout);
    printf("\n  ]\n}\n");
}

/* ==== building blocks ======================================================================================= */
typedef struct { float *b; int mask, w; } Line;
static int line_init(Line *l, int min_len) {
    int n = 1;
    while (n < min_len) n <<= 1;
    free(l->b);
    l->b = calloc((size_t)n, sizeof(float));
    l->mask = n - 1;
    l->w = 0;
    return l->b != NULL;
}
static void line_free(Line *l) { free(l->b); l->b = NULL; }
static void line_clear(Line *l) { if (l->b) memset(l->b, 0, (size_t)(l->mask + 1) * sizeof(float)); l->w = 0; }
static void line_push(Line *l, float x) { l->b[l->w] = x; l->w = (l->w + 1) & l->mask; }
static float line_tap(const Line *l, int d) { return l->b[(l->w - 1 - d) & l->mask]; }
static float line_read(const Line *l, float d) {   /* d samples ago (fractional; linear) */
    if (d < 0) d = 0;
    if (d > (float)(l->mask - 2)) d = (float)(l->mask - 2);
    const int i = (int)d;
    const float f = d - (float)i;
    return line_tap(l, i) * (1 - f) + line_tap(l, i + 1) * f;
}

typedef struct { float z; } OnePole;
static float lp(OnePole *f, float x, float a) { f->z += a * (x - f->z); return f->z; }
static float hp(OnePole *f, float x, float a) { return x - lp(f, x, a); }
static float lp_coef(float hz, float fs) { const float a = 1 - expf(-2 * PI * hz / fs); return a > 1 ? 1 : a; }

typedef struct { float b0, b1, b2, a1, a2; } BqCo;
typedef struct { float z1, z2; } Bq;
static float bq(Bq *s, const BqCo *c, float x) {
    const float y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return y;
}
enum { BQ_LP, BQ_HP, BQ_BP, BQ_PEAK, BQ_LSHELF, BQ_HSHELF };
static BqCo bq_co(int type, float hz, float q, float gain_db, float fs) {   /* RBJ cookbook */
    hz = clampf(hz, 10, fs * 0.45f);
    const float A = powf(10, gain_db / 40), w = 2 * PI * hz / fs, cw = cosf(w), sw = sinf(w), al = sw / (2 * q);
    float b0, b1, b2, a0, a1, a2;
    switch (type) {
    case BQ_LP: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case BQ_HP: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case BQ_BP: b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case BQ_PEAK: b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; break;
    case BQ_LSHELF: {
        const float r = 2 * sqrtf(A) * al;
        b0 = A * ((A + 1) - (A - 1) * cw + r); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - r);
        a0 = (A + 1) + (A - 1) * cw + r; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - r; break;
    }
    default: {
        const float r = 2 * sqrtf(A) * al;
        b0 = A * ((A + 1) + (A - 1) * cw + r); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - r);
        a0 = (A + 1) - (A - 1) * cw + r; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - r; break;
    }
    }
    return (BqCo){b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

/* a small stereo reverb: 8-line FDN (Hadamard), damped, with two diffusing allpasses per side */
#define NFDN 8
static const float FDN_MS[NFDN] = {29.7f, 37.1f, 41.1f, 43.7f, 47.9f, 53.3f, 59.1f, 67.3f};
typedef struct { Line l[NFDN], ap[2][2]; OnePole damp[NFDN]; float g[NFDN], scale, damp_a; int apd[2][2]; } Verb;
static int verb_init(Verb *v, float fs) {
    int ok = 1;
    for (int k = 0; k < NFDN; k++) ok &= line_init(&v->l[k], (int)(fs * FDN_MS[k] * 2.2f / 1000) + 8);
    for (int c = 0; c < 2; c++) for (int k = 0; k < 2; k++) ok &= line_init(&v->ap[c][k], (int)(fs * 0.02f));
    return ok;
}
static void verb_free(Verb *v) { for (int k = 0; k < NFDN; k++) line_free(&v->l[k]); for (int c = 0; c < 2; c++) for (int k = 0; k < 2; k++) line_free(&v->ap[c][k]); }
static void verb_clear(Verb *v) {
    for (int k = 0; k < NFDN; k++) { line_clear(&v->l[k]); v->damp[k].z = 0; }
    for (int c = 0; c < 2; c++) for (int k = 0; k < 2; k++) line_clear(&v->ap[c][k]);
}
static void verb_set(Verb *v, float size, float rt60, float damp_hz, float fs) {   /* size 0.5..2 (line scale) */
    v->scale = size * fs / 1000;
    for (int k = 0; k < NFDN; k++) v->g[k] = clampf(powf(10, -3 * FDN_MS[k] * size / 1000 / rt60), 0, 0.995f) * 0.35355339f;
    v->damp_a = lp_coef(damp_hz, fs);
    static const float AP[2][2] = {{4.7f, 11.3f}, {5.3f, 12.7f}};
    for (int c = 0; c < 2; c++) for (int k = 0; k < 2; k++) v->apd[c][k] = (int)(AP[c][k] * fs / 1000);
}
static void verb_run(Verb *v, float inl, float inr, float *ol, float *or_) {
    float x[2] = {inl, inr};
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < 2; k++) {   /* allpass, g 0.6 */
            Line *l = &v->ap[c][k];
            const float d = line_tap(l, v->apd[c][k] - 1), u = x[c] + 0.6f * d;
            line_push(l, u);
            x[c] = d - 0.6f * u;
        }
    float s[NFDN];
    for (int k = 0; k < NFDN; k++) s[k] = line_read(&v->l[k], FDN_MS[k] * v->scale);
    *ol = (s[0] - s[2] + s[4] - s[6] + s[1] + s[5]) * 0.35f;
    *or_ = (s[1] - s[3] + s[5] - s[7] + s[2] + s[6]) * 0.35f;
    for (int h = 1; h < NFDN; h <<= 1)
        for (int i = 0; i < NFDN; i += h << 1)
            for (int j = i; j < i + h; j++) { const float a = s[j], b = s[j + h]; s[j] = a + b; s[j + h] = a - b; }
    for (int k = 0; k < NFDN; k++) line_push(&v->l[k], clampf(lp(&v->damp[k], s[k] * v->g[k], v->damp_a), -20, 20) + x[k & 1] * 0.5f);
}

/* ==== pitch: the tracker and the scales ===================================================================== */
#define TRK_N 1024
typedef struct { float buf[TRK_N]; int w, dec, cnt; float acc, hz, conf, hist[3]; OnePole pre; } Tracker;
static void track(Tracker *t, float x, float fs) {   /* YIN on a 4x decimated copy, every 512 samples */
    t->acc += lp(&t->pre, x, 0.25f);
    if (++t->dec < 4) return;
    t->buf[t->w] = t->acc * 0.25f;
    t->w = (t->w + 1) & (TRK_N - 1);
    t->acc = 0; t->dec = 0;
    if (++t->cnt < 128) return;
    t->cnt = 0;
    const float fsd = fs / 4;
    const int TMIN = (int)(fsd / 1100), TMAX = (int)(fsd / 75) < 300 ? (int)(fsd / 75) : 300;
    enum { W = 320 };
    float d[302], x0[W + 302];
    for (int i = 0; i < W + TMAX; i++) x0[i] = t->buf[(t->w - (W + TMAX) + i) & (TRK_N - 1)];
    float energy = 0;
    for (int i = 0; i < W; i++) energy += x0[i] * x0[i];
    if (energy < 2e-5f) { t->conf = 0; return; }   /* (about -60 dB) */
    float run = 0;
    d[0] = 1;
    int best = -1;
    for (int tau = 1; tau <= TMAX; tau++) {
        float s = 0;
        for (int i = 0; i < W; i++) { const float e = x0[i] - x0[i + tau]; s += e * e; }
        run += s;
        d[tau] = s * (float)tau / (run + 1e-12f);
        if (tau > TMIN + 1 && best < 0 && d[tau - 1] < 0.2f && d[tau - 1] <= d[tau] && d[tau - 1] <= d[tau - 2]) best = tau - 1;
    }
    if (best < 0) { t->conf = 0; return; }
    const float a = d[best - 1], b = d[best], c = d[best + 1], den = a - 2 * b + c;
    const float tau = (float)best + (fabsf(den) > 1e-9f ? 0.5f * (a - c) / den : 0);
    const float hz = fsd / tau;
    /* a median of the last three readings: one stray octave jump doesn't move the voices */
    t->hist[0] = t->hist[1]; t->hist[1] = t->hist[2]; t->hist[2] = hz;
    const float h0 = t->hist[0], h1 = t->hist[1], h2 = t->hist[2];
    t->hz = h0 > h1 ? (h1 > h2 ? h1 : h0 > h2 ? h2 : h0) : (h0 > h2 ? h0 : h1 > h2 ? h2 : h1);
    if (t->hz <= 0) t->hz = hz;
    t->conf = 1 - b;
}
static float midi_of(float hz) { return 69 + 12 * log2f(hz / 440); }

static const char *const KEYS[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static const char *const SCALES[] = {"Major", "Minor", "Dorian", "Mixolydian", "Harm Minor", "Penta Maj", "Penta Min", "Blues", "Chromatic"};
static const int SCALE_STEPS[9][13] = {   /* the count, then each degree's semitones */
    {7, 0, 2, 4, 5, 7, 9, 11}, {7, 0, 2, 3, 5, 7, 8, 10}, {7, 0, 2, 3, 5, 7, 9, 10}, {7, 0, 2, 4, 5, 7, 9, 10},
    {7, 0, 2, 3, 5, 7, 8, 11}, {5, 0, 2, 4, 7, 9}, {5, 0, 3, 5, 7, 10}, {6, 0, 3, 5, 6, 7, 10},
    {12, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
static float nearest_in_scale(float m, int key, int scale) {
    const int *st = SCALE_STEPS[scale];
    float best = m, bd = 1e9f;
    const int base = (int)floorf((m - (float)key) / 12);
    for (int o = -1; o <= 1; o++)
        for (int i = 0; i < st[0]; i++) {
            const float c = (float)(key + st[1 + i] + 12 * (base + o));
            if (fabsf(c - m) < bd) { bd = fabsf(c - m); best = c; }
        }
    return best;
}
/* the note `steps` scale degrees from scale note q (q must be in the scale) */
static float diatonic_from(float q, int key, int scale, int steps) {
    const int *st = SCALE_STEPS[scale];
    const int n = st[0], note = (int)lrintf(q), rel = ((note - key) % 12 + 12) % 12, oct = (note - key - rel) / 12;
    int idx = 0;
    for (int i = 0; i < n; i++) if (st[1 + i] <= rel) idx = i;
    int target = idx + steps, toct = oct;
    while (target < 0) { target += n; toct--; }
    while (target >= n) { target -= n; toct++; }
    return (float)(key + toct * 12 + st[1 + target]);
}
/* harmony intervals (a voice's choice), as scale steps: 2nd = 1, 3rd = 2, ... (a 7-note scale; others scale them) */
static const char *const INTERVALS[] = {"Off", "Oct Down", "6th Down", "5th Down", "4th Down", "3rd Down", "Unison",
                                        "3rd Up", "4th Up", "5th Up", "6th Up", "Oct Up", "Oct+3rd", "Oct+5th"};
#define N_INTERVALS 14
static const int INTERVAL_STEPS[N_INTERVALS] = {0, -7, -5, -4, -3, -2, 0, 2, 3, 4, 5, 7, 9, 11};
/* a harmony note for lead note q (in the scale), for interval choice c in key/scale; chord tones (MIDI) instead
 * when a chord is held: up/down choices take the 1st/2nd/3rd chord tone above/below */
static float harmony_note(float q, int key, int scale, int c, const int *chord_pc, int nchord) {
    if (c <= 0) return q;
    const int steps = INTERVAL_STEPS[c];
    if (steps == 0) return q;
    if (steps == 7 || steps == -7) return q + (float)(steps / 7 * 12);
    if (nchord > 0) {   /* the chord's tones in order above (or below) the lead note */
        const int up = steps > 0;
        int a = up ? steps : -steps, ext = 0;
        if (a > 7) { a -= 7; ext = 12; }
        const int k = a <= 2 ? 1 : a <= 4 ? 2 : 3, note = (int)lrintf(q);
        int found = 0;
        for (int d = 1; d <= 24; d++) {
            const int cand = up ? note + d : note - d, pc = ((cand % 12) + 12) % 12;
            for (int i = 0; i < nchord; i++)
                if (chord_pc[i] == pc && ++found == k) return (float)(cand + (up ? ext : -ext));
        }
        return q;
    }
    const int n = SCALE_STEPS[scale][0];
    int st = steps;
    if (n != 7) st = (int)lrintf((float)steps * (float)n / 7);   /* (pentatonic, blues, chromatic: the nearest degree) */
    if (st == 0) st = steps > 0 ? 1 : -1;
    return diatonic_from(q, key, scale, st);
}

/* ==== MIDI: the notes held (for chords and Notes mode) ====================================================== */
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4];
                 char detune, noteOffVelocity, reserved1, reserved2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstMidiEvent *events[2]; } VstEvents;
enum { effProcessEvents = 25 };
typedef struct { unsigned char on[128]; int n; } Held;
static void held_midi(Held *h, const unsigned char *m) {
    const int st = m[0] & 0xf0;
    if (st == 0x90 && m[2] > 0) { if (!h->on[m[1] & 127]) { h->on[m[1] & 127] = 1; h->n++; } }
    else if (st == 0x80 || (st == 0x90 && m[2] == 0)) { if (h->on[m[1] & 127]) { h->on[m[1] & 127] = 0; h->n--; } }
    else if (st == 0xb0 && (m[1] == 123 || m[1] == 120)) { memset(h->on, 0, sizeof h->on); h->n = 0; }
}
static int held_notes(const Held *h, int *out, int max) {   /* lowest first */
    int n = 0;
    for (int i = 0; i < 128 && n < max; i++) if (h->on[i]) out[n++] = i;
    return n;
}
static int held_pcs(const Held *h, int *pc) {   /* the pitch classes held (a chord) */
    int n = 0, seen[12] = {0};
    for (int i = 0; i < 128; i++) if (h->on[i] && !seen[i % 12]) { seen[i % 12] = 1; pc[n++] = i % 12; }
    return n;
}

/* ==== the voice: TD-PSOLA ===================================================================================== */
/* All voices read the same input history (VoiceIn: mono, ~1.5 s). Times are in samples since start (int64). */
typedef struct { Line l; int64_t n; float fs; int tmax, delay; } VoiceIn;
static int voicein_init(VoiceIn *v, float fs) {
    v->fs = fs;
    v->tmax = (int)(fs / 80) + 2;                 /* the longest period followed: 80 Hz */
    v->delay = (int)((float)v->tmax * 1.5f) + 8;  /* room for a grain read up to 1.45x faster (Gender) */
    v->n = 0;
    return line_init(&v->l, (int)(fs * 1.5f));
}
static void voicein_push(VoiceIn *v, float x) { line_push(&v->l, x); v->n++; }
static float voicein_at(const VoiceIn *v, double t) {   /* the input at absolute time t (fractional) */
    return line_read(&v->l, (float)((double)(v->n - 1) - t));
}

/* the grain window, 0.5 + 0.5 cos(pi x) for x in 0..1, tabulated (cosf per grain sample was most of the cost) */
#define HANN_N 1024
static float HANN[HANN_N + 2];
static void hann_init(void) { for (int i = 0; i <= HANN_N + 1; i++) HANN[i] = 0.5f + 0.5f * cosf(PI * (float)i / HANN_N); }
static float hann_at(float x) {   /* |x| < 1 */
    const float p = fabsf(x) * HANN_N;
    const int i = (int)p;
    return HANN[i] + (HANN[i + 1] - HANN[i]) * (p - (float)i);
}
#define ACC_N 4096
typedef struct { float acc[ACC_N]; double snext, ap; float gain; } Voice;
static void voice_reset(Voice *v, const VoiceIn *in) {
    memset(v->acc, 0, sizeof v->acc);
    v->snext = (double)(in->n - in->delay);
    v->ap = v->snext;
    v->gain = 0;
}
/* one output sample of this voice. t0: the source period now (samples), ratio: target / source pitch,
 * form: grain read speed (1: formants kept; >1 up, <1 down), out_t = when (in input time) this sample is from. */
static float voice_run(Voice *v, const VoiceIn *in, float t0, float ratio, float form) {
    const double t_out = (double)(in->n - in->delay);
    t0 = clampf(t0, 16, (float)in->tmax - 2);
    ratio = clampf(ratio, 0.25f, 4);
    form = clampf(form, 0.68f, 1.45f);
    if (v->snext < t_out - 2.0 * in->tmax || v->snext > t_out + 4.0 * in->tmax) { v->snext = t_out; v->ap = t_out; }   /* (lost: resync) */
    while (v->snext <= t_out) {   /* lay down the grains due: centred on snext, two source periods long */
        const double s = v->snext;
        while (v->ap + 0.5 * t0 < s) v->ap += t0;   /* the analysis mark nearest: marks one source period apart */
        if (v->ap > s + t0) v->ap = s;
        const int L = (int)t0;
        const float ts = t0 / ratio, amp = ts / t0;
        const int64_t c = (int64_t)floor(s);
        const float frac = (float)(s - (double)c);   /* (the grain's centre is between samples: placed exactly) */
        const float inv = 1 / t0;
        /* the input around the analysis mark, read straight from the line (positions relative to its newest sample) */
        const float base = (float)((double)(in->n - 1) - v->ap);
        const Line *ln = &in->l;
        for (int i = -L; i <= L + 1; i++) {
            const float off = (float)i - frac;
            if (off <= -t0 || off >= t0) continue;
            float d = base - off * form;
            d = d < 0 ? 0 : d > (float)(ln->mask - 2) ? (float)(ln->mask - 2) : d;
            const int di = (int)d;
            const float f = d - (float)di, x0 = ln->b[(ln->w - 1 - di) & ln->mask], x1 = ln->b[(ln->w - 2 - di) & ln->mask];
            v->acc[(c + i) & (ACC_N - 1)] += hann_at(off * inv) * amp * (x0 + (x1 - x0) * f);
        }
        v->snext += ts;
    }
    const int64_t r = (int64_t)floor(t_out) - in->tmax - 1;   /* every grain touching this sample is laid down */
    const float y = v->acc[r & (ACC_N - 1)];
    v->acc[r & (ACC_N - 1)] = 0;
    return y;
}
/* the input itself, as late as a voice (for a dry lead that lines up with the voices) */
static float voicein_late(const VoiceIn *in) { return voicein_at(in, (double)(in->n - in->delay) - in->tmax - 1); }

/* ==== shared VST bits ========================================================================================= */
#if defined(__arm__)   /* flush denormals (slow on ARMv7's VFP) while a block runs */
#define VC_FLUSH_DENORMALS_BEGIN                                               \
    uint32_t fpscr_saved = 0;                                                  \
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr_saved));                    \
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr_saved | (1u << 24)))
#define VC_FLUSH_DENORMALS_END __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr_saved))
#else
#define VC_FLUSH_DENORMALS_BEGIN do {} while (0)
#define VC_FLUSH_DENORMALS_END do {} while (0)
#endif
#endif
