/* eq_test: checks GQ31 Graphic and X-Para EQ offline by measuring their response with sine tones.
 *   eq_test <GQ31-Graphic.so> <X-Para-EQ.so>   exit 0 = PASSED */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../nam-mpc/src/vst2.h"

#define FS 48000
static int failures;
static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) { (void)e; (void)i; (void)v; (void)p; (void)o; return op == 1 ? 2400 : 0; }
static AEffect *make(const char *so) {
    void *h = dlopen(so, RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); exit(1); }
    AEffect *(*entry)(audioMasterCallback) = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    AEffect *e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, FS);
    e->dispatcher(e, effSetBlockSize, 0, 128, NULL, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    return e;
}
static int param(AEffect *e, const char *want) {
    for (int i = 0; i < e->numParams; i++) { char t[64] = {0}; e->dispatcher(e, effGetParamName, i, 0, t, 0); if (!strcmp(t, want)) return i; }
    printf("  (no parameter %s)\n", want); failures++; return -1;
}
static double num(const char *t) { char *end; double v = strtod(t, &end); return strstr(end, "kHz") ? v * 1000 : v; }
/* a parameter to the value showing nearest `v` (or the exact text, for options) */
static void set(AEffect *e, const char *name, double v, const char *text) {
    const int i = param(e, name);
    if (i < 0) return;
    if (text) {   /* an option: exact steps (a value between them is a Q-Link nudge) */
        for (int n = 2; n <= 64; n++)
            for (int j = 0; j < n; j++) {
                char t[64] = {0};
                e->setParameter(e, i, (float)j / (float)(n - 1));
                e->dispatcher(e, effGetParamDisplay, i, 0, t, 0);
                if (!strcmp(t, text) && fabsf(e->getParameter(e, i) - (float)j / (float)(n - 1)) < 1e-6f) return;
            }
        printf("  (can't set %s to %s)\n", name, text); failures++;
        return;
    }
    float best = 0;
    double bd = 1e30;
    for (int k = 0; k <= 4000; k++) {
        char t[64] = {0};
        e->setParameter(e, i, (float)k / 4000);
        e->dispatcher(e, effGetParamDisplay, i, 0, t, 0);
        const double d = fabs(num(t) - v);
        if (d < bd) { bd = d; best = (float)k / 4000; }
    }
    e->setParameter(e, i, best);
}
static double gain_at(AEffect *e, double hz) {   /* dB, left channel, 0.1 amplitude */
    float il[128], ir[128], ol[128], orr[128], *in[2] = {il, ir}, *out[2] = {ol, orr};
    double si = 0, so = 0;
    for (long t = 0; t < FS * 3 / 4; t += 128) {
        for (int k = 0; k < 128; k++) il[k] = ir[k] = 0.1f * (float)sin(2 * M_PI * hz * (double)(t + k) / FS);
        e->processReplacing(e, in, out, 128);
        if (t >= FS / 4) for (int k = 0; k < 128; k++) { si += (double)il[k] * il[k]; so += (double)ol[k] * ol[k]; }
    }
    return 10 * log10(so / si);
}
#define NEAR(e, hz, want, tol, what) do { const double g_ = gain_at(e, hz); printf("  %-34s %7.0f Hz: %+6.2f dB (want %+.1f)\n", what, (double)(hz), g_, (double)(want)); \
    if (fabs(g_ - (want)) > (tol)) { printf("  FAIL\n"); failures++; } } while (0)

static void stability(AEffect *e, const char *what) {   /* random settings and a NaN: finite, under the ceiling */
    float il[128], ir[128], ol[128], orr[128], *in[2] = {il, ir}, *out[2] = {ol, orr};
    srand(5);
    for (int trial = 0; trial < 30; trial++) {
        for (int i = 0; i < e->numParams; i++) e->setParameter(e, i, (float)rand() / RAND_MAX);
        float peak = 0;
        int ok = 1;
        for (long t = 0; t < FS / 4; t += 128) {
            for (int k = 0; k < 128; k++) il[k] = ir[k] = (t == 0 && k == 5) ? NAN : 0.5f * ((float)rand() / RAND_MAX * 2 - 1);
            e->processReplacing(e, in, out, 128);
            for (int k = 0; k < 128; k++) { if (!isfinite(ol[k]) || !isfinite(orr[k])) ok = 0; if (fabsf(ol[k]) > peak) peak = fabsf(ol[k]); }
        }
        if (!ok || peak > 4.0f) { printf("  FAIL  %s random settings %d: %s (peak %.2f)\n", what, trial, ok ? "ceiling" : "not finite", peak); failures++; }
    }
    printf("  %s: 30 random settings with noise and a NaN: finite, under the ceiling\n", what);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: eq_test <GQ31-Graphic.so> <X-Para-EQ.so>\n"); return 2; }
    printf("GQ31 Graphic\n");
    AEffect *g = make(argv[1]);
    for (int k = 0; k < 6; k++) { static const double F[] = {30, 100, 440, 1000, 5000, 15000}; NEAR(g, F[k], 0, 0.05, "flat"); }
    set(g, "1k", 12, NULL);
    NEAR(g, 1000, 12, 0.6, "1k at +12");
    NEAR(g, 2000, 0, 1.5, "1k at +12, an octave up");
    NEAR(g, 500, 0, 1.5, "1k at +12, an octave down");
    set(g, "Range", 0, "+/-6 dB");
    NEAR(g, 1000, 6, 0.4, "1k at +12, Range +/-6");
    set(g, "Range", 0, "+/-12 dB");
    set(g, "1k", 0, NULL);
    set(g, "100", -12, NULL);
    NEAR(g, 100, -12, 0.6, "100 at -12");
    g->setParameter(g, param(g, "Flat"), 1);
    NEAR(g, 100, 0, 0.05, "Flat");
    set(g, "High-Pass", 200, NULL);
    NEAR(g, 100, -18, 2.5, "High-Pass 200 Hz, an octave below");
    NEAR(g, 2000, 0, 0.3, "High-Pass 200 Hz, passband");
    set(g, "High-Pass", 20, NULL);
    set(g, "Low-Pass", 4000, NULL);
    NEAR(g, 8000, -18, 2.5, "Low-Pass 4 kHz, an octave above");
    set(g, "Low-Pass", 20000, NULL);
    set(g, "Level", -6, NULL);
    NEAR(g, 1000, -6, 0.1, "Level -6");
    set(g, "Level", 0, NULL);
    stability(g, "GQ31");

    printf("X-Para EQ\n");
    AEffect *x = make(argv[2]);
    for (int k = 0; k < 6; k++) { static const double F[] = {30, 100, 440, 1000, 5000, 15000}; NEAR(x, F[k], 0, 0.05, "defaults (bands at 0 dB)"); }
    set(x, "2 Freq", 500, NULL);
    set(x, "2 Gain", 12, NULL);
    NEAR(x, 500, 12, 0.5, "band 2 PEQ 500 Hz +12, Q 2");
    NEAR(x, 2000, 0, 1.0, "band 2 PEQ, two octaves up");
    const double peq_wide = gain_at(x, 1000);
    set(x, "2 Type", 0, "VEQ");
    set(x, "2 Gain", 4, NULL);
    const double veq4 = gain_at(x, 1000);
    set(x, "2 Type", 0, "PEQ");
    const double peq4 = gain_at(x, 1000);
    printf("  an octave above a +4 dB band: VEQ %+.2f dB, PEQ %+.2f dB (VEQ broader when gentle; +12 PEQ %+.2f)\n", veq4, peq4, peq_wide);
    if (!(veq4 > peq4 + 0.3)) { printf("  FAIL\n"); failures++; }
    set(x, "2 Gain", 0, NULL);
    set(x, "1 Gain", 6, NULL);
    NEAR(x, 25, 6, 0.6, "band 1 LShv 100 Hz +6");
    set(x, "1 Gain", 0, NULL);
    set(x, "4 Type", 0, "HCut");
    set(x, "4 Freq", 1000, NULL);
    NEAR(x, 4000, -24, 2.0, "band 4 HCut 1 kHz, two octaves above");
    set(x, "4 Type", 0, "HShv");
    set(x, "Low Cut", 0, "On");
    set(x, "Low Cut Freq", 200, NULL);
    NEAR(x, 50, -24, 2.0, "Low Cut 200 Hz, two octaves below");
    set(x, "EQ", 0, "Off");
    for (int w = 0; w < 4; w++) gain_at(x, 50);
    NEAR(x, 50, 0, 0.1, "EQ off");
    stability(x, "X-Para");
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
