/* comp_test: checks Fairchyld 670 offline.   comp_test <Fairchyld-670.so>   exit 0 = PASSED
 * Quiet signals pass (almost) untouched, loud ones are turned down by about the right amount, more Bias compresses
 * harder, Time 1 lets go faster than Time 4, the GR meter follows, Mix 0 is the dry signal, and random settings with
 * noise and a NaN stay finite and under the ceiling. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../nam-mpc/src/vst2.h"

#define FS 48000
static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)
static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) { (void)e; (void)i; (void)v; (void)p; (void)o; return op == 1 ? 2400 : 0; }
static AEffect *(*entry)(audioMasterCallback);
static AEffect *make(void) {
    AEffect *e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, FS);
    e->dispatcher(e, effSetBlockSize, 0, 128, NULL, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    return e;
}
static int param(AEffect *e, const char *want) {
    for (int i = 0; i < e->numParams; i++) { char t[64] = {0}; e->dispatcher(e, effGetParamName, i, 0, t, 0); if (!strcmp(t, want)) return i; }
    printf("  (no parameter %s)\n", want); failures++; return 0;
}
static void set(AEffect *e, const char *name, float v) { e->setParameter(e, param(e, name), v); }
/* rms ratio (dB) of the last `meas` seconds of `secs` of a 1 kHz tone at `amp` */
static double run(AEffect *e, float amp, double secs, double meas) {
    float il[128], ir[128], ol[128], orr[128], *in[2] = {il, ir}, *out[2] = {ol, orr};
    double si = 0, so = 0;
    const long n = (long)(secs * FS), from = (long)((secs - meas) * FS);
    for (long t = 0; t < n; t += 128) {
        for (int k = 0; k < 128; k++) il[k] = ir[k] = amp * sinf(2 * (float)M_PI * 1000 * (float)(t + k) / FS);
        e->processReplacing(e, in, out, 128);
        if (t >= from) for (int k = 0; k < 128; k++) { si += (double)il[k] * il[k]; so += (double)ol[k] * ol[k]; }
    }
    return 10 * log10((so + 1e-20) / (si + 1e-20));
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: comp_test <Fairchyld-670.so>\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    entry = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    AEffect *e = make();
    double g = run(e, 0.003f, 1, 0.5);
    printf("  -50 dBFS tone, defaults: %+.2f dB\n", g);
    CHECK(fabs(g) < 0.3, "a quiet tone isn't left alone");
    set(e, "Threshold", 0.8f);   /* 8 of 10 */
    g = run(e, 0.5f, 2, 1);
    char t[64] = {0};
    e->dispatcher(e, effGetParamDisplay, param(e, "GR"), 0, t, 0);
    printf("  -6 dBFS tone, Threshold 8: %+.2f dB (GR meter %s)\n", g, t);
    CHECK(g < -6 && g > -25, "a loud tone isn't compressed about right");
    CHECK(strcmp(t, "-0.0 dB") && atof(t + 1) > 4, "the GR meter doesn't follow");
    set(e, "Bias", 1);
    const double hard = run(e, 0.5f, 2, 1);
    set(e, "Bias", 0);
    const double soft = run(e, 0.5f, 2, 1);
    printf("  Bias 10: %+.2f dB, Bias 0: %+.2f dB\n", hard, soft);
    CHECK(hard < soft - 1, "more Bias doesn't compress harder");
    set(e, "Bias", 0.5f);
    double rec[2];
    for (int k = 0; k < 2; k++) {   /* Time 1 vs Time 4: a loud second, then a quiet tone: how much is still turned down */
        set(e, "Time", k ? 3.0f / 5 : 0);
        run(e, 0.5f, 1.5, 0.1);
        rec[k] = run(e, 0.01f, 0.6, 0.2);
    }
    printf("  0.4 s after loud: Time 1 %+.2f dB, Time 4 %+.2f dB\n", rec[0], rec[1]);
    CHECK(rec[0] > rec[1] + 1, "Time 1 doesn't let go faster than Time 4");
    set(e, "Mix", 0);
    g = run(e, 0.5f, 1, 0.5);
    printf("  Mix 0: %+.2f dB\n", g);
    CHECK(fabs(g) < 0.1, "Mix 0 isn't the dry signal");
    e->dispatcher(e, effClose, 0, 0, NULL, 0);
    e = make();
    {   /* random settings, noise, a NaN */
        float il[128], ir[128], ol[128], orr[128], *in[2] = {il, ir}, *out[2] = {ol, orr};
        srand(9);
        for (int trial = 0; trial < 40; trial++) {
            for (int i = 0; i < e->numParams; i++) e->setParameter(e, i, (float)rand() / RAND_MAX);
            float peak = 0;
            int ok = 1;
            for (long tt = 0; tt < FS / 4; tt += 128) {
                for (int k = 0; k < 128; k++) il[k] = ir[k] = (tt == 0 && k == 3) ? NAN : 1.5f * ((float)rand() / RAND_MAX * 2 - 1);
                e->processReplacing(e, in, out, 128);
                for (int k = 0; k < 128; k++) { if (!isfinite(ol[k]) || !isfinite(orr[k])) ok = 0; if (fabsf(ol[k]) > peak) peak = fabsf(ol[k]); }
            }
            CHECK(ok && peak <= 4.0f, "random settings %d: %s (peak %.2f)", trial, ok ? "ceiling" : "not finite", peak);
        }
        printf("  40 random settings with loud noise and a NaN: finite, under the ceiling\n");
    }
    e->dispatcher(e, effClose, 0, 0, NULL, 0);
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
