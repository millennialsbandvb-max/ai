/* enzo_test: checks Enz0 X offline (no MPC needed).   enzo_test <Enz0-X.so>   exit 0 = PASSED
 * Every program plays a guitar line clean (finite, under the ceiling, not silent); Mono plays the tracked note
 * (A 220 -> ~220 Hz; Pitch +12 -> ~440 Hz); Poly with Pitch -12 an octave down; Arp moves the pitch; Hold keeps
 * sounding after the playing stops; Dry passes the input; Mix 0 = the dry input; random settings stay clean; CPU. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../nam-mpc/src/vst2.h"

#define FS 48000
#define N 128
static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL  " __VA_ARGS__); printf("\n"); failures++; } } while (0)
static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) { (void)e; (void)i; (void)v; (void)p; (void)o; return op == 1 ? 2400 : 0; }
static AEffect *e;
static int param(const char *name) {
    for (int i = 0; i < e->numParams; i++) { char t[64] = {0}; e->dispatcher(e, effGetParamName, i, 0, t, 0); if (!strcmp(t, name)) return i; }
    printf("  (no parameter %s)\n", name); failures++; return 0;
}
static void disp(int i, char *t) { memset(t, 0, 64); e->dispatcher(e, effGetParamDisplay, i, 0, t, 0); }
static void set_text(const char *name, const char *want) {
    const int i = param(name);
    for (int k = 0; k <= 400; k++) { e->setParameter(e, i, (float)k / 400); char t[64]; disp(i, t); if (!strcmp(t, want)) return; }
    printf("  (%s can't show %s)\n", name, want); failures++;
}
static float L[12 * FS], R[12 * FS];
static float g_hz = 220, g_amp = 0.3f;
static float pluck(long t) {   /* a plucked, harmonic-rich note, restruck every second */
    const long u = t % FS;
    const double ph = 2 * M_PI * g_hz * (double)t / FS;
    return g_amp * expf(-(float)u / (0.6f * FS)) * (float)(sin(ph) + 0.5 * sin(2 * ph) + 0.25 * sin(3 * ph));
}
static float chord(long t) { const double a = 2 * M_PI * (double)t / FS; return 0.15f * (float)(sin(a * 196) + sin(a * 246.9) + sin(a * 293.7)); }
static int run(float (*f)(long), float secs, float *peak) {
    const long n = (long)(secs * FS);
    float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
    int ok = 1;
    *peak = 0;
    for (long b = 0; b < n; b += N) {
        for (int k = 0; k < N; k++) il[k] = ir[k] = f ? f(b + k) : 0;
        e->processReplacing(e, in, out, N);
        for (int k = 0; k < N && b + k < n; k++) {
            if (!isfinite(ol[k]) || !isfinite(orr[k])) ok = 0;
            L[b + k] = ol[k]; R[b + k] = orr[k];
            if (fabsf(ol[k]) > *peak) *peak = fabsf(ol[k]);
        }
    }
    return ok;
}
static float pitch_of(long from, long len) {   /* the fundamental by autocorrelation (shortest lag near the best) */
    double best = -1;
    int bl = 0;
    double c[1200];
    for (int lag = 40; lag < 1200; lag++) {
        double s = 0, e0 = 0, e1 = 0;
        for (long i = from; i < from + len; i++) { s += L[i] * L[i + lag]; e0 += L[i] * L[i]; e1 += L[i + lag] * L[i + lag]; }
        c[lag] = s / sqrt(e0 * e1 + 1e-20);
        if (c[lag] > best) { best = c[lag]; bl = lag; }
    }
    for (int lag = 40; lag < bl; lag++)
        if (c[lag] > best * 0.92 && c[lag] >= c[lag - 1] && c[lag] >= c[lag + 1]) { bl = lag; break; }
    return bl ? (float)FS / (float)bl : 0;
}
static float rms(long from, long len) { double a = 0; for (long i = from; i < from + len; i++) a += L[i] * L[i]; return (float)sqrt(a / (double)len); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: enzo_test <Enz0-X.so>\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    AEffect *(*entry)(audioMasterCallback) = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, FS);
    e->dispatcher(e, effSetBlockSize, 0, N, NULL, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    char name[64], t[64];
    e->dispatcher(e, effGetEffectName, 0, 0, name, 0);
    printf("%s, %d parameters\n", name, e->numParams);
    float peak;
    /* every program */
    const int prog = param("Program"), next = param("Program >");
    char first[64];
    disp(prog, first);
    for (int k = 0; k < 40; k++) {
        disp(prog, t);
        const int ok = run(pluck, 3, &peak);
        printf("  %-18s peak %.2f\n", t, peak);
        CHECK(ok && peak < 4.01f && peak > 0.01f, "program \"%s\" doesn't play clean", t);
        e->setParameter(e, next, 1); e->setParameter(e, next, 0);
        disp(prog, t);
        if (!strcmp(t, first)) break;
    }
    /* Mono follows the note */
    set_text("Mode", "Mono"); set_text("Filter Type", "Low Pass");
    e->setParameter(e, param("Cutoff"), 1); e->setParameter(e, param("Env Amount"), 0.5f);
    e->setParameter(e, param("Mod Depth"), 0); e->setParameter(e, param("Delay Level"), 0); e->setParameter(e, param("Reverb"), 0);
    e->setParameter(e, param("Mix"), 1); e->setParameter(e, param("Sub"), 0); e->setParameter(e, param("Hold"), 0);
    set_text("Pitch", "+0 st");
    g_hz = 220;
    run(pluck, 2, &peak);
    float f0 = pitch_of(FS + 2000, 4000);
    printf("  Mono, A 220: plays %.1f Hz\n", f0);
    CHECK(fabsf(f0 - 220) < 6, "Mono doesn't play the note");
    set_text("Pitch", "+12 st");
    run(pluck, 2, &peak);
    f0 = pitch_of(FS + 2000, 4000);
    printf("  Mono, Pitch +12: plays %.1f Hz\n", f0);
    CHECK(fabsf(f0 - 440) < 12, "Mono's Pitch doesn't transpose");
    /* Poly, an octave down, on a chord: the lowest note is about half */
    set_text("Mode", "Poly"); set_text("Pitch", "-12 st");
    g_hz = 220;
    run(pluck, 2, &peak);
    f0 = pitch_of(FS + 2000, 6000);
    printf("  Poly, Pitch -12 on A 220: %.1f Hz\n", f0);
    CHECK(fabsf(f0 - 110) < 6, "Poly doesn't shift an octave down");
    run(chord, 1, &peak);
    CHECK(peak > 0.01f, "Poly is silent on a chord");
    /* Arp moves the pitch */
    set_text("Mode", "Arp"); set_text("Pitch", "+0 st"); set_text("Arp Pattern", "Octaves");
    g_hz = 220;
    run(pluck, 2, &peak);
    float lo = 1e9f, hi = 0;
    for (long b = FS; b + 4000 < 2 * FS; b += FS / 12) { const float p = pitch_of(b, 1800); if (p > 50) { lo = fminf(lo, p); hi = fmaxf(hi, p); } }
    printf("  Arp Octaves on A 220: %.0f .. %.0f Hz\n", lo, hi);
    CHECK(hi > lo * 1.8f, "Arp doesn't step");
    /* Hold */
    set_text("Mode", "Mono"); set_text("Hold", "Held");
    run(NULL, 1, &peak);
    printf("  Hold, a second of silence in: rms %.4f\n", rms(FS / 2, FS / 2));
    CHECK(rms(FS / 2, FS / 2) > 0.005f, "Hold doesn't keep the note");
    set_text("Hold", "Off");
    run(NULL, 2, &peak);
    CHECK(rms(FS, FS) < 0.002f, "the synth doesn't stop after Hold");
    /* Dry passes the input (filter wide open, no envelope) */
    set_text("Mode", "Dry"); set_text("Filter Type", "Low Pass"); e->setParameter(e, param("Cutoff"), 1);
    e->setParameter(e, param("Env Amount"), 0.5f);
    run(pluck, 1, &peak);
    printf("  Dry: peak %.2f\n", peak);
    CHECK(peak > 0.1f, "Dry doesn't pass the input");
    /* random settings stay clean */
    srand(3);
    int bad = 0;
    for (int r = 0; r < 60; r++) {
        for (int i = 3; i < e->numParams && i < 28; i++) e->setParameter(e, i, (float)rand() / RAND_MAX);
        g_hz = 80 + 400 * (float)rand() / RAND_MAX;
        if (!run(r & 1 ? chord : pluck, 1, &peak) || peak > 4.01f) bad++;
    }
    printf("  60 random settings: %d bad\n", bad);
    CHECK(!bad, "random settings go bad");
    /* CPU: Mono with delay and reverb */
    {
        set_text("Mode", "Poly"); e->setParameter(e, param("Delay Level"), 0.4f); e->setParameter(e, param("Reverb"), 0.4f);
        float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int b = 0; b < 2000; b++) { for (int k = 0; k < N; k++) il[k] = ir[k] = pluck((long)b * N + k); e->processReplacing(e, in, out, N); }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / 2000;
        printf("  CPU: %.2f%% of real time (this machine)\n", 100 * ms / (1000.0 * N / FS));
    }
    e->dispatcher(e, effClose, 0, 0, NULL, 0);
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
