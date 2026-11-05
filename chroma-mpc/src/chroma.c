/* Chr0ma Console: four effect modules in series for MPC OS (VST2, stereo), in the spirit of the four-module texture
 * pedals (our own code and sound, on vocal-mpc's core: parameter table, lines, one-poles, FDN reverb).
 *   Character  Drive / Sweeten / Fuzz / Howl / Swell
 *   Movement   Doubler / Vibrato / Phaser / Tremolo / Pitch
 *   Diffusion  Cascade / Reels / Space / Collage / Reverse
 *   Texture    Filter / Squash / Cassette / Broken / Interference
 * Each module: its effect, Amount, Tweak (a second control, its meaning per effect, named live) and On. Then Tilt
 * (dark .. bright), Sensitivity (the input's level into the chain), Mix and Level. Programs: PROGS[].
 * MIDI (any channel / MIDI Ch): Program Change n = program n + 1; CC 102-105 switch the modules (64+ = on).
 * Every feedback loop saturates and stays under one. Safety as everywhere here: input cleaned, reset if the output
 * goes bad or past +36 dB, capped at +12 dB. */
#include "vocal_core.h"

static const char *const CHARS[] = {"Drive", "Sweeten", "Fuzz", "Howl", "Swell"};
static const char *const MOVES[] = {"Doubler", "Vibrato", "Phaser", "Tremolo", "Pitch"};
static const char *const DIFFS[] = {"Cascade", "Reels", "Space", "Collage", "Reverse"};
static const char *const TEXTS[] = {"Filter", "Squash", "Cassette", "Broken", "Interference"};
/* what Tweak does, per module and effect (the knob's name, live) */
static const char *const TWEAKS[4][5] = {{"Tone", "Sparkle", "Tone", "Pitch", "Swell Time"},
                                         {"Time", "Rate", "Rate", "Rate", "Interval"},
                                         {"Time", "Time", "Decay", "Density", "Length"},
                                         {"Range", "Attack", "Age", "Dropouts", "Frequency"}};
enum { kProgram, kProgPrev, kProgNext, kMod0, M_TYPE = 0, M_AMT, M_TWEAK, M_ON, MP,
       kTilt = kMod0 + 4 * MP, kSens, kMix, kLevel, kNumParams };
#define MOD(m, w) (kMod0 + (m) * MP + (w))
#define MODDEF(k, n, list, t) OPT(k "_type", n, list, t), PCT(k "_amt", n " Amount", 50), PCT(k "_tweak", n " Tweak", 50), \
    TOGGLE(k "_on", n " On", 1)
static const ParamDef P[kNumParams] = {
    PRESET("program", "Program"), BUTTON("program_prev", "Program <"), BUTTON("program_next", "Program >"),
    MODDEF("char", "Character", CHARS, 0), MODDEF("move", "Movement", MOVES, 0), MODDEF("diff", "Diffusion", DIFFS, 2),
    MODDEF("text", "Texture", TEXTS, 2),
    LIN("tilt", "Tilt", -100, 100, 0, "%"), LIN("sens", "Sensitivity", -12, 12, 0, "dB"), PCT("mix", "Mix", 100),
    LIN("level", "Level", -24, 12, 0, "dB"),
};

/* ---- programs (ours): per module type, amount %, tweak %, on; then tilt, sensitivity, mix, level ---------------- */
typedef struct { const char *name; float m[4][4]; float tilt, sens, mix, level; } Prog;
static const Prog PROGS[] = {
    {"Warm Doubled", {{0, 25, 45, 1}, {0, 45, 40, 1}, {2, 25, 35, 1}, {1, 35, 50, 1}}, -15, 0, 100, 0},
    {"Lo-Fi Memory", {{1, 30, 50, 1}, {1, 30, 30, 1}, {1, 40, 45, 1}, {2, 65, 60, 1}}, -25, 0, 100, 0},
    {"Tape Dream", {{0, 15, 40, 1}, {3, 20, 35, 1}, {1, 55, 60, 1}, {2, 45, 40, 1}}, -20, 0, 100, 0},
    {"Broken Radio", {{2, 45, 35, 1}, {3, 40, 70, 1}, {0, 30, 40, 1}, {4, 60, 55, 1}}, 20, 0, 100, -3},
    {"Fuzz Cathedral", {{2, 70, 45, 1}, {0, 30, 50, 0}, {2, 60, 80, 1}, {0, 35, 40, 1}}, -10, 0, 100, -4},
    {"Swell Pads", {{4, 90, 55, 1}, {1, 25, 25, 1}, {2, 55, 75, 1}, {1, 30, 40, 1}}, 0, 0, 100, 0},
    {"Octave Space", {{1, 25, 50, 1}, {4, 45, 100, 1}, {2, 55, 70, 1}, {0, 20, 60, 0}}, 10, 0, 100, -2},
    {"Phase Drift", {{0, 20, 50, 1}, {2, 60, 20, 1}, {0, 40, 55, 1}, {0, 30, 50, 0}}, 0, 0, 100, 0},
    {"Reverse Haze", {{1, 20, 50, 1}, {1, 25, 30, 1}, {4, 60, 55, 1}, {2, 35, 40, 1}}, -10, 0, 100, 0},
    {"Cassette Chorus", {{0, 15, 50, 0}, {0, 55, 55, 1}, {2, 20, 30, 1}, {2, 70, 50, 1}}, -15, 0, 100, 0},
    {"Howling Wind", {{3, 55, 60, 1}, {1, 40, 25, 1}, {2, 60, 85, 1}, {0, 40, 70, 1}}, -5, -3, 100, -4},
    {"Granular Bloom", {{1, 30, 50, 1}, {0, 30, 40, 1}, {3, 60, 65, 1}, {0, 30, 50, 0}}, 5, 0, 100, -1},
    {"Squashed Tremolo", {{0, 35, 55, 1}, {3, 65, 55, 1}, {0, 25, 30, 1}, {1, 70, 30, 1}}, 0, 0, 100, -2},
    {"Sweet Shimmer", {{1, 55, 65, 1}, {4, 30, 100, 1}, {2, 45, 60, 1}, {1, 30, 50, 1}}, 15, 0, 100, -2},
    {"Interference", {{2, 30, 50, 1}, {2, 40, 60, 1}, {3, 45, 50, 1}, {4, 75, 35, 1}}, 0, 0, 100, -4},
    {"Clean Space", {{0, 10, 50, 0}, {0, 30, 40, 1}, {2, 40, 55, 1}, {0, 20, 50, 0}}, 5, 0, 100, 0},
};
#define NPROGS ((int)(sizeof PROGS / sizeof *PROGS))

/* ---- building blocks ----------------------------------------------------------------------------------------- */
typedef struct { Line l; float d; int win; } Shift;   /* two heads half a window apart, Hann-crossfaded */
static float shift_run(Shift *s, float x, float ratio) {
    line_push(&s->l, x);
    const float W = (float)s->win;
    s->d += 1 - ratio;
    while (s->d < 0) s->d += W;
    while (s->d >= W) s->d -= W;
    float y = 0;
    for (int h = 0; h < 2; h++) {
        float d = s->d + (float)h * W * 0.5f;
        if (d >= W) d -= W;
        y += line_read(&s->l, d + 2) * (0.5f - 0.5f * cosf(2 * PI * d / W));
    }
    return y;
}
typedef struct { float z; } Ap1;   /* first-order allpass (phaser stage) */
static float ap1(Ap1 *a, float x, float c) { const float y = c * x + a->z; a->z = x - c * y; return y; }

typedef struct { float pos, inc, len, age, pan; int on; } Grain;
#define NGR 8
typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    int ready, program;
    volatile int update_display;
    volatile unsigned char automate[kNumParams];
    float on_s[4], mix_s, out_s, sens_s;
    /* character */
    OnePole c_lp[2], c_hp[2], c_pre[2];
    Line howl[2];
    float env, swell, prev_env;
    /* movement */
    Line mv[2];
    Shift sh[2];
    Ap1 ph[2][6];
    float lfo, lfo2, ph_fb[2], rnd_d[2];
    /* diffusion */
    Line dl[2];
    Verb verb;
    float verb_key;
    Line apd[2][3];
    OnePole d_lp[2];
    Grain gr[2][NGR];
    float rev_pos, gr_timer;
    /* texture */
    float t_ic1[2], t_ic2[2], t_env, t_gr, hold[2], hold_n, drop, drop_left;
    OnePole t_lp[2], t_hiss;
    Line wow[2];
    float wow_ph, flut_ph, int_ph;
    OnePole tilt_lp[2];
    uint32_t rng;
} Plugin;

static float val(const Plugin *s, int i) { return pv(&P[i], s->norm[i]); }
static float frand(Plugin *s) { s->rng = s->rng * 1664525u + 1013904223u; return (float)(s->rng >> 8) / 16777216.0f; }

static void reset(Plugin *s) {
    for (int c = 0; c < 2; c++) {
        line_clear(&s->howl[c]); line_clear(&s->mv[c]); line_clear(&s->sh[c].l); line_clear(&s->dl[c]); line_clear(&s->wow[c]);
        for (int k = 0; k < 3; k++) line_clear(&s->apd[c][k]);
        memset(s->ph[c], 0, sizeof s->ph[c]);
        s->c_lp[c].z = s->c_hp[c].z = s->c_pre[c].z = s->d_lp[c].z = s->t_lp[c].z = s->tilt_lp[c].z = 0;
        s->t_ic1[c] = s->t_ic2[c] = s->ph_fb[c] = s->hold[c] = 0;
        for (int g = 0; g < NGR; g++) s->gr[c][g].on = 0;
    }
    verb_clear(&s->verb);
    s->env = s->swell = s->t_env = s->t_gr = 0;
}
static int setup(Plugin *s, float fs) {
    s->ready = 0;
    s->fs = fs;
    int ok = verb_init(&s->verb, fs);
    for (int c = 0; c < 2; c++) {
        ok &= line_init(&s->howl[c], (int)(fs * 0.03f));
        ok &= line_init(&s->mv[c], (int)(fs * 0.06f));
        s->sh[c].win = (int)(fs * 0.05f);
        ok &= line_init(&s->sh[c].l, s->sh[c].win * 2 + 8);
        ok &= line_init(&s->dl[c], (int)(fs * 3.2f));
        ok &= line_init(&s->wow[c], (int)(fs * 0.05f));
        for (int k = 0; k < 3; k++) ok &= line_init(&s->apd[c][k], (int)(fs * 0.03f));
    }
    if (!ok) return 0;
    s->verb_key = -1;
    reset(s);
    s->ready = 1;
    return 1;
}

/* ---- the modules (per sample, stereo in place) ----------------------------------------------------------------- */
static void character(Plugin *s, int type, float amt, float tw, float *x) {
    const float fs = s->fs;
    switch (type) {
    case 0: {   /* drive: warm soft clipping, Tweak = tone */
        const float g = 1 + amt * 25, lv = 1 / (1 + amt * 2.2f), a = lp_coef(expmap(1500, 14000, tw), fs);
        for (int c = 0; c < 2; c++) x[c] = lp(&s->c_lp[c], tanh_fast(x[c] * g) * lv, a);
        break;
    }
    case 1: {   /* sweeten: an exciter on the top (Tweak = where it starts) and a little glue */
        const float a = lp_coef(expmap(1500, 7000, 1 - tw), fs);
        for (int c = 0; c < 2; c++) { const float top = hp(&s->c_hp[c], x[c], a); x[c] = tanh_fast((x[c] + amt * 0.7f * tanh_fast(top * 6)) * 1.1f) / 1.1f; }
        break;
    }
    case 2: {   /* fuzz: hard square-ish clipping, Tweak = tone */
        const float g = 1 + amt * 120, a = lp_coef(expmap(1000, 9000, tw), fs), pre = lp_coef(120, fs);
        for (int c = 0; c < 2; c++) {
            const float v = hp(&s->c_pre[c], x[c], pre) * g;
            x[c] = lp(&s->c_lp[c], clampf(v, -1, 1) * 0.45f, a);
        }
        break;
    }
    case 3: {   /* howl: a short saturated feedback loop that sings (Tweak = its pitch) */
        const float d = fs / expmap(90, 1500, tw), fb = 0.55f + 0.42f * amt;
        for (int c = 0; c < 2; c++) {
            const float y = line_read(&s->howl[c], d * (c ? 1.003f : 1));
            line_push(&s->howl[c], x[c] + tanh_fast(lp(&s->c_lp[c], y, 0.4f) * fb));
            x[c] = (x[c] + y * amt) * (1 - 0.3f * amt);
        }
        break;
    }
    default: {   /* swell: each new note fades in (Tweak = how slowly) */
        const float lvl = fmaxf(fabsf(x[0]), fabsf(x[1]));
        s->env += (lvl > s->env ? 0.02f : 0.0004f) * (lvl - s->env);
        if (s->env > 0.02f && s->env > s->prev_env * 2.2f + 0.004f) s->swell = 0;   /* a new note */
        s->prev_env += 0.0005f * (s->env - s->prev_env);
        s->swell = fminf(1, s->swell + 1 / (expmap(50, 2500, tw) * 0.001f * fs));
        const float g = 1 - amt + amt * s->swell * s->swell;
        x[0] *= g; x[1] *= g;
        break;
    }
    }
}
static void movement(Plugin *s, int type, float amt, float tw, float *x) {
    const float fs = s->fs;
    switch (type) {
    case 0: {   /* doubler: a short, slowly wandering second take each side (Tweak = how late) */
        const float base = expmap(10, 40, tw) * 0.001f * fs;
        s->lfo += 0.31f / fs; if (s->lfo >= 1) s->lfo -= 1;
        for (int c = 0; c < 2; c++) {
            line_push(&s->mv[c], x[c]);
            const float d = base * (c ? 1.23f : 1) * (1 + 0.04f * sinf(2 * PI * (s->lfo + 0.37f * (float)c)));
            x[c] = x[c] * (1 - 0.35f * amt) + line_read(&s->mv[c], d) * amt * 0.8f;
        }
        break;
    }
    case 1: {   /* vibrato: pitch wobble (Tweak = rate) */
        s->lfo += expmap(0.3f, 10, tw) / fs; if (s->lfo >= 1) s->lfo -= 1;
        for (int c = 0; c < 2; c++) {
            line_push(&s->mv[c], x[c]);
            x[c] = line_read(&s->mv[c], (0.004f + 0.0035f * amt * (1 + sinf(2 * PI * s->lfo))) * fs);
        }
        break;
    }
    case 2: {   /* phaser: six stages with feedback (Tweak = rate) */
        s->lfo += expmap(0.05f, 5, tw) / fs; if (s->lfo >= 1) s->lfo -= 1;
        for (int c = 0; c < 2; c++) {
            const float m = 0.5f + 0.5f * sinf(2 * PI * (s->lfo + 0.25f * (float)c));
            const float hz = expmap(200, 3000, m * amt + (1 - amt) * 0.3f), t = tanf(PI * hz / fs), co = (t - 1) / (t + 1);
            float y = x[c] + s->ph_fb[c] * 0.5f;
            for (int k = 0; k < 6; k++) y = ap1(&s->ph[c][k], y, co);
            s->ph_fb[c] = tanh_fast(y);
            x[c] = (x[c] + y * amt) / (1 + amt * 0.5f);
        }
        break;
    }
    case 3: {   /* tremolo (Tweak = rate), the sides a little apart */
        s->lfo += expmap(1, 14, tw) / fs; if (s->lfo >= 1) s->lfo -= 1;
        for (int c = 0; c < 2; c++) x[c] *= 1 - amt * 0.5f * (1 + sinf(2 * PI * (s->lfo + 0.12f * (float)c)));
        break;
    }
    default: {   /* pitch: a shifted voice blended in (Tweak = interval: -12, -5, +5, +7, +12) */
        static const float ST[5] = {-12, -5, 5, 7, 12};
        const float ratio = powf(2, ST[(int)lrintf(tw * 4)] / 12);
        for (int c = 0; c < 2; c++) x[c] = x[c] * (1 - 0.4f * amt) + shift_run(&s->sh[c], x[c], ratio * (c ? 1.002f : 0.998f)) * amt;
        break;
    }
    }
}
static void diffusion(Plugin *s, int type, float amt, float tw, float *x) {
    const float fs = s->fs;
    float w[2] = {0, 0};
    switch (type) {
    case 0: {   /* cascade: four taps through diffusers, fed back (Tweak = time) */
        const float T = expmap(80, 900, tw) * 0.001f * fs;
        static const float TAP[4] = {0.31f, 0.55f, 0.78f, 1};
        for (int c = 0; c < 2; c++) {
            float y = 0;
            for (int k = 0; k < 4; k++) y += line_read(&s->dl[c], T * TAP[k] * (c ? 1.07f : 1)) * (0.5f - 0.08f * (float)k);
            for (int k = 0; k < 3; k++) {   /* allpass diffusers */
                Line *l = &s->apd[c][k];
                const float d = line_tap(l, (int)((3.1f + 4.3f * (float)k + (float)c) * 0.001f * fs)), u = y + 0.55f * d;
                line_push(l, u);
                y = d - 0.55f * u;
            }
            line_push(&s->dl[c], x[c] + tanh_fast(lp(&s->d_lp[c], y, 0.3f) * 0.45f));
            w[c] = y;
        }
        break;
    }
    case 1: {   /* reels: a worn tape echo (Tweak = time) */
        const float T = expmap(60, 1100, tw) * 0.001f * fs;
        s->gr_timer += 0.6f / fs; if (s->gr_timer >= 1) s->gr_timer -= 1;
        const float wow = 1 + 0.004f * sinf(2 * PI * s->gr_timer);
        for (int c = 0; c < 2; c++) {
            const float y = lp(&s->d_lp[c], line_read(&s->dl[c], T * wow * (c ? 1.02f : 1)), 0.25f);
            line_push(&s->dl[c], x[c] + tanh_fast(y * 0.5f));
            w[c] = y;
        }
        break;
    }
    case 2: {   /* space: a big reverb (Tweak = decay) */
        if (tw != s->verb_key) { s->verb_key = tw; verb_set(&s->verb, 1.6f, expmap(1, 14, tw), 7000, fs); }
        verb_run(&s->verb, x[0] * 0.35f, x[1] * 0.35f, &w[0], &w[1]);
        break;
    }
    case 3: {   /* collage: grains picked from the last second and a half (Tweak = how many) */
        for (int c = 0; c < 2; c++) line_push(&s->dl[c], x[c]);
        s->gr_timer += expmap(2, 40, tw) / fs;
        if (s->gr_timer >= 1) {
            s->gr_timer -= 1;
            for (int c = 0; c < 2; c++)
                for (int g = 0; g < NGR; g++)
                    if (!s->gr[c][g].on) {
                        Grain *q = &s->gr[c][g];
                        q->on = 1; q->age = 0; q->len = (0.05f + 0.2f * frand(s)) * fs;
                        q->pos = (0.1f + 1.3f * frand(s)) * fs; q->inc = frand(s) < 0.25f ? -1.0f : frand(s) < 0.15f ? 0.5f : 0.0f;
                        break;
                    }
        }
        for (int c = 0; c < 2; c++) {
            float y = 0;
            for (int g = 0; g < NGR; g++) {
                Grain *q = &s->gr[c][g];
                if (!q->on) continue;
                const float u = q->age / q->len;
                y += line_read(&s->dl[c], q->pos) * sinf(PI * u);
                q->pos += q->inc;
                if (++q->age >= q->len) q->on = 0;
            }
            w[c] = y * 0.6f;
        }
        break;
    }
    default: {   /* reverse: chunks played backwards (Tweak = their length) */
        const float L = expmap(200, 1500, tw) * 0.001f * fs;
        for (int c = 0; c < 2; c++) line_push(&s->dl[c], x[c] + tanh_fast(w[c] * 0.3f));
        s->rev_pos += 2; if (s->rev_pos >= L) s->rev_pos -= L;
        for (int c = 0; c < 2; c++) {
            const float u = s->rev_pos / L, e = sinf(PI * u);
            float u2 = u + 0.5f; if (u2 >= 1) u2 -= 1;
            w[c] = line_read(&s->dl[c], s->rev_pos + 2) * e + line_read(&s->dl[c], u2 * L + 2) * sinf(PI * u2);
            w[c] *= 0.7f;
        }
        break;
    }
    }
    for (int c = 0; c < 2; c++) x[c] = x[c] * (1 - 0.5f * amt) + w[c] * amt * 1.2f;
}
static void texture(Plugin *s, int type, float amt, float tw, float *x) {
    const float fs = s->fs;
    switch (type) {
    case 0: {   /* filter: an envelope-swept resonant low-pass (Tweak = its range) */
        const float lvl = fmaxf(fabsf(x[0]), fabsf(x[1]));
        s->t_env += (lvl > s->t_env ? 0.01f : 0.0006f) * (lvl - s->t_env);
        const float hz = clampf(expmap(150, 1500, tw) * powf(2, fminf(s->t_env * 20, 1) * 4 * amt), 40, fs * 0.42f);
        const float g = tanf(PI * hz / fs), k = 2 - 1.6f * amt, a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        for (int c = 0; c < 2; c++) {
            const float v3 = x[c] - s->t_ic2[c], v1 = a1 * s->t_ic1[c] + a2 * v3, v2 = s->t_ic2[c] + a2 * s->t_ic1[c] + a3 * v3;
            s->t_ic1[c] = 2 * v1 - s->t_ic1[c]; s->t_ic2[c] = 2 * v2 - s->t_ic2[c];
            x[c] = v2;
        }
        break;
    }
    case 1: {   /* squash: a compressor that pumps a little (Tweak = attack) */
        const float lvl = fmaxf(fabsf(x[0]), fabsf(x[1]));
        const float at = 1 - expf(-1 / (expmap(0.5f, 60, tw) * 0.001f * fs)), re = 1 - expf(-1 / (0.15f * fs));
        s->t_env += (lvl > s->t_env ? at : re) * (lvl - s->t_env);
        const float thr = db(-40 * amt), over = s->t_env > thr ? s->t_env / thr : 1;
        const float g = powf(over, -0.8f) * db(10 * amt);
        x[0] *= g; x[1] *= g;
        break;
    }
    case 2: {   /* cassette: wow, flutter, a dull top and hiss (Tweak = how worn) */
        s->wow_ph += 0.45f / fs; if (s->wow_ph >= 1) s->wow_ph -= 1;
        s->flut_ph += 9.3f / fs; if (s->flut_ph >= 1) s->flut_ph -= 1;
        const float d = (0.006f + amt * 0.0035f * (sinf(2 * PI * s->wow_ph) + 0.25f * sinf(2 * PI * s->flut_ph))) * fs;
        const float a = lp_coef(expmap(2500, 12000, 1 - tw), fs), hiss = (frand(s) - 0.5f) * 0.006f * amt * (0.5f + tw);
        for (int c = 0; c < 2; c++) {
            line_push(&s->wow[c], x[c]);
            x[c] = lp(&s->t_lp[c], tanh_fast(line_read(&s->wow[c], d) * 1.2f) / 1.2f, a) + hiss;
        }
        break;
    }
    case 3: {   /* broken: fewer bits, a lower rate, and drop-outs (Tweak = how many) */
        const float bits = powf(2, 14 - 10 * amt), every = 1 + amt * 10;
        if (s->drop_left > 0) s->drop_left -= 1;
        else if (frand(s) < tw * tw * 0.00012f) s->drop_left = (0.02f + 0.15f * frand(s)) * fs;
        s->drop += 0.004f * ((s->drop_left > 0 ? 0.0f : 1.0f) - s->drop);
        if ((s->hold_n += 1) >= every) {
            s->hold_n = 0;
            for (int c = 0; c < 2; c++) s->hold[c] = floorf(x[c] * bits + 0.5f) / bits;
        }
        for (int c = 0; c < 2; c++) x[c] = s->hold[c] * s->drop;
        break;
    }
    default: {   /* interference: a wandering carrier and crackle (Tweak = its frequency) */
        s->int_ph += expmap(30, 3000, tw) * (1 + 0.02f * sinf(2 * PI * s->wow_ph)) / fs; if (s->int_ph >= 1) s->int_ph -= 1;
        s->wow_ph += 0.2f / fs; if (s->wow_ph >= 1) s->wow_ph -= 1;
        const float lvl = fmaxf(fabsf(x[0]), fabsf(x[1]));   /* (the noise follows the playing: no hiss in silence) */
        s->t_env += (lvl > s->t_env ? 0.01f : 0.0003f) * (lvl - s->t_env);
        const float nz = fminf(s->t_env * 4, 1);
        const float car = sinf(2 * PI * s->int_ph), crackle = frand(s) < 0.0008f * amt ? (frand(s) - 0.5f) * 0.3f * amt * nz : 0;
        for (int c = 0; c < 2; c++) x[c] = x[c] * (1 - amt * 0.6f + amt * 0.6f * car) + crackle + (frand(s) - 0.5f) * 0.004f * amt * nz;
        break;
    }
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
    int type[4];
    float amt[4], tw[4], on[4];
    for (int m = 0; m < 4; m++) {
        type[m] = (int)val(s, MOD(m, M_TYPE)); amt[m] = val(s, MOD(m, M_AMT)) / 100; tw[m] = val(s, MOD(m, M_TWEAK)) / 100;
        on[m] = s->norm[MOD(m, M_ON)] > 0.5f ? 1 : 0;
    }
    const float tilt = val(s, kTilt) / 100, sens = db(val(s, kSens)), mix = val(s, kMix) / 100, og = db(val(s, kLevel));
    const float tl = db(-tilt * 6), th = db(tilt * 6), ta = lp_coef(800, s->fs);
    for (int k = 0; k < n; k++) {
        float a = il ? il[k] : 0, b = ir ? ir[k] : a;
        if (!finite_bits(a) || a > 8 || a < -8) a = 0;
        if (!finite_bits(b) || b > 8 || b < -8) b = 0;
        s->sens_s += 0.002f * (sens - s->sens_s);
        float x[2] = {a * s->sens_s, b * s->sens_s};
        for (int m = 0; m < 4; m++) {
            s->on_s[m] += 0.003f * (on[m] - s->on_s[m]);
            if (s->on_s[m] < 1e-4f && on[m] == 0) continue;
            float y[2] = {x[0], x[1]};
            switch (m) {
            case 0: character(s, type[m], amt[m], tw[m], y); break;
            case 1: movement(s, type[m], amt[m], tw[m], y); break;
            case 2: diffusion(s, type[m], amt[m], tw[m], y); break;
            default: texture(s, type[m], amt[m], tw[m], y); break;
            }
            for (int c = 0; c < 2; c++) x[c] += (y[c] - x[c]) * s->on_s[m];
        }
        for (int c = 0; c < 2; c++) {   /* tilt: lows one way, highs the other, around 800 Hz */
            const float lo = lp(&s->tilt_lp[c], x[c], ta);
            x[c] = lo * tl + (x[c] - lo) * th;
        }
        s->mix_s += 0.002f * (mix - s->mix_s);
        s->out_s += 0.002f * (og - s->out_s);
        float l = (a * (1 - s->mix_s) + x[0] * s->mix_s) * s->out_s, r = (b * (1 - s->mix_s) + x[1] * s->mix_s) * s->out_s;
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
    const Prog *g = &PROGS[idx];
    for (int m = 0; m < 4; m++)
        for (int w = 0; w < MP; w++) { s->norm[MOD(m, w)] = pn(&P[MOD(m, w)], g->m[m][w]); s->automate[MOD(m, w)] = 1; }
    const float v[4] = {g->tilt, g->sens, g->mix, g->level};
    for (int i = 0; i < 4; i++) { s->norm[kTilt + i] = pn(&P[kTilt + i], v[i]); s->automate[kTilt + i] = 1; }
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
    if (P[i].kind == K_OPT || P[i].kind == K_TOGGLE) s->update_display = 1;
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }
static void param_name(const Plugin *s, int i, char *t) {
    if (i >= kMod0 && i < kTilt && (i - kMod0) % MP == M_TWEAK) {   /* the Tweak knob's meaning for the effect chosen */
        const int m = (i - kMod0) / MP;
        snprintf(t, 32, "%s", TWEAKS[m][(int)val(s, MOD(m, M_TYPE))]);
        return;
    }
    snprintf(t, 32, "%s", P[i].name);
}
static void display(const Plugin *s, int i, char *t) {
    if (i == kProgram) { snprintf(t, 24, "%s", s->program >= 0 && s->program < NPROGS ? PROGS[s->program].name : "-"); return; }
    if (i == kProgPrev || i == kProgNext) { snprintf(t, 24, "%s", i == kProgPrev ? "<" : ">"); return; }
    if (i >= kMod0 && i < kTilt && (i - kMod0) % MP == M_TWEAK) {
        const int m = (i - kMod0) / MP, ty = (int)val(s, MOD(m, M_TYPE));
        if (m == 1 && ty == 4) { static const char *const IV[5] = {"-12 st", "-5 st", "+5 st", "+7 st", "+12 st"}; snprintf(t, 24, "%s", IV[(int)lrintf(s->norm[i] * 4)]); return; }
    }
    pdisplay(&P[i], s->norm[i], t);
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        for (int c = 0; c < 2; c++) {
            line_free(&s->howl[c]); line_free(&s->mv[c]); line_free(&s->sh[c].l); line_free(&s->dl[c]); line_free(&s->wow[c]);
            for (int k = 0; k < 3; k++) line_free(&s->apd[c][k]);
        }
        verb_free(&s->verb);
        free(s);
        return 1;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) { char t[40] = {0}; param_name(s, idx, t); copy_str(p, t, 32); } return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay: if (idx >= 0 && idx < kNumParams) { char t[32] = {0}; display(s, idx, t); copy_str(p, t, 24); } return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "Chr0ma Console", 32); return 1;
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
            else if ((m[0] & 0xf0) == 0xb0 && m[1] >= 102 && m[1] <= 105) {
                const int i = MOD(m[1] - 102, M_ON);
                s->norm[i] = m[2] >= 64 ? 1.0f : 0.0f;
                s->automate[i] = 1;
            }
        }
        return 1;
    }
    case effCanDo: return p && (!strcmp((const char *)p, "receiveVstEvents") || !strcmp((const char *)p, "receiveVstMidiEvent")) ? 1 : -1;
    default: return 0;
    }
}

static int chroma_pick(AEffect *e, int list, int op, int i, char *out, int max) {
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
static const PickLists LISTS = {1, LIST_KEYS, LIST_TITLES, chroma_pick, 1};   /* (1: MIDI Ch) */

static AEffect *make(audioMasterCallback master) {
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    s->rng = 4242u;
    s->program = -1;
    s->drop = 1;
    for (int i = 0; i < kNumParams; i++) s->norm[i] = pn(&P[i], P[i].def);
    if (!setup(s, 44100)) { free(s); return NULL; }
    load_program(s, 0);
    memset((void *)s->automate, 0, sizeof s->automate);
    for (int m = 0; m < 4; m++) s->on_s[m] = s->norm[MOD(m, M_ON)] > 0.5f ? 1 : 0;
    s->out_s = db(val(s, kLevel)); s->mix_s = val(s, kMix) / 100; s->sens_s = db(val(s, kSens));
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
    e->uniqueID = 0x4348524d;   /* 'CHRM' */
    e->version = 1000;
    return e;
}
__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) { return pl_wrap(master, make, &LISTS); }

#ifdef VC_PARAMS
int main(void) { write_params_json("Chr0ma Console", P, kNumParams, &LISTS); return 0; }
#endif
