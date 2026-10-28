/* Tri Chorus: a three-voice bucket-brigade chorus for MPC OS (VST2, stereo), in the spirit of the classic tri-chorus
 * pedals and ensembles. Three modulated delay voices, their sweeps a third of a cycle apart, panned left, centre and
 * right; each sweep is a slow LFO plus a faster "ensemble" wobble. The voices go through a bucket-brigade's
 * darkening (Tone) and gentle saturation. Vibrato mode is the voices alone (no dry); Shimmer blends in a
 * pitch-shifted copy of the chorus (an octave up by default) for a sparkle on top. Our own code.
 * Safety, as in every plugin here: input cleaned, reset if the output goes bad or past +36 dB, output capped at
 * +12 dB (one NaN reaching MPC's mixer silences it until restart). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vst2.h"

#define PI 3.14159265358979f

enum { kMix, kRate, kDepth, kEnsemble, kWidth, kTone, kDelay, kShimmer, kShimPitch, kMode, kOutput, kNumParams };
static const char *const MODES[] = {"Chorus", "Vibrato"};
static const char *const SHIM_PITCHES[] = {"-12 st", "-5 st", "+7 st", "+12 st", "+19 st", "+24 st"};
static const float SHIM_ST[] = {-12, -5, 7, 12, 19, 24};
#define NMODES 2
#define NSHIM 6
typedef struct { const char *key, *name; float def; } Param;
static const Param PARAMS[kNumParams] = {   /* defaults as 0..1 */
    {"mix", "Mix", 0.5f}, {"rate", "Rate", 0.35f}, {"depth", "Depth", 0.5f}, {"ensemble", "Ensemble", 0.35f},
    {"width", "Width", 0.8f}, {"tone", "Tone", 0.55f}, {"delay", "Delay", 0.35f}, {"shimmer", "Shimmer", 0},
    {"shim_pitch", "Shimmer Pitch", 3.0f / (NSHIM - 1)}, {"mode", "Mode", 0}, {"output", "Output", 24.0f / 36},
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float expmap(float lo, float hi, float t) { return lo * powf(hi / lo, t); }
static int option_of(float v, int n) { return (int)lrintf(clampf(v, 0, 1) * (float)(n - 1)); }
static int finite_bits(float v) { uint32_t u; memcpy(&u, &v, 4); return (u & 0x7f800000u) != 0x7f800000u; }
static float rate_hz(float n) { return expmap(0.05f, 8, n); }
static float delay_ms(float n) { return lerp(3, 25, n); }
static float tone_hz(float n) { return expmap(2000, 16000, n); }

/* ---- building blocks (as in Duo FX) ------------------------------------------------------------------------ */
typedef struct { float *b; int mask, w; } Line;
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
static float sine01(float ph) { const float x = (ph - floorf(ph)) * 1024; const int i = (int)x; return SINE[i] + (SINE[i + 1] - SINE[i]) * (x - (float)i); }
static float hann01(float ph) { const float v = sine01(ph * 0.5f); return v * v; }
static float tanh_fast(float x) { x = clampf(x, -3, 3); const float x2 = x * x; return x * (27 + x2) / (27 + 9 * x2); }
typedef struct { float z; } OnePole;
static float lp(OnePole *f, float x, float a) { f->z += a * (x - f->z); return f->z; }
static float lp_coef(float hz, float fs) { const float a = 1 - expf(-2 * PI * hz / fs); return a > 1 ? 1 : a; }

/* the pitch shifter: two Hann grains whose start is chosen by a correlation search, so splices line up */
typedef struct { Line l; float d[2], ph[2]; int search, init; } Shifter;
static float grain_start(Shifter *s, float ratio, float win, int other) {
    const float base = 8 + (float)s->search + (ratio > 1 ? (ratio - 1) * win : 0);
    float ref[32];
    for (int i = 0; i < 32; i++) ref[i] = line_tap(&s->l, (int)(s->d[other] - (float)(i * 6) * ratio));
    float best = base, best_v = -1e30f;
    for (int o = -s->search; o <= s->search; o += 3) {
        const float c = base + (float)o;
        float xy = 0, yy = 1e-9f;
        for (int i = 0; i < 32; i++) { const float y = line_tap(&s->l, (int)(c - (float)(i * 6) * ratio)); xy += ref[i] * y; yy += y * y; }
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

/* ---- the plugin --------------------------------------------------------------------------------------------- */
typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    int ready;
    Line line;                       /* the voices read one mono line (the BBD's input) */
    Shifter sh[2];
    OnePole tone[3][2], pre, shim_lp[2];
    float lfo, fast, out_s, mix_s, depth_s, delay_s;
    uint32_t rng;
    volatile int update_display;
} Plugin;

static void reset(Plugin *s) {
    line_clear(&s->line);
    for (int c = 0; c < 2; c++) { line_clear(&s->sh[c].l); s->sh[c].init = 0; s->shim_lp[c].z = 0; }
    memset(s->tone, 0, sizeof s->tone);
    s->pre.z = 0;
}
static int setup(Plugin *s, float fs) {
    s->ready = 0;
    free(s->line.b);
    for (int c = 0; c < 2; c++) free(s->sh[c].l.b);
    s->fs = fs;
    int ok = line_init(&s->line, (int)(fs * 0.06f));
    for (int c = 0; c < 2; c++) { ok &= line_init(&s->sh[c].l, (int)(fs * 0.3f)); s->sh[c].search = (int)(fs * 0.012f); s->sh[c].init = 0; }
    s->ready = ok;
    return ok;
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
    if (!s->ready) { for (int k = 0; k < n; k++) out[0][k] = out[1][k] = 0; goto done; }
    const float fs = s->fs, ms = fs * 0.001f;
    const float *p = s->norm;
    const int vib = option_of(p[kMode], NMODES);
    const float rate = rate_hz(p[kRate]) / fs, fast = rate_hz(p[kRate]) * 9.5f / fs;   /* the ensemble wobble: ~9x faster */
    const float ens = p[kEnsemble], width = p[kWidth], shim = p[kShimmer];
    const float tone_a = lp_coef(tone_hz(p[kTone]), fs), pre_a = lp_coef(14000, fs), shim_a = lp_coef(9000, fs);
    const float ratio = powf(2, SHIM_ST[option_of(p[kShimPitch], NSHIM)] / 12);
    const float mix_t = vib ? 1 : p[kMix], og = powf(10, lerp(-24, 12, p[kOutput]) / 20);
    /* three voices panned left, centre, right (Width: towards mono) */
    const float pan[3][2] = {{lerp(0.6f, 1.0f, width), lerp(0.6f, 0.0f, width)}, {0.7f, 0.7f}, {lerp(0.6f, 0.0f, width), lerp(0.6f, 1.0f, width)}};
    for (int k = 0; k < n; k++) {
        float l = il ? il[k] : 0, r = ir ? ir[k] : 0;
        if (!finite_bits(l) || l > 8 || l < -8) l = 0;   /* only clean audio in */
        if (!finite_bits(r) || r > 8 || r < -8) r = 0;
        s->mix_s += 0.002f * (mix_t - s->mix_s);
        s->depth_s += 0.002f * (p[kDepth] - s->depth_s);
        s->delay_s += 0.0005f * (delay_ms(p[kDelay]) * ms - s->delay_s);
        s->lfo += rate; s->lfo -= floorf(s->lfo);
        s->fast += fast; s->fast -= floorf(s->fast);
        /* the BBD's input: band-limited, a little compressed */
        const float mono = lp(&s->pre, (l + r) * 0.5f, pre_a);
        line_push(&s->line, tanh_fast(mono * 1.2f) / 1.2f);
        float wl = 0, wr = 0;
        const float swing = s->depth_s * (vib ? 0.008f : 0.0045f) * fs;   /* up to 8 / 4.5 ms of sweep */
        for (int v = 0; v < 3; v++) {
            const float off = (float)v / 3;
            const float m = sine01(s->lfo + off) + ens * 0.35f * sine01(s->fast + off * 1.7f);
            float y = line_read(&s->line, s->delay_s + swing * 0.5f * (1 + m) + 2);
            y = lp(&s->tone[v][1], lp(&s->tone[v][0], y, tone_a), tone_a);   /* the BBD's darkening, two poles */
            wl += y * pan[v][0];
            wr += y * pan[v][1];
        }
        wl *= 0.75f; wr *= 0.75f;
        if (shim > 0.001f) {   /* a pitched copy of the chorus, blended in on top */
            wl += lp(&s->shim_lp[0], shift(&s->sh[0], wl, ratio, 0.05f * fs), shim_a) * shim * 0.8f;
            wr += lp(&s->shim_lp[1], shift(&s->sh[1], wr, ratio * 1.003f, 0.05f * fs), shim_a) * shim * 0.8f;
        }
        const float m = s->mix_s, dg = m < 0.5f ? 1 : 2 * (1 - m), wg = m > 0.5f ? 1 : 2 * m;
        s->out_s += 0.002f * (og - s->out_s);
        float yl = (l * dg + wl * wg) * s->out_s, yr = (r * dg + wr * wg) * s->out_s;
        if (!finite_bits(yl) || !finite_bits(yr) || fabsf(yl) > 63 || fabsf(yr) > 63) { reset(s); yl = yr = 0; }   /* past +36 dB: start over */
        out[0][k] = clampf(yl, -4, 4);   /* +12 dB ceiling */
        out[1][k] = clampf(yr, -4, 4);
    }
done:
#if defined(__arm__)
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#endif
    return;
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams) return;
    v = clampf(v, 0, 1);
    const int n = i == kMode ? NMODES : i == kShimPitch ? NSHIM : 0;
    if (n) {   /* an option: a small move off one (a Q-Link nudge) steps one; others snap */
        const float pos = v * (float)(n - 1), cur = (float)option_of(s->norm[i], n);
        int opt = n > 2 && fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1 ? (int)cur + (pos > cur ? 1 : -1) : (int)lrintf(pos);
        opt = opt < 0 ? 0 : opt > n - 1 ? n - 1 : opt;
        v = (float)opt / (float)(n - 1);
        s->update_display = 1;
    }
    s->norm[i] = v;
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }

static void display(const Plugin *s, int i, char *t) {
    const float v = s->norm[i];
    switch (i) {
    case kRate: snprintf(t, 24, rate_hz(v) < 1 ? "%.2f Hz" : "%.1f Hz", rate_hz(v)); break;
    case kDelay: snprintf(t, 24, "%.1f ms", delay_ms(v)); break;
    case kTone: snprintf(t, 24, "%.1f kHz", tone_hz(v) / 1000); break;
    case kShimPitch: snprintf(t, 24, "%s", SHIM_PITCHES[option_of(v, NSHIM)]); break;
    case kMode: snprintf(t, 24, "%s", MODES[option_of(v, NMODES)]); break;
    case kOutput: snprintf(t, 24, "%+.1f dB", lerp(-24, 12, v)); break;
    case kMix: if (option_of(s->norm[kMode], NMODES)) { snprintf(t, 24, "Vibrato"); break; } /* fallthrough */
    default: snprintf(t, 24, "%.0f%%", v * 100); break;
    }
}
static void copy_str(void *dst, const char *src, size_t max) { const size_t n = strnlen(src, max - 1); memcpy(dst, src, n); ((char *)dst)[n] = 0; }

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: free(s->line.b); for (int c = 0; c < 2; c++) free(s->sh[c].l.b); free(s); return 1;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay: if (idx >= 0 && idx < kNumParams) { char t[24]; display(s, idx, t); copy_str(p, t, 24); } return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "Tri Chorus", 32); return 1;
    case effGetVendorString: copy_str(p, "Other Things", 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate: if (opt > 1000 && fabsf(opt - s->fs) > 0.5f) setup(s, opt); return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (val) reset(s); return 1;
    case effCanDo: return -1;
    default: return 0;
    }
}

__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    sine_init();
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    for (int i = 0; i < kNumParams; i++) s->norm[i] = PARAMS[i].def;
    s->out_s = 1;
    s->mix_s = PARAMS[kMix].def;
    s->depth_s = PARAMS[kDepth].def;
    s->delay_s = 0;
    setup(s, 44100);
    s->delay_s = delay_ms(PARAMS[kDelay].def) * s->fs * 0.001f;
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
    e->uniqueID = 0x54524943;   /* 'TRIC' */
    e->version = 1000;
    return e;
}

#ifdef TRI_CHORUS_PARAMS
/* the page's parameter list (params.json), from the table above: cc -DTRI_CHORUS_PARAMS ... && ./a.out > params.json */
int main(void) {
    printf("{\n  \"name\": \"Tri Chorus\",\n  \"params\": [\n");
    for (int i = 0; i < kNumParams; i++) {
        if (i == kMode) printf("    {\"key\": \"mode\", \"name\": \"Mode\", \"options\": [\"Chorus\", \"Vibrato\"]}");
        else if (i == kShimPitch) {
            printf("    {\"key\": \"shim_pitch\", \"name\": \"Shimmer Pitch\", \"options\": [");
            for (int k = 0; k < NSHIM; k++) printf("%s\"%s\"", k ? ", " : "", SHIM_PITCHES[k]);
            printf("], \"default\": 3}");
        } else if (i == kOutput) printf("    {\"key\": \"output\", \"name\": \"Output\", \"min\": -24, \"max\": 12, \"default\": 0, \"unit\": \"dB\"}");
        else printf("    {\"key\": \"%s\", \"name\": \"%s\", \"min\": 0, \"max\": 100, \"default\": %.0f}", PARAMS[i].key, PARAMS[i].name, PARAMS[i].def * 100);
        printf(i < kNumParams - 1 ? ",\n" : "\n");
    }
    printf("  ]\n}\n");
    return 0;
}
#endif
