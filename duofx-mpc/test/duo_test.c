/* duo_test: checks Duo FX offline (no MPC needed).
 *   duo_test <Duo-FX.so>        exit status 0 = PASSED
 * For each algorithm, in each slot and both routings, with the knobs at their defaults and at their extremes:
 * 20 s of sound then silence stays finite, under the +12 dB ceiling and (with feedback below the top) dies away.
 * Then what each algorithm should do: Delay's echo arrives at the Time it shows, Pitch and Shimmer move a tone by
 * the Pitch knob, Detune stays close, the reverbs leave a tail, Tremolo changes the level, Phaser colours noise.
 * Also times each algorithm (128-frame blocks at 44.1 kHz). */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../nam-mpc/src/vst2.h"

enum { NALGO = 12, P_SLOT = 8, ROUTING = 16, OUTPUT = 17 };
enum { P_ALGO, P_MIX, P_TIME, P_FEEDBACK, P_TONE, P_MOD, P_RATE, P_PITCH };
static const char *NAMES[NALGO] = {"Off", "Delay", "Echo", "Reverse", "Detune", "Pitch", "Chorus", "Phaser",
                                   "Tremolo", "Room", "Shimmer", "Abyss"};
#define FS 44100
#define N 128
static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) { (void)e; (void)op; (void)i; (void)v; (void)p; (void)o; return 0; }
static AEffect *(*entry)(audioMasterCallback);

static AEffect *make(void) {
    AEffect *e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, FS);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    return e;
}
static void algo(AEffect *e, int slot, int a) { e->setParameter(e, slot * P_SLOT + P_ALGO, (float)a / (NALGO - 1)); }
static void knob(AEffect *e, int slot, int p, float v) { e->setParameter(e, slot * P_SLOT + p, v); }

/* render `secs` of input from gen() into a buffer (left channel kept); -> 0 if anything wasn't finite */
static int render(AEffect *e, float secs, float (*gen)(long), float *keep_l, float *peak) {
    float il[N], ir[N], ol[N], orr[N];
    float *in[2] = {il, ir}, *out[2] = {ol, orr};
    const long total = (long)(secs * FS);
    *peak = 0;
    for (long t = 0; t < total; t += N) {
        for (int k = 0; k < N; k++) il[k] = ir[k] = gen(t + k);
        e->processReplacing(e, in, out, N);
        for (int k = 0; k < N; k++) {
            if (!isfinite(ol[k]) || !isfinite(orr[k])) return 0;
            const float a = fabsf(ol[k]) > fabsf(orr[k]) ? fabsf(ol[k]) : fabsf(orr[k]);
            if (a > *peak) *peak = a;
            if (keep_l && t + k < total) keep_l[t + k] = ol[k];
        }
    }
    return 1;
}

static unsigned rng = 1;
static float noise(void) { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 8388608.0f - 1; }
static float g_sound_then_silence(long t) { return t < 20 * FS ? 0.5f * sinf(2 * (float)M_PI * 220 * (float)t / FS) + 0.2f * noise() : 0; }
static float g_impulse(long t) { return t == 0 ? 1.0f : 0; }
static float g_sine220(long t) { return 0.5f * sinf(2 * (float)M_PI * 220 * (float)t / FS); }
static float g_noise(long t) { (void)t; return 0.3f * noise(); }

static double rms(const float *x, long a, long b) { double s = 0; for (long i = a; i < b; i++) s += x[i] * x[i]; return sqrt(s / (double)(b - a)); }
static double goertzel1(const float *x, long a, long b, double f) {
    const double w = 2 * M_PI * f / FS, c = 2 * cos(w);
    double s1 = 0, s2 = 0;
    for (long i = a; i < b; i++) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (double)(b - a);
}
/* the level of frequency f in x[a..b), averaged over 8192-sample windows (a pitch shifter's grains don't keep
 * their phase from one to the next, so one long window would average its tone away) */
static double goertzel(const float *x, long a, long b, double f) {
    double sum = 0;
    int n = 0;
    for (long i = a; i + 8192 <= b; i += 8192, n++) sum += goertzel1(x, i, i + 8192, f);
    return n ? sum / n : 0;
}
static float g_zero(long t) { (void)t; return 0; }
static void settle(AEffect *e) { float p; render(e, 0.5f, g_zero, NULL, &p); }   /* let the knobs glide into place */

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <Duo-FX.so>\n", argv[0]); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h || !(*(void **)&entry = dlsym(h, "VSTPluginMain"))) { fprintf(stderr, "can't load %s\n", argv[1]); return 1; }
    static float buf[45 * FS];

    printf("stability: every algorithm, both slots, both routings, knobs at defaults / minimum / maximum\n");
    for (int a = 1; a < NALGO; a++)
        for (int slot = 0; slot < 2; slot++)
            for (int route = 0; route < 2; route++)
                for (int setting = 0; setting < 3; setting++) {
                    AEffect *e = make();
                    algo(e, 0, 0); algo(e, 1, 0);
                    algo(e, slot, a);
                    e->setParameter(e, ROUTING, (float)route);
                    if (setting) for (int p = 1; p < P_SLOT; p++) knob(e, slot, p, setting == 1 ? 0.0f : 1.0f);
                    if (setting == 2) knob(e, slot, P_FEEDBACK, 0.9f);   /* (a full Feedback may hold forever) */
                    float peak;
                    const int ok = render(e, 45, g_sound_then_silence, buf, &peak);
                    CHECK(ok, "%s slot %c %s %s: not finite", NAMES[a], 'A' + slot, route ? "parallel" : "series",
                          setting == 0 ? "defaults" : setting == 1 ? "min" : "max");
                    CHECK(peak < 3.99f, "%s slot %c %s %s: hits the +12 dB ceiling (peak %.2f)", NAMES[a], 'A' + slot,
                          route ? "parallel" : "series", setting == 0 ? "defaults" : setting == 1 ? "min" : "max", peak);
                    const double end = rms(buf, 44 * FS, 45 * FS), before = rms(buf, 34 * FS, 35 * FS);
                    /* with the input silent for 25 s it should be (nearly) gone, or (long repeats/decays at the top
                     * of Feedback) clearly dying away */
                    CHECK(end < 0.002 || (setting == 2 && end < 0.6 * before), "%s slot %c %s %s: still %.4f rms 24 s after the input stopped", NAMES[a],
                          'A' + slot, route ? "parallel" : "series", setting == 0 ? "defaults" : setting == 1 ? "min" : "max", end);
                    e->dispatcher(e, effClose, 0, 0, NULL, 0);
                }

    printf("behaviour\n");
    {   /* Delay: an impulse comes back after the Time shown (default 200 ms) */
        AEffect *e = make();
        algo(e, 1, 0); algo(e, 0, 1); knob(e, 0, P_MIX, 1.0f); knob(e, 0, P_FEEDBACK, 0); knob(e, 0, P_MOD, 0);
        char t[64] = {0};
        e->dispatcher(e, effGetParamDisplay, P_TIME, 0, t, 0);
        float peak;
        settle(e);
        render(e, 1, g_impulse, buf, &peak);
        long at = 0; float best = 0;
        for (long i = 0; i < FS; i++) if (fabsf(buf[i]) > best) { best = fabsf(buf[i]); at = i; }
        printf("  Delay: Time shows %s, echo at %.1f ms\n", t, 1000.0 * at / FS);
        CHECK(fabs(1000.0 * at / FS - atof(t)) < 3, "Delay's echo isn't at the Time it shows");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    for (int a = 5; a <= 10; a += 5) {   /* Pitch and Shimmer at +12: a 220 Hz tone gains 440 Hz */
        AEffect *e = make();
        algo(e, 1, 0); algo(e, 0, a); knob(e, 0, P_MIX, 1.0f); knob(e, 0, P_FEEDBACK, a == 5 ? 0 : 0.5f);
        knob(e, 0, P_MOD, a == 5 ? 0 : 0.8f); knob(e, 0, P_TIME, a == 5 ? 0 : 0.5f); knob(e, 0, P_PITCH, 0.75f);
        float peak;
        settle(e);
        render(e, 4, g_sine220, buf, &peak);
        const double f440 = goertzel(buf, 2 * FS, 4 * FS, 440), f220 = goertzel(buf, 2 * FS, 4 * FS, 220);
        printf("  %s +12: 440 Hz %.4f, 220 Hz %.4f\n", NAMES[a], f440, f220);
        CHECK(a == 5 ? f440 > 4 * f220 : f440 > 0.05 * f220, "%s doesn't move the tone up an octave", NAMES[a]);
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    {   /* Detune: stays close to the input's pitch */
        AEffect *e = make();
        algo(e, 1, 0); algo(e, 0, 4); knob(e, 0, P_MIX, 1.0f);
        float peak;
        settle(e);
        render(e, 3, g_sine220, buf, &peak);
        const double f220 = goertzel(buf, FS, 3 * FS, 220), f233 = goertzel(buf, FS, 3 * FS, 250);
        printf("  Detune: 220 Hz %.4f, 250 Hz %.4f\n", f220, f233);
        CHECK(f220 > 3 * f233, "Detune moves too far");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    for (int a = 9; a <= 11; a++) {   /* the reverbs leave a tail */
        AEffect *e = make();
        algo(e, 1, 0); algo(e, 0, a); knob(e, 0, P_MIX, 1.0f);
        float peak;
        settle(e);
        render(e, 2, g_impulse, buf, &peak);
        const double tail = rms(buf, FS / 2, FS), late = rms(buf, (long)(1.5 * FS), 2 * FS);
        printf("  %s: tail 0.5-1 s %.5f, 1.5-2 s %.5f\n", NAMES[a], tail, late);
        CHECK(tail > 1e-4, "%s leaves no tail", NAMES[a]);
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    {   /* Reverse: sound comes out, delayed, after a note */
        AEffect *e = make();
        algo(e, 1, 0); algo(e, 0, 3); knob(e, 0, P_MIX, 1.0f); knob(e, 0, P_PITCH, 0.5f);
        float peak;
        settle(e);
        render(e, 3, g_sine220, buf, &peak);
        const double level = rms(buf, FS, 3 * FS), f220 = goertzel(buf, FS, 3 * FS, 220);
        printf("  Reverse (0 st): rms %.4f, 220 Hz %.4f\n", level, f220);
        CHECK(level > 0.05 && f220 > 0.02, "Reverse doesn't play the input back");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    {   /* Tremolo: the level swings */
        AEffect *e = make();
        algo(e, 1, 0); algo(e, 0, 8); knob(e, 0, P_MIX, 1.0f); knob(e, 0, P_MOD, 1.0f); knob(e, 0, P_RATE, 0.6f);
        float peak;
        settle(e);
        render(e, 3, g_sine220, buf, &peak);
        double lo = 1, hi = 0;
        for (long i = FS; i + 441 < 3 * FS; i += 441) { const double r = rms(buf, i, i + 441); if (r < lo) lo = r; if (r > hi) hi = r; }
        printf("  Tremolo: level %.3f .. %.3f\n", lo, hi);
        CHECK(hi > 4 * lo, "Tremolo doesn't change the level");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    {   /* Phaser, Chorus: the sound changes (wet differs from dry) */
        for (int a = 6; a <= 7; a++) {
            AEffect *e = make();
            algo(e, 1, 0); algo(e, 0, a); knob(e, 0, P_MIX, 1.0f); knob(e, 0, P_MOD, 0.8f);
            float peak;
            settle(e);
            rng = 7;
            render(e, 2, g_noise, buf, &peak);
            rng = 7;
            double diff = 0;
            for (long i = 0; i < 2 * FS; i++) { const float d = buf[i] - g_noise(i); diff += d * d; }
            diff = sqrt(diff / (2.0 * FS));
            printf("  %s: differs from the input by %.3f rms\n", NAMES[a], diff);
            CHECK(diff > 0.05, "%s doesn't change the sound", NAMES[a]);
            e->dispatcher(e, effClose, 0, 0, NULL, 0);
        }
    }

    printf("CPU (128-frame blocks, this machine)\n");
    for (int a = 1; a < NALGO; a++) {
        AEffect *e = make();
        algo(e, 1, 0); algo(e, 0, a);
        float il[N], ir[N], ol[N], orr[N];
        float *in[2] = {il, ir}, *out[2] = {ol, orr};
        for (int k = 0; k < N; k++) il[k] = ir[k] = 0.3f * noise();
        const int blocks = 3000;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int b = 0; b < blocks; b++) e->processReplacing(e, in, out, N);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / blocks;
        printf("  %-8s %.4f ms per block = %.2f%% of real time\n", NAMES[a], ms, 100 * ms / (1000.0 * N / FS));
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
