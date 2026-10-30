/* Ambient Lab: eleven "machines" for strange ambient sound, for MPC OS (VST2, stereo). Each is our own take on the
 * kind of effect a well-known ambient pedal makes (near-miss names, personal use): a granular looper (Mikrocosm),
 * a micro-looper into delay and reverb (Moodd MK2), a granular delay (Partikle), a tape time-stretcher (Tensr), a
 * sequenced-filter shimmer reverb (NiteSky), a swelling ensemble reverb (Cloudbrst), cascading modulated delays
 * (Polym00n), a three-voice pitched rhythmic delay (Hedr4), a swarm of short delays (Aftrneath), a warbling,
 * badly-tracking pitch shifter (Rainbow Mchn) and a square-wave synth that follows your notes with subharmonics
 * (Data Korrupt). A machine has eight knobs of its own (names, ranges: MACHINES[]); Hold freezes what it has.
 * Building blocks (lines, filters, FDN reverb, pitch tracker) from vocal-mpc/src/vocal_core.h. Our own code.
 * Safety, as in every plugin here: input cleaned, reset if the output goes bad or past +36 dB, output capped at
 * +12 dB (one NaN reaching MPC's mixer silences it until restart). */
#include "vocal_core.h"   /* (and tools/picklist.h) */

#define NK 8
/* ==== knob meanings (as in Duo FX) ========================================================================= */
enum { M_LIN, M_EXP, M_INT, M_CHOICE };
typedef struct { const char *name; int map; float lo, hi, def; const char *unit; const char *const *choices; } Knob;
#define KLIN(n, lo, hi, d, u) {n, M_LIN, lo, hi, d, u, NULL}
#define KEXP(n, lo, hi, d, u) {n, M_EXP, lo, hi, d, u, NULL}
#define KINT(n, lo, hi, d, u) {n, M_INT, lo, hi, d, u, NULL}
#define KCHO(n, list, d) {n, M_CHOICE, 0, (float)(sizeof list / sizeof *list - 1), d, "", list}
#define KPCT(n, d) KLIN(n, 0, 100, d, "%")
#define KMS(n, lo, hi, d) KEXP(n, lo, hi, d, "ms")
#define KHZ(n, lo, hi, d) KEXP(n, lo, hi, d, "Hz")
#define KST(n, d) KINT(n, -24, 24, d, "st")
#define KNONE {NULL, 0, 0, 0, 0, NULL, NULL}

static const char *const MK_PITCH[] = {"Unison", "Oct Up", "Oct Down", "Fifth", "Oct+Fifth", "Random"};
static const char *const MOOD_SPEED[] = {"-2x", "-1x", "-0.5x", "0.5x", "1x", "2x"};
static const float MOOD_SPEED_V[] = {-2, -1, -0.5f, 0.5f, 1, 2};
static const char *const SHIM_PITCH[] = {"+12", "+7", "+5", "+19", "-12"};
static const float SHIM_ST[] = {12, 7, 5, 19, -12};
static const char *const RHYTHMS[] = {"Even", "Triplet", "Dotted", "Swing", "Cascade"};
static const char *const SECOND[] = {"Off", "Oct Down", "Unison", "Fifth", "Oct Up"};
static const float SECOND_ST[] = {0, -12, 0, 7, 12};
static const char *const SUBS[] = {"/2", "/3", "/4", "/5"};

enum { MK_MICRO, MK_MOOD, MK_PARTICLE, MK_TENSOR, MK_NIGHT, MK_CLOUD, MK_POLY, MK_HEDRA, MK_AFTER, MK_RAINBOW, MK_DATA, kNumMachines };
typedef struct { const char *name; float mix; const char *about; Knob k[NK]; } Machine;
static const Machine MACHINES[kNumMachines] = {
    [MK_MICRO] = {"Mikrocosm", 50, "Granular looper: grains, reversed and pitched, into clouds",
        {KEXP("Activity", 1, 40, 8, "/s"), KMS("Grain", 20, 500, 120), KEXP("Time", 0.3f, 8, 3, "s"), KCHO("Pitch", MK_PITCH, 0),
         KPCT("Reverse", 30), KPCT("Spread", 70), KPCT("Space", 40), KPCT("Tone", 60)}},
    [MK_MOOD] = {"Moodd MK2", 50, "A micro-looper of the last seconds, into delay and reverb",
        {KEXP("Length", 0.25f, 4, 1.5f, "s"), KCHO("Speed", MOOD_SPEED, 4), KPCT("Smear", 30), KPCT("Loop", 60),
         KMS("Time", 50, 1500, 400), KPCT("Feedback", 40), KPCT("Reverb", 40), KPCT("Tone", 55)}},
    [MK_PARTICLE] = {"Partikle", 45, "Granular delay: pitched, reversed, scattered grains",
        {KMS("Delay", 20, 2000, 400), KMS("Grain", 10, 300, 80), KEXP("Density", 1, 40, 12, "/s"), KST("Pitch", 0),
         KPCT("Reverse", 20), KPCT("Spread", 30), KPCT("Feedback", 30), KPCT("Tone", 60)}},
    [MK_TENSOR] = {"Tensr", 60, "Tape time: varispeed, reverse, stretch, stutter",
        {KLIN("Speed", -2, 2, 1, "x"), KLIN("Stretch", 0.25f, 1, 1, "x"), KHZ("Stutter", 0.5f, 16, 4), KPCT("Chance", 0),
         KEXP("Length", 0.2f, 4, 1, "s"), KPCT("Wow", 10), KST("Pitch", 0), KPCT("Tone", 60)}},
    [MK_NIGHT] = {"NiteSky", 45, "Shimmer reverb through a sequenced, resonant filter",
        {KEXP("Decay", 1, 30, 6, "s"), KPCT("Size", 70), KPCT("Shimmer", 30), KCHO("Pitch", SHIM_PITCH, 0),
         KHZ("Filter", 200, 8000, 1500), KPCT("Reso", 40), KPCT("Seq Depth", 50), KHZ("Seq Rate", 0.1f, 8, 1)}},
    [MK_CLOUD] = {"Cloudbrst", 45, "Huge ensemble reverb that swells into pads",
        {KEXP("Decay", 1, 60, 12, "s"), KPCT("Size", 80), KPCT("Ensemble", 50), KPCT("Swell", 30), KMS("Predelay", 1, 500, 30),
         KPCT("Tone", 55), KLIN("Low", -12, 6, 0, "dB"), KPCT("Mod", 40)}},
    [MK_POLY] = {"Polym00n", 45, "Cascading modulated delays, phaser and flanger in repeats",
        {KMS("Time", 20, 1500, 380), KPCT("Feedback", 45), KINT("Dimension", 1, 6, 3, ""), KPCT("Depth", 40), KHZ("Rate", 0.05f, 8, 0.6f),
         KPCT("Phaser", 40), KPCT("Flanger", 30), KPCT("Tone", 60)}},
    [MK_HEDRA] = {"Hedr4", 45, "Three pitch-shifted voices in a rhythmic delay",
        {KST("Pitch 1", 7), KST("Pitch 2", 12), KST("Pitch 3", -12), KMS("Time", 20, 2000, 450), KCHO("Rhythm", RHYTHMS, 0),
         KPCT("Feedback", 30), KPCT("Spread", 70), KPCT("Tone", 60)}},
    [MK_AFTER] = {"Aftrneath", 50, "A swarm of short delays smeared into a strange reverb",
        {KMS("Length", 20, 400, 150), KPCT("Diffuse", 60), KPCT("Drag", 30), KPCT("Reflect", 50), KPCT("Mod", 30),
         KPCT("Tone", 50), KPCT("Width", 80), KINT("Density", 3, 8, 8, "")}},
    [MK_RAINBOW] = {"Rainbow Mchn", 50, "A pitch shifter that warbles, smears, tracks badly",
        {KST("Pitch", 12), KCHO("Second", SECOND, 0), KPCT("Tracking", 50), KPCT("Magic", 0), KPCT("Mod Depth", 20),
         KHZ("Mod Rate", 0.1f, 10, 3), KPCT("Tone", 60), KNONE}},
    [MK_DATA] = {"Data Korrupt", 60, "Square-wave synth on your notes, with subharmonics",
        {KST("Interval", 0), KCHO("Sub", SUBS, 0), KPCT("Square", 70), KPCT("Sub Level", 50), KPCT("Fuzz", 30),
         KMS("Glide", 1, 300, 20), KPCT("Vibrato", 0), KPCT("Tone", 50)}},
};

/* presets: a machine and its knobs (in their units), Mix */
typedef struct { const char *name; int machine; float mix; float k[NK]; } Preset;
static const Preset PRESETS[] = {
    {"Cloud Bank", MK_MICRO, 50, {8, 120, 3, 0, 30, 70, 40, 60}},
    {"Reverse Swarm", MK_MICRO, 55, {20, 200, 4, 0, 90, 90, 60, 50}},
    {"Octave Mist", MK_MICRO, 55, {12, 250, 5, 1, 30, 80, 70, 70}},
    {"Glitch Rain", MK_MICRO, 50, {35, 30, 1.5f, 5, 50, 100, 25, 75}},
    {"Slow Mood", MK_MOOD, 50, {2, 3, 40, 60, 600, 45, 50, 50}},
    {"Backwards Day", MK_MOOD, 55, {1.5f, 1, 20, 70, 400, 35, 45, 55}},
    {"Double Time", MK_MOOD, 50, {0.8f, 5, 10, 60, 250, 30, 30, 60}},
    {"Particles", MK_PARTICLE, 45, {400, 80, 12, 0, 20, 30, 30, 60}},
    {"Up Sparks", MK_PARTICLE, 45, {600, 60, 20, 12, 10, 60, 45, 75}},
    {"Dust Down", MK_PARTICLE, 50, {900, 150, 8, -12, 50, 50, 40, 40}},
    {"Half Speed", MK_TENSOR, 60, {0.5f, 1, 4, 0, 1.5f, 15, 0, 55}},
    {"Reverse Tape", MK_TENSOR, 60, {-1, 1, 4, 0, 2, 20, 0, 55}},
    {"Stretched", MK_TENSOR, 60, {1, 0.25f, 4, 0, 3, 10, 0, 60}},
    {"Stutters", MK_TENSOR, 60, {1, 1, 8, 60, 1, 5, 0, 65}},
    {"Night Shimmer", MK_NIGHT, 45, {8, 75, 40, 0, 1500, 40, 50, 1}},
    {"Star Steps", MK_NIGHT, 50, {6, 60, 25, 1, 900, 70, 90, 3}},
    {"Deep Night", MK_NIGHT, 50, {15, 90, 30, 4, 600, 30, 30, 0.3f}},
    {"Cloud Pad", MK_CLOUD, 45, {12, 80, 50, 30, 30, 55, 0, 40}},
    {"Infinite Swell", MK_CLOUD, 55, {40, 95, 70, 80, 80, 50, 2, 50}},
    {"Bright Air", MK_CLOUD, 40, {6, 60, 40, 10, 15, 80, -3, 30}},
    {"Moon Echo", MK_POLY, 45, {380, 45, 3, 40, 0.6f, 40, 30, 60}},
    {"Phase Cascade", MK_POLY, 45, {500, 60, 5, 30, 0.3f, 80, 10, 55}},
    {"Jet Repeats", MK_POLY, 40, {300, 50, 2, 50, 1.2f, 10, 80, 65}},
    {"Fifth Steps", MK_HEDRA, 45, {7, 12, -12, 450, 0, 30, 70, 60}},
    {"Triplet Harmony", MK_HEDRA, 45, {4, 7, 12, 400, 1, 25, 80, 60}},
    {"Pitch Stairs", MK_HEDRA, 45, {5, 5, 5, 350, 4, 35, 60, 55}},
    {"Underneath", MK_AFTER, 50, {150, 60, 30, 50, 30, 50, 80, 8}},
    {"Long Drag", MK_AFTER, 55, {350, 70, 90, 60, 40, 45, 100, 8}},
    {"Small Swarm", MK_AFTER, 45, {40, 40, 20, 40, 20, 60, 60, 5}},
    {"Rainbow Up", MK_RAINBOW, 50, {12, 0, 50, 0, 20, 3, 60, 0}},
    {"Magic Rise", MK_RAINBOW, 50, {7, 0, 30, 70, 15, 2, 55, 0}},
    {"Broken Octaves", MK_RAINBOW, 55, {12, 1, 100, 20, 40, 6, 50, 0}},
    {"Corrupt Sub", MK_DATA, 60, {0, 0, 70, 50, 30, 20, 0, 50}},
    {"Square Lead", MK_DATA, 70, {12, 0, 90, 20, 10, 40, 30, 70}},
    {"Deep Divide", MK_DATA, 70, {-12, 2, 50, 80, 20, 10, 0, 35}},
};
#define NPRESETS ((int)(sizeof PRESETS / sizeof *PRESETS))

enum { kMachine, kMachineOpen, kMix, kK, kHold = kK + NK, kPreset, kPresetPrev, kPresetNext, kOutput, kNumParams };

static float knob_value(const Knob *k, float n) {
    n = clampf(n, 0, 1);
    switch (k->map) {
    case M_EXP: return expmap(k->lo, k->hi, n);
    case M_INT: case M_CHOICE: return roundf(lerp(k->lo, k->hi, n));
    default: return lerp(k->lo, k->hi, n);
    }
}
static float knob_norm(const Knob *k, float v) {
    if (k->map == M_EXP) return clampf(logf(v / k->lo) / logf(k->hi / k->lo), 0, 1);
    return k->hi > k->lo ? clampf((v - k->lo) / (k->hi - k->lo), 0, 1) : 0;
}

/* ==== building blocks ===================================================================================== */
typedef struct { float ic1, ic2; } Svf;   /* TPT state-variable filter */
static float svf_lp(Svf *f, float x, float g, float k) {
    const float a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const float v3 = x - f->ic2, v1 = a1 * f->ic1 + a2 * v3, v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
    f->ic1 = 2 * v1 - f->ic1; f->ic2 = 2 * v2 - f->ic2;
    return v2;
}
/* a plain two-grain pitch shifter (50 ms grains); ratio > 1 up */
typedef struct { Line l; float ph; } Shift;
static float hann01(float p) { const float s = sinf(PI * p); return s * s; }
static float shift(Shift *s, float x, float ratio, float win) {
    line_push(&s->l, x);
    s->ph += (1 - ratio) / win;
    s->ph -= floorf(s->ph);
    float p2 = s->ph + 0.5f;
    p2 -= floorf(p2);
    return line_read(&s->l, s->ph * win + 2) * hann01(s->ph) + line_read(&s->l, p2 * win + 2) * hann01(p2);
}
/* grains reading the recording: d = samples ago; it moves by (recording ? 1 : 0) - rate each sample */
#define NGR 32
typedef struct { float d, rate, len, age, pan, gain; int on; } Grain;
typedef struct { float ph; } Lfo;
static float sine01(float ph) { return sinf(2 * PI * ph); }

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    int ready, machine, preset;
    volatile int update_display;
    volatile uint32_t automate;
    Line rec[2], dl[2], dl2[2], ap[2][6];
    int apd[2][6];
    Verb verb;
    Shift sh[2][3];
    Grain gr[NGR];
    Tracker trk;
    OnePole op[2][8];
    Svf svf[2];
    float st[2][16], g[24], lfo[6], timer, mix_s, out_s, env, seq[8];
    float v[NK];   /* the knobs in their units (worked out every 16 samples) */
    int tick, hold;
    float dly_s[8];
} Plugin;

static void reset(Plugin *s) {
    for (int c = 0; c < 2; c++) {
        line_clear(&s->rec[c]); line_clear(&s->dl[c]); line_clear(&s->dl2[c]);
        for (int k = 0; k < 6; k++) line_clear(&s->ap[c][k]);
        for (int k = 0; k < 3; k++) { line_clear(&s->sh[c][k].l); s->sh[c][k].ph = 0; }
        memset(s->op[c], 0, sizeof s->op[c]);
        memset(s->st[c], 0, sizeof s->st[c]);
        s->svf[c].ic1 = s->svf[c].ic2 = 0;
    }
    verb_clear(&s->verb);
    memset(s->gr, 0, sizeof s->gr);
    memset(&s->trk, 0, sizeof s->trk);
    memset(s->lfo, 0, sizeof s->lfo);
    s->timer = 0; s->env = 0;
    for (int k = 0; k < 8; k++) s->dly_s[k] = 0;
}
static void free_all(Plugin *s) {
    for (int c = 0; c < 2; c++) {
        line_free(&s->rec[c]); line_free(&s->dl[c]); line_free(&s->dl2[c]);
        for (int k = 0; k < 6; k++) line_free(&s->ap[c][k]);
        for (int k = 0; k < 3; k++) line_free(&s->sh[c][k].l);
    }
    verb_free(&s->verb);
}
static int setup(Plugin *s, float fs) {
    s->ready = 0;
    free_all(s);
    s->fs = fs;
    int ok = verb_init(&s->verb, fs * 1.0f);
    for (int c = 0; c < 2; c++) {
        ok &= line_init(&s->rec[c], (int)(fs * 10));
        ok &= line_init(&s->dl[c], (int)(fs * 3));
        ok &= line_init(&s->dl2[c], (int)(fs * 0.6f));
        for (int k = 0; k < 6; k++) ok &= line_init(&s->ap[c][k], (int)(fs * 0.12f));
        for (int k = 0; k < 3; k++) ok &= line_init(&s->sh[c][k].l, (int)(fs * 0.12f));
    }
    reset(s);
    s->ready = ok;
    return ok;
}

static void spawn(Plugin *s, float d, float rate, float len, float pan, float gain) {
    for (int i = 0; i < NGR; i++)
        if (!s->gr[i].on) { s->gr[i] = (Grain){d, rate, len, 0, pan, gain, 1}; return; }
}
static void run_grains(Plugin *s, int recording, float *ol, float *or_) {
    float l = 0, r = 0;
    const float maxd = (float)s->rec[0].mask - 4;
    for (int i = 0; i < NGR; i++) {
        Grain *g = &s->gr[i];
        if (!g->on) continue;
        const float w = hann01(g->age / g->len) * g->gain;
        const float d = clampf(g->d, 1, maxd);
        l += line_read(&s->rec[0], d) * w * (1 - g->pan);
        r += line_read(&s->rec[1], d) * w * (1 + g->pan);
        g->d += (recording ? 1.0f : 0.0f) - g->rate;
        if (++g->age >= g->len || g->d < 1 || g->d > maxd) g->on = 0;
    }
    *ol = l; *or_ = r;
}
static float allpass(Line *l, int d, float x, float g) {
    const float v = line_tap(l, d - 1), u = x + g * v;
    line_push(l, u);
    return v - g * u;
}

/* per-machine values that don't change every sample */
static void control(Plugin *s) {
    const Machine *m = &MACHINES[s->machine];
    for (int i = 0; i < NK; i++) s->v[i] = m->k[i].name ? knob_value(&m->k[i], s->norm[kK + i]) : 0;
    const float fs = s->fs, *v = s->v;
    float *g = s->g;
    switch (s->machine) {
    case MK_MICRO: verb_set(&s->verb, 1.6f, s->hold ? 60 : 4, 9000, fs); g[0] = lp_coef(expmap(1500, 16000, v[7] * 0.01f), fs); break;
    case MK_MOOD: verb_set(&s->verb, 1.4f, s->hold ? 60 : 3.5f, 8000, fs); g[0] = lp_coef(expmap(1500, 16000, v[7] * 0.01f), fs); break;
    case MK_PARTICLE: case MK_TENSOR: g[0] = lp_coef(expmap(1500, 16000, v[7] * 0.01f), fs); break;
    case MK_NIGHT: {
        const float size = lerp(0.8f, 2.0f, v[1] * 0.01f), rt = s->hold ? 1000 : v[0];
        verb_set(&s->verb, size, rt, 7000, fs);
        /* the shimmer goes round the reverb again: scaled by what the reverb loses each pass, it can't run away */
        g[2] = 1 - powf(10, -3 * 0.0474f * size / rt);
        break;
    }
    case MK_CLOUD: verb_set(&s->verb, lerp(0.9f, 2.0f, v[1] * 0.01f), s->hold ? 1000 : v[0], expmap(2000, 14000, v[5] * 0.01f), fs);
        g[0] = lp_coef(200, fs); g[1] = db(v[6]) - 1; break;
    case MK_POLY: case MK_HEDRA: case MK_DATA: g[0] = lp_coef(expmap(1000, 16000, v[7] * 0.01f), fs); g[1] = lp_coef(25, fs); break;   /* Tone: knob 8 */
    case MK_AFTER: g[0] = lp_coef(expmap(1000, 16000, v[5] * 0.01f), fs); break;                               /* knob 6 */
    case MK_RAINBOW: g[0] = lp_coef(expmap(1000, 16000, v[6] * 0.01f), fs); break;                             /* knob 7 */
    default: break;
    }
    if (s->machine == MK_AFTER) {   /* the swarm's diffusers */
        for (int c = 0; c < 2; c++) for (int k = 0; k < 6; k++) {
            static const float MS[6] = {3.1f, 4.7f, 7.3f, 11.3f, 17.9f, 23.1f};
            s->apd[c][k] = (int)(MS[k] * (c ? 1.11f : 1) * fs / 1000 * lerp(0.6f, 2.5f, v[0] / 400));
            if (s->apd[c][k] < 2) s->apd[c][k] = 2;
            if (s->apd[c][k] > s->ap[c][k].mask - 2) s->apd[c][k] = s->ap[c][k].mask - 2;
        }
    }
}

static void run_machine(Plugin *s, const float in[2], float out[2]) {
    const float fs = s->fs, ms = fs * 0.001f, *v = s->v, *g = s->g;
    const int hold = s->hold;
    switch (s->machine) {
    case MK_MICRO: {
        if (!hold) { line_push(&s->rec[0], in[0]); line_push(&s->rec[1], in[1]); }
        const float glen = v[1] * ms, window = v[2] * fs;
        s->timer -= v[0] / fs;
        if (s->timer <= 0) {
            s->timer += 0.6f + 0.8f * rnd();
            static const float R[5] = {1, 2, 0.5f, 1.5f, 3};
            const int pc = (int)v[3];
            const float ratio = pc == 5 ? R[(int)(rnd() * 4.99f)] : R[pc];
            const float rate = rnd() * 100 < v[4] ? -ratio : ratio;
            const float d = glen * (rate > 1 ? rate : 1) + 4 + rnd() * window;
            spawn(s, d, rate, glen, (rnd() * 2 - 1) * v[5] * 0.01f, 1 / sqrtf(1 + v[0] * v[1] * 0.001f));
        }
        float l, r, vl, vr;
        run_grains(s, !hold, &l, &r);
        l = lp(&s->op[0][0], l, g[0]); r = lp(&s->op[1][0], r, g[0]);
        verb_run(&s->verb, l, r, &vl, &vr);
        const float sp = v[6] * 0.01f;
        out[0] = (l * (1 - 0.5f * sp) + vl * sp * 1.4f) * 1.8f;
        out[1] = (r * (1 - 0.5f * sp) + vr * sp * 1.4f) * 1.8f;
        break;
    }
    case MK_MOOD: {
        if (!hold) { line_push(&s->rec[0], in[0]); line_push(&s->rec[1], in[1]); }
        const float L = v[0] * fs, speed = MOOD_SPEED_V[(int)v[1]], glen = (0.08f + 0.25f * v[2] * 0.01f) * fs;
        /* the loop's phase runs at the speed; overlapping grains read from it, smeared by Smear */
        s->st[0][0] += fabsf(speed) / L;
        s->st[0][0] -= floorf(s->st[0][0]);
        s->timer -= 1;
        if (s->timer <= 0) {
            s->timer = glen * 0.5f;
            const float ph = speed > 0 ? s->st[0][0] : 1 - s->st[0][0];
            const float d = (1 - ph) * L + glen * (fabsf(speed) > 1 ? fabsf(speed) : 1) + 4 + rnd() * v[2] * 0.01f * 0.3f * L;
            spawn(s, d, speed, glen, (rnd() - 0.5f) * 0.4f, 0.75f);
        }
        float l, r;
        run_grains(s, !hold, &l, &r);
        const float lp_amt = v[3] * 0.01f;
        const float a = in[0] * (1 - lp_amt * 0.5f) + l * lp_amt, b = in[1] * (1 - lp_amt * 0.5f) + r * lp_amt;
        /* the wet side: a delay (feedback crossing over) and a reverb */
        const float t = v[4] * ms;
        s->dly_s[0] += 0.0005f * (t - s->dly_s[0]);
        const float dl = line_read(&s->dl[0], s->dly_s[0]), dr = line_read(&s->dl[1], s->dly_s[0] * 1.03f);
        line_push(&s->dl[0], a + tanh_fast(lp(&s->op[0][1], dr, g[0]) * v[5] * 0.0095f));
        line_push(&s->dl[1], b + tanh_fast(lp(&s->op[1][1], dl, g[0]) * v[5] * 0.0095f));
        float vl, vr;
        verb_run(&s->verb, a + dl * 0.5f, b + dr * 0.5f, &vl, &vr);
        out[0] = a + dl * 0.6f + vl * v[6] * 0.014f;
        out[1] = b + dr * 0.6f + vr * v[6] * 0.014f;
        break;
    }
    case MK_PARTICLE: {
        const float fb = v[6] * 0.009f;
        if (!hold) {
            line_push(&s->rec[0], in[0] + tanh_fast(s->st[0][1] * fb));
            line_push(&s->rec[1], in[1] + tanh_fast(s->st[1][1] * fb));
        }
        const float glen = v[1] * ms, ratio = exp2f(v[3] / 12);
        s->timer -= v[2] / fs;
        if (s->timer <= 0) {
            s->timer += 0.5f + rnd();
            const float rate = rnd() * 100 < v[4] ? -ratio : ratio;
            const float d = v[0] * ms * (1 + (rnd() - 0.5f) * v[5] * 0.012f) + glen * (rate > 1 ? rate : 1) + 4;
            spawn(s, d, rate, glen, (rnd() * 2 - 1) * 0.7f, 1 / sqrtf(1 + v[2] * v[1] * 0.001f));
        }
        float l, r;
        run_grains(s, !hold, &l, &r);
        l = lp(&s->op[0][0], l, g[0]); r = lp(&s->op[1][0], r, g[0]);
        s->st[0][1] = l; s->st[1][1] = r;
        out[0] = l; out[1] = r;
        break;
    }
    case MK_TENSOR: {
        if (!hold) { line_push(&s->rec[0], in[0]); line_push(&s->rec[1], in[1]); }
        const float L = v[4] * fs, glen = 0.07f * fs, speed = v[0], stretch = v[1];
        const float wow = 1 + v[5] * 0.0002f * (sine01(s->lfo[0]) + 0.3f * sine01(s->lfo[1]));
        s->lfo[0] += 0.6f / fs; s->lfo[0] -= floorf(s->lfo[0]);
        s->lfo[1] += 7.0f / fs; s->lfo[1] -= floorf(s->lfo[1]);
        /* the tape head: moves through the recent past at Speed x Stretch (the grains play at Speed x Pitch) */
        float *head = &s->st[0][2];
        if (*head < glen * 3) *head = glen * 3;
        *head += (hold ? 0.0f : 1.0f) - speed * stretch;
        if (*head > L) *head = glen * 3 + 4;
        if (*head < glen * 3) *head = L;
        /* stutter: now and then the head comes back to where it was a moment ago, a few times over */
        s->st[0][3] -= v[2] / fs;
        if (s->st[0][3] <= 0) {
            s->st[0][3] += 1;
            if (rnd() * 100 < v[3]) { s->st[0][4] = *head; s->st[0][5] = 1; } else s->st[0][5] = 0;
        }
        const float d0 = s->st[0][5] > 0.5f ? s->st[0][4] : *head;
        s->timer -= 1;
        if (s->timer <= 0) {
            s->timer = glen * 0.5f;
            const float rate = speed * exp2f(v[6] / 12) * wow;
            spawn(s, d0 + glen * (rate > 1 ? rate : 1), rate, glen, 0, 0.7f);
        }
        float l, r;
        run_grains(s, !hold, &l, &r);
        out[0] = lp(&s->op[0][0], l, g[0]); out[1] = lp(&s->op[1][0], r, g[0]);
        break;
    }
    case MK_NIGHT: {
        /* the step sequencer moves the filter in octaves; the shimmer pitch-shifts the tail back into itself */
        s->lfo[0] += v[7] / fs;
        if (s->lfo[0] >= 1) { s->lfo[0] -= 1; s->st[0][6] = (float)(((int)s->st[0][6] + 1) & 7); }
        const float step = s->seq[(int)s->st[0][6]] * v[6] * 0.01f * 2;   /* +-2 octaves */
        s->st[0][7] += 0.004f * (step - s->st[0][7]);
        const float fc = clampf(v[4] * exp2f(s->st[0][7]), 60, fs * 0.4f), gg = tanf(PI * fc / fs), k = 2 - 1.9f * v[5] * 0.01f;
        const float shim = v[2] * 0.01f;
        float x[2];
        for (int c = 0; c < 2; c++) {
            const float sh_in = shift(&s->sh[c][0], s->st[c][8], exp2f(SHIM_ST[(int)v[3]] / 12) * (c ? 1.003f : 1), 0.05f * fs);
            x[c] = svf_lp(&s->svf[c], in[c] * (hold ? 0.02f : 1) + tanh_fast(sh_in * shim * 0.9f * g[2] * 4), gg, k) * (0.3f + 0.35f * k);
        }
        float vl, vr;
        verb_run(&s->verb, x[0], x[1], &vl, &vr);
        s->st[0][8] = vl; s->st[1][8] = vr;
        out[0] = vl * 2.5f; out[1] = vr * 2.5f;
        break;
    }
    case MK_CLOUD: {
        /* swell: the reverb hears less while you play, so it blooms after; then an ensemble over the tail */
        const float lvl = fmaxf(fabsf(in[0]), fabsf(in[1]));
        s->env += (lvl > s->env ? 0.005f : 0.0002f) * (lvl - s->env);
        const float gin = (hold ? 0.02f : 1) / (1 + v[3] * 0.01f * s->env * 20);
        float x[2] = {0, 0};
        for (int c = 0; c < 2; c++) { line_push(&s->dl[c], in[c] * gin); x[c] = line_read(&s->dl[c], v[4] * ms); }
        float vl, vr;
        verb_run(&s->verb, x[0], x[1], &vl, &vr);
        vl += lp(&s->op[0][1], vl, g[0]) * g[1];
        vr += lp(&s->op[1][1], vr, g[0]) * g[1];
        float y[2] = {vl, vr};
        const float ens = v[2] * 0.01f, mod = v[7] * 0.01f;
        for (int c = 0; c < 2; c++) {
            line_push(&s->dl2[c], y[c]);
            float e = 0;
            for (int k = 0; k < 3; k++) {
                const float ph = s->lfo[k] + (float)c * 0.25f;
                e += line_read(&s->dl2[c], (8 + 4 * (float)k + (2 + 4 * mod) * (1 + sine01(ph))) * ms);
            }
            out[c] = y[c] * (1 - 0.5f * ens) + e * 0.33f * ens;
        }
        for (int k = 0; k < 3; k++) { s->lfo[k] += (0.13f + 0.07f * (float)k) * (0.5f + mod) / fs; s->lfo[k] -= floorf(s->lfo[k]); }
        break;
    }
    case MK_POLY: {
        const int dim = (int)v[2];
        const float t = v[0] * ms, depth = v[3] * 0.01f * 0.004f * fs;
        s->lfo[0] += v[4] / fs; s->lfo[0] -= floorf(s->lfo[0]);
        s->lfo[1] += v[4] * 0.37f / fs; s->lfo[1] -= floorf(s->lfo[1]);
        s->dly_s[0] += 0.0005f * (t - s->dly_s[0]);
        for (int c = 0; c < 2; c++) {
            float sum = 0;
            for (int k = 1; k <= dim; k++) {   /* the repeat spread into Dimension taps, each swaying its own way */
                const float d = s->dly_s[0] * (0.4f + 0.6f * (float)k / (float)dim) + depth * (1 + sine01(s->lfo[0] + 0.17f * (float)k + 0.25f * (float)c));
                sum += line_read(&s->dl[c], d);
            }
            const float fbsum = sum / (float)dim;   /* (fed back: the taps can add up in phase, at the lows most) */
            sum /= sqrtf((float)dim);
            /* a phaser (four swept allpasses) and a flanger (a short swept delay) inside the repeats */
            const float a = lerp(-0.2f, 0.85f, 0.5f + 0.5f * sine01(s->lfo[1] + 0.3f * (float)c));
            float ph2 = fbsum;
            for (int k = 0; k < 4; k++) { const float yy = -a * ph2 + s->st[c][k]; s->st[c][k] = ph2 + a * yy; ph2 = yy; }
            float y = lerp(fbsum, (fbsum + ph2) * 0.5f, v[5] * 0.01f);   /* (both mixes peak at unity: the loop stays under one) */
            line_push(&s->dl2[c], y);
            const float fl = line_read(&s->dl2[c], (0.5f + 3 * (1 + sine01(s->lfo[1] * 0.7f + 0.5f * (float)c))) * ms);
            y = lerp(y, (y + fl) * 0.5f, v[6] * 0.01f);
            y = lp(&s->op[c][0], y, g[0]);
            y -= lp(&s->op[c][1], y, g[1]);   /* (no DC round the loop) */
            line_push(&s->dl[c], in[c] + tanh_fast(y * v[1] * 0.0097f));
            out[c] = sum;
        }
        break;
    }
    case MK_HEDRA: {
        static const float RH[5][3] = {{1, 2, 3}, {0.6667f, 1.3333f, 2}, {0.75f, 1.5f, 2.25f}, {1, 1.6667f, 2.6667f}, {1, 2, 3}};
        const int rh = (int)v[4];
        const float t = v[3] * ms, mono = (in[0] + in[1]) * 0.5f;
        s->dly_s[0] += 0.0005f * (t - s->dly_s[0]);
        line_push(&s->dl[0], mono + tanh_fast(s->st[0][9] * v[5] * 0.009f));
        float l = 0, r = 0, fbs = 0, cum = 0;
        for (int k = 0; k < 3; k++) {
            const float d = clampf(s->dly_s[0] * RH[rh][k], 2, (float)s->dl[0].mask - 4);
            cum = rh == 4 ? cum + v[k] : v[k];   /* Cascade: each voice stacks on the last */
            const float y = shift(&s->sh[0][k], line_read(&s->dl[0], d), exp2f(clampf(cum, -36, 36) / 12), 0.05f * fs);
            const float pan = (k == 0 ? -1.0f : k == 1 ? 1.0f : 0.0f) * v[6] * 0.01f;
            l += y * (1 - pan) * 0.6f; r += y * (1 + pan) * 0.6f;
            fbs += y;
        }
        s->st[0][9] = lp(&s->op[0][0], fbs * 0.4f, g[0]);
        out[0] = lp(&s->op[0][1], l, g[0]); out[1] = lp(&s->op[1][1], r, g[0]);
        break;
    }
    case MK_AFTER: {
        const int n = (int)v[7];
        const float span = v[0] * ms, drag = v[2] * 0.01f, mod = v[4] * 0.01f;
        s->lfo[0] += 0.23f / fs; s->lfo[0] -= floorf(s->lfo[0]);
        for (int c = 0; c < 2; c++) {
            float sum = 0, wsum = 0;
            for (int k = 0; k < n; k++) {   /* the swarm: taps over the span, bunched by Drag, wandering by Mod */
                const float pos = (float)(k + 1) / (float)n, bunched = powf(pos, 1 + 2.5f * drag);
                const float d = 1 + span * bunched * (c ? 0.93f : 1) + mod * 0.003f * fs * (1 + sine01(s->lfo[0] + 0.13f * (float)k + 0.4f * (float)c));
                sum += line_read(&s->dl2[c], clampf(d, 1, (float)s->dl2[c].mask - 4)) * (1 - 0.5f * pos);
                wsum += 1 - 0.5f * pos;
            }
            sum /= wsum;   /* (the taps' weights sum to one: the loop's gain is Reflect at most) */
            float y = sum;
            for (int k = 0; k < 6; k++) y = allpass(&s->ap[c][k], s->apd[c][k], y, v[1] * 0.0072f);
            y = lp(&s->op[c][0], y, g[0]);
            line_push(&s->dl2[c], in[c] + tanh_fast(y * v[3] * 0.0094f));
            s->st[c][10] = y;
        }
        const float w = v[6] * 0.01f, mid = (s->st[0][10] + s->st[1][10]) * 0.5f;
        out[0] = lerp(mid, s->st[0][10], w) * 5; out[1] = lerp(mid, s->st[1][10], w) * 5;
        break;
    }
    case MK_RAINBOW: {
        /* tracking: the pitch wanders to a new wrong place every so often, by up to a semitone */
        s->st[0][11] -= 1;
        if (s->st[0][11] <= 0) { s->st[0][11] = (0.05f + 0.25f * rnd()) * fs; s->st[0][12] = (rnd() * 2 - 1) * v[2] * 0.01f; }
        s->st[0][13] += 0.0008f * (s->st[0][12] - s->st[0][13]);
        s->lfo[0] += v[5] / fs; s->lfo[0] -= floorf(s->lfo[0]);
        const float cents = s->st[0][13] * 100 + v[4] * 0.6f * sine01(s->lfo[0]);
        const float ratio = exp2f((v[0] + cents * 0.01f) / 12), r2 = exp2f((SECOND_ST[(int)v[1]] + cents * 0.007f) / 12);
        for (int c = 0; c < 2; c++) {
            /* Magic: the shifted sound shifted again (two voices add up, so less of it goes round then) */
            const float fb = line_read(&s->dl[c], 0.09f * fs) * v[3] * 0.0085f / ((int)v[1] > 0 ? 1.4f : 1.0f);
            const float x = in[c] + tanh_fast(fb);
            float y = shift(&s->sh[c][0], x, ratio * (c ? 1.002f : 1), 0.045f * fs);
            if ((int)v[1] > 0) y = y * 0.7f + shift(&s->sh[c][1], x, r2, 0.05f * fs) * 0.6f;
            y = lp(&s->op[c][0], y, g[0]);
            line_push(&s->dl[c], y);
            out[c] = y;
        }
        break;
    }
    case MK_DATA: {
        const float mono = (in[0] + in[1]) * 0.5f;
        track(&s->trk, mono, fs);
        const float a = fabsf(mono);
        s->env += (a > s->env ? 0.01f : 0.0004f) * (a - s->env);
        const int voiced = s->trk.conf > 0.7f && s->trk.hz > 40 && s->trk.hz < 2000;
        const float target = voiced ? s->trk.hz * exp2f(v[0] / 12) : s->st[0][14];
        float *f = &s->st[0][14];
        if (*f <= 0) *f = target > 0 ? target : 110;
        *f += (1 - expf(-1 / (v[5] * ms))) * (target - *f);
        s->lfo[0] += 5.5f / fs; s->lfo[0] -= floorf(s->lfo[0]);
        const float hz = *f * exp2f(v[6] * 0.005f * sine01(s->lfo[0]) / 12);
        s->lfo[1] += hz / fs; s->lfo[1] -= floorf(s->lfo[1]);
        s->lfo[2] += hz / ((float)v[1] + 2) / fs; s->lfo[2] -= floorf(s->lfo[2]);   /* the subharmonic: /2 .. /5 */
        const float sq = s->lfo[1] < 0.5f ? 1.0f : -1.0f, sub = s->lfo[2] < 0.5f ? 1.0f : -1.0f;
        s->st[0][15] += 0.003f * ((voiced ? 1.0f : 0.0f) - s->st[0][15]);
        const float gate = s->st[0][15] * clampf(s->env * 6, 0, 1);
        const float synth = (sq * v[2] + sub * v[3]) * 0.01f * gate * 0.5f;
        for (int c = 0; c < 2; c++) {
            const float fz = tanh_fast(in[c] * 40) * 0.4f * v[4] * 0.01f;
            out[c] = lp(&s->op[c][0], lp(&s->op[c][1], synth + fz, g[0]), g[0]);
        }
        break;
    }
    default: out[0] = in[0]; out[1] = in[1]; break;
    }
}

static void load_defaults(Plugin *s, int m) {
    for (int i = 0; i < NK; i++) {
        const Knob *k = &MACHINES[m].k[i];
        s->norm[kK + i] = k->name ? knob_norm(k, k->def) : 0;
    }
    s->norm[kMix] = MACHINES[m].mix * 0.01f;
    s->automate |= ((1u << NK) - 1) << kK | 1u << kMix | 1u << kMachine;
    s->update_display = 1;
}
static void set_machine(Plugin *s, int m) {
    s->norm[kMachine] = (float)m / (float)(kNumMachines - 1);
    load_defaults(s, m);
}
static void load_preset(Plugin *s, int idx) {
    const Preset *p = &PRESETS[idx];
    set_machine(s, p->machine);
    for (int i = 0; i < NK; i++) {
        const Knob *k = &MACHINES[p->machine].k[i];
        if (k->name) s->norm[kK + i] = knob_norm(k, p->k[i]);
    }
    s->norm[kMix] = p->mix * 0.01f;
    s->preset = idx;
    s->norm[kPreset] = (float)idx / (float)(NPRESETS - 1);
    s->automate |= 1u << kPreset;
}

static void process(AEffect *e, float **in, float **out, int32_t n) {
    VC_FLUSH_DENORMALS_BEGIN;
    Plugin *s = (Plugin *)e->object;
    if (s->master) {
        const uint32_t a = s->automate;
        if (a) {
            s->automate &= ~a;
            for (int i = 0; i < kNumParams; i++) if (a & (1u << i)) s->master(e, audioMasterAutomate, i, 0, NULL, s->norm[i]);
            s->update_display = 1;
        }
        if (s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    }
    const float *il = in && in[0] ? in[0] : NULL, *ir = in && in[1] ? in[1] : il;
    if (!s->ready) { for (int k = 0; k < n; k++) out[0][k] = out[1][k] = 0; goto done; }
    {
        const int m = option_of(s->norm[kMachine], kNumMachines);
        if (m != s->machine) { s->machine = m; reset(s); }
        const int hold = s->norm[kHold] > 0.5f;
        if (hold != s->hold) { s->hold = hold; s->tick = 0; }
        const float og = db(lerp(-24, 12, s->norm[kOutput]));
        for (int k = 0; k < n; k++) {
            if ((s->tick++ & 15) == 0) control(s);
            float l = il ? il[k] : 0, r = ir ? ir[k] : 0;
            if (!finite_bits(l) || l > 8 || l < -8) l = 0;   /* only clean audio in */
            if (!finite_bits(r) || r > 8 || r < -8) r = 0;
            const float x[2] = {l, r};
            float w[2];
            run_machine(s, x, w);
            s->mix_s += 0.002f * (s->norm[kMix] - s->mix_s);
            const float mx = s->mix_s, dg = mx < 0.5f ? 1 : 2 * (1 - mx), wg = mx > 0.5f ? 1 : 2 * mx;
            s->out_s += 0.002f * (og - s->out_s);
            float yl = (l * dg + w[0] * wg) * s->out_s, yr = (r * dg + w[1] * wg) * s->out_s;
            if (!finite_bits(yl) || !finite_bits(yr) || fabsf(yl) > 63 || fabsf(yr) > 63) { reset(s); yl = yr = 0; }   /* past +36 dB: start over */
            out[0][k] = clampf(yl, -4, 4);   /* +12 dB ceiling */
            out[1][k] = clampf(yr, -4, 4);
        }
    }
done:
    VC_FLUSH_DENORMALS_END;
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams) return;
    v = clampf(v, 0, 1);
    if (i == kPresetPrev || i == kPresetNext) {
        if (v > 0.5f) load_preset(s, s->preset < 0 ? (i == kPresetNext ? 0 : NPRESETS - 1) : (s->preset + (i == kPresetNext ? 1 : -1) + NPRESETS) % NPRESETS);
        s->norm[i] = 0;
        s->automate |= 1u << i;
        return;
    }
    if (i == kPreset) {   /* a small move (a Q-Link nudge) steps one; an exact value (a project loading) only names it */
        const float pos = v * (float)(NPRESETS - 1), cur = s->preset >= 0 ? (float)s->preset : s->norm[i] * (float)(NPRESETS - 1);
        if (fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1) { load_preset(s, ((int)lrintf(cur) + (pos > cur ? 1 : -1) + NPRESETS) % NPRESETS); return; }
        s->preset = (int)lrintf(pos);
        s->norm[i] = v;
        s->update_display = 1;
        return;
    }
    if (i == kMachine) {   /* a pick from the list (closes it), or a Q-Link nudge (steps one) */
        const float pos = v * (float)(kNumMachines - 1), cur = (float)option_of(s->norm[kMachine], kNumMachines);
        const int between = fabsf(pos - roundf(pos)) > 0.001f;
        int opt = between && fabsf(pos - cur) < 1 ? (int)cur + (pos > cur ? 1 : -1) : (int)lrintf(pos);
        opt = opt < 0 ? 0 : opt > kNumMachines - 1 ? kNumMachines - 1 : opt;
        if (opt != (int)cur) set_machine(s, opt);
        else s->norm[i] = (float)opt / (float)(kNumMachines - 1);
        if (!between && s->norm[kMachineOpen] > 0.5f) { s->norm[kMachineOpen] = 0; s->automate |= 1u << kMachineOpen; }
        s->update_display = 1;
        return;
    }
    if (i == kMachineOpen || i == kHold) { s->norm[i] = v > 0.5f ? 1 : 0; s->update_display = 1; return; }
    if (i >= kK && i < kK + NK) {   /* a choice or whole-number knob snaps (a Q-Link nudge steps one) */
        const Knob *k = &MACHINES[option_of(s->norm[kMachine], kNumMachines)].k[i - kK];
        if (k->name && (k->map == M_CHOICE || k->map == M_INT)) {
            const int nn = (int)(k->hi - k->lo) + 1;
            const float pos = v * (float)(nn - 1), cur = (float)option_of(s->norm[i], nn);
            int opt = nn > 2 && fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1 ? (int)cur + (pos > cur ? 1 : -1) : (int)lrintf(pos);
            opt = opt < 0 ? 0 : opt > nn - 1 ? nn - 1 : opt;
            v = (float)opt / (float)(nn - 1);
            s->update_display = 1;
        }
    }
    s->norm[i] = v;
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }

static void fmt_knob(const Knob *k, float n, char *t) {
    if (!k->name) { snprintf(t, 32, " "); return; }
    const float v = knob_value(k, n);
    if (k->map == M_CHOICE) { snprintf(t, 32, "%s", k->choices[(int)v]); return; }
    if (!strcmp(k->unit, "ms") && v >= 1000) { snprintf(t, 32, "%.2f s", v / 1000); return; }
    if (!strcmp(k->unit, "Hz") && v >= 1000) { snprintf(t, 32, "%.1f kHz", v / 1000); return; }
    if (!strcmp(k->unit, "st")) { snprintf(t, 32, "%+.0f st", v); return; }
    if (!strcmp(k->unit, "dB")) { snprintf(t, 32, "%+.1f dB", v); return; }
    if (!strcmp(k->unit, "x")) { snprintf(t, 32, "%.2fx", v); return; }
    const int pct = !strcmp(k->unit, "%");
    if (k->map == M_INT || pct || fabsf(v) >= 100) snprintf(t, 32, "%.0f%s%s", v, k->unit[0] && !pct ? " " : "", k->unit);
    else snprintf(t, 32, fabsf(v) >= 10 ? "%.1f %s" : "%.2f %s", v, k->unit);
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: free_all(s); free(s); return 1;
    case effGetParamName:
        if (idx >= 0 && idx < kNumParams) {
            static const char *const N[] = {"Machine", "Machine List", "Mix"};
            char t[40];
            if (idx < kK) snprintf(t, sizeof t, "%s", N[idx]);
            else if (idx < kK + NK) { const Knob *k = &MACHINES[option_of(s->norm[kMachine], kNumMachines)].k[idx - kK]; snprintf(t, sizeof t, "%s", k->name ? k->name : "-"); }
            else snprintf(t, sizeof t, "%s", idx == kHold ? "Hold" : idx == kPreset ? "Preset" : idx == kPresetPrev ? "Preset <" : idx == kPresetNext ? "Preset >" : "Output");
            copy_str(p, t, 32);
        }
        return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay:
        if (idx >= 0 && idx < kNumParams) {
            char t[64];
            const int m = option_of(s->norm[kMachine], kNumMachines);
            if (idx == kMachine) snprintf(t, sizeof t, "%s", MACHINES[m].name);
            else if (idx == kMachineOpen) snprintf(t, sizeof t, "%s", s->norm[idx] > 0.5f ? "Open" : "Closed");
            else if (idx == kMix) snprintf(t, sizeof t, "%.0f%%", s->norm[idx] * 100);
            else if (idx < kK + NK) fmt_knob(&MACHINES[m].k[idx - kK], s->norm[idx], t);
            else if (idx == kHold) snprintf(t, sizeof t, "%s", s->norm[idx] > 0.5f ? "Held" : "Off");
            else if (idx == kPreset) snprintf(t, sizeof t, "%s", s->preset >= 0 && s->preset < NPRESETS && PRESETS[s->preset].machine == m ? PRESETS[s->preset].name : "-");
            else if (idx == kPresetPrev || idx == kPresetNext) snprintf(t, sizeof t, "%s", idx == kPresetPrev ? "<" : ">");
            else snprintf(t, sizeof t, "%+.1f dB", lerp(-24, 12, s->norm[idx]));
            copy_str(p, t, 24);
        }
        return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "Ambient Lab", 32); return 1;
    case effGetVendorString: copy_str(p, VC_VENDOR, 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate: if (opt > 1000 && fabsf(opt - s->fs) > 0.5f) setup(s, opt); return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (val) reset(s); return 1;
    case effProcessEvents: {   /* MIDI Program Change n: preset n+1; CC 64 (sustain): Hold */
        const VstEvents *ev = (const VstEvents *)p;
        for (int k = 0; ev && k < ev->numEvents; k++) {
            if (!ev->events[k] || ev->events[k]->type != 1) continue;
            const unsigned char *md = ev->events[k]->midiData;
            if ((md[0] & 0xf0) == 0xc0 && md[1] < NPRESETS) load_preset(s, md[1]);
            else if ((md[0] & 0xf0) == 0xb0 && md[1] == 64) { s->norm[kHold] = md[2] >= 64 ? 1 : 0; s->automate |= 1u << kHold; }
        }
        return 1;
    }
    case effCanDo:
        return p && (!strcmp((const char *)p, "receiveVstEvents") || !strcmp((const char *)p, "receiveVstMidiEvent")) ? 1 : -1;
    default: return 0;
    }
}

/* the full-page list of the presets (tools/picklist.h; Quad Rig calls this for its slots' engines too) */
int amb_pick(AEffect *e, int list, int op, int i, char *out, int max) {
    Plugin *s = (Plugin *)e->object;
    (void)list;
    if (op == PL_COUNT) return NPRESETS;
    if (op == PL_CURRENT) return s->preset >= 0 && s->preset < NPRESETS && PRESETS[s->preset].machine == option_of(s->norm[kMachine], kNumMachines) ? s->preset : -1;
    if (i < 0 || i >= NPRESETS) return 0;
    if (op == PL_NAME) snprintf(out, (size_t)max, "%s", PRESETS[i].name);
    else if (op == PL_CHOOSE) load_preset(s, i);
    return 0;
}
static const char *const LIST_KEYS[] = {"preset"};
static const char *const LIST_TITLES[] = {"Presets"};
static const PickLists LISTS = {1, LIST_KEYS, LIST_TITLES, amb_pick};

static AEffect *make(audioMasterCallback master) {
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    s->preset = -1;
    s->machine = -1;
    s->norm[kOutput] = 24.0f / 36;   /* 0 dB */
    set_machine(s, MK_MICRO);
    s->automate = 0;
    s->out_s = 1;
    for (int k = 0; k < 8; k++) s->seq[k] = rnd() * 2 - 1;   /* NiteSky's filter steps */
    setup(s, 44100);
    s->machine = MK_MICRO;
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
    e->uniqueID = 0x414d424c;   /* 'AMBL' */
    e->version = 1000;
    return e;
}
__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
#ifdef PICKLIST_OFF
    return make(master);
#else
    return pl_wrap(master, make, &LISTS);
#endif
}

#ifdef AMB_PARAMS
/* the page's parameter list (params.json), from the tables above */
int main(void) {
    printf("{\n  \"name\": \"Ambient Lab\",\n  \"params\": [\n");
    printf("    {\"key\": \"machine\", \"name\": \"Machine\", \"options\": [");
    for (int m = 0; m < kNumMachines; m++) printf("%s\"%s\"", m ? ", " : "", MACHINES[m].name);
    printf("]},\n");
    printf("    {\"key\": \"machine__open\", \"name\": \"Machine List\", \"options\": [\"Closed\", \"Open\"]},\n");
    printf("    {\"key\": \"mix\", \"name\": \"Mix\", \"min\": 0, \"max\": 100, \"unit\": \"%%\"},\n");
    for (int i = 0; i < NK; i++) printf("    {\"key\": \"k%d\", \"name\": \"Knob %d\", \"min\": 0, \"max\": 100},\n", i + 1, i + 1);
    printf("    {\"key\": \"hold\", \"name\": \"Hold\", \"options\": [\"Off\", \"Held\"]},\n");
    printf("    {\"key\": \"preset\", \"name\": \"Preset\", \"min\": 0, \"max\": 1, \"display\": \"string\", \"type\": \"stepper\"},\n");
    printf("    {\"key\": \"preset_prev\", \"name\": \"Preset <\", \"options\": [\"Off\", \"On\"], \"momentary\": true},\n");
    printf("    {\"key\": \"preset_next\", \"name\": \"Preset >\", \"options\": [\"Off\", \"On\"], \"momentary\": true},\n");
    printf("    {\"key\": \"output\", \"name\": \"Output\", \"min\": -24, \"max\": 12, \"default\": 0, \"unit\": \"dB\"}");
    pl_params_json(&LISTS, stdout);
    printf("\n  ]\n}\n");
    return 0;
}
#endif
