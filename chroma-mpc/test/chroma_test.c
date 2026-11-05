/* chroma_test: checks Chr0ma Console offline (no MPC needed).   chroma_test <Chr0ma-Console.so>   exit 0 = PASSED
 * Every effect of every module, alone, at Amount/Tweak 0, 100 and random, plays a guitar line clean (finite, under
 * the ceiling, not silent) and rings out to silence; every program plays clean; the Tweak knob's name follows the
 * effect; all modules off and Tilt 0 = the input; MIDI CC 102 switches Character off; random settings; CPU. */
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
static float L[15 * FS], IN[15 * FS];
static float music(long t) {
    static const float NOTES[6] = {110, 146.8f, 196, 246.9f, 329.6f, 220};
    const long n = t / (FS / 3), u = t % (FS / 3);
    const double ph = 2 * M_PI * NOTES[n % 6] * (double)t / FS;
    return 0.3f * expf(-(float)u / (0.25f * FS)) * (float)(sin(ph) + 0.4 * sin(2 * ph) + 0.2 * sin(3 * ph));
}
static int run(float secs, int sound, float *peak, float *tail) {
    const long n = (long)(secs * FS);
    float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
    int ok = 1;
    *peak = 0;
    if (tail) *tail = 0;
    for (long b = 0; b < n; b += N) {
        for (int k = 0; k < N; k++) il[k] = ir[k] = sound ? music(b + k) : 0;
        e->processReplacing(e, in, out, N);
        for (int k = 0; k < N && b + k < n; k++) {
            if (!isfinite(ol[k]) || !isfinite(orr[k])) ok = 0;
            L[b + k] = ol[k]; IN[b + k] = il[k];
            if (fabsf(ol[k]) > *peak) *peak = fabsf(ol[k]);
            if (tail && b + k > n - FS / 2 && fabsf(ol[k]) > *tail) *tail = fabsf(ol[k]);
        }
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: chroma_test <Chr0ma-Console.so>\n"); return 2; }
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
    static const char *const MODS[4] = {"Character", "Movement", "Diffusion", "Texture"};
    int type[4], amt[4], tw[4], on[4];
    for (int m = 0; m < 4; m++) {
        char k[64];
        type[m] = param(MODS[m]);
        snprintf(k, sizeof k, "%s Amount", MODS[m]); amt[m] = param(k);
        tw[m] = type[m] + 2;   /* (its name follows the effect) */
        snprintf(k, sizeof k, "%s On", MODS[m]); on[m] = param(k);
    }
    const int tilt = param("Tilt"), mix = param("Mix"), level = param("Level"), sens = param("Sensitivity");
    float peak, tail;
    /* every effect, alone */
    int bad = 0;
    for (int m = 0; m < 4; m++) {
        for (int x = 0; x < 4; x++) e->setParameter(e, on[x], x == m);
        e->setParameter(e, tilt, 0.5f); e->setParameter(e, mix, 1); e->setParameter(e, level, 24.0f / 36); e->setParameter(e, sens, 0.5f);
        for (int ty = 0; ty < 5; ty++) {
            e->setParameter(e, type[m], ty / 4.0f);
            disp(type[m], t);
            char tn[64] = {0};
            e->dispatcher(e, effGetParamName, tw[m], 0, tn, 0);
            const float sets[4][2] = {{0, 0}, {1, 1}, {0.5f, 0.5f}, {(float)rand() / RAND_MAX, (float)rand() / RAND_MAX}};
            float worst = 0;
            int fine = 1;
            for (int sset = 0; sset < 4; sset++) {
                e->setParameter(e, amt[m], sets[sset][0]); e->setParameter(e, tw[m], sets[sset][1]);
                e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
                if (!run(3, 1, &peak, NULL) || peak > 4.01f || peak < 0.001f) fine = 0;
                worst = fmaxf(worst, peak);
                run(m == 2 ? 14 : 6, 0, &peak, &tail);   /* (Diffusion's longest echoes and halls take a while) */
                if (tail > 0.01f) { fine = 0; printf("  %s %s doesn't ring out (%.3f)\n", MODS[m], t, tail); }
            }
            printf("  %-9s %-12s (tweak: %-9s) peak %.2f%s\n", MODS[m], t, tn, worst, fine ? "" : "  BAD");
            if (!fine) bad++;
        }
    }
    CHECK(!bad, "an effect plays badly");
    /* all off, flat: the input itself */
    for (int x = 0; x < 4; x++) e->setParameter(e, on[x], 0);
    e->setParameter(e, tilt, 0.5f);
    run(2, 1, &peak, NULL);
    double err = 0;
    for (long i = FS; i < 2 * FS; i++) err = fmax(err, fabs(L[i] - IN[i]));
    printf("  all modules off: largest difference from the input %.5f\n", err);
    CHECK(err < 0.01, "all off isn't the input");
    /* CC 102 switches Character */
    {
        struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char d[4]; char a, b, c, x; } ev = {1, 32, 0, 0, 0, 0, {0xb0, 102, 127, 0}, 0, 0, 0, 0};
        struct { int32_t n; intptr_t r; void *ev[2]; } evs = {1, 0, {&ev, NULL}};
        e->dispatcher(e, 25, 0, 0, &evs, 0);
        CHECK(e->getParameter(e, on[0]) > 0.5f, "CC 102 doesn't switch Character on");
    }
    /* every program */
    const int prog = param("Program"), next = param("Program >");
    char first[64];
    disp(prog, first);
    bad = 0;
    for (int k = 0; k < 40; k++) {
        disp(prog, t);
        const int ok = run(4, 1, &peak, NULL);
        printf("  program %-18s peak %.2f\n", t, peak);
        if (!ok || peak > 4.01f || peak < 0.01f) bad++;
        e->setParameter(e, next, 1); e->setParameter(e, next, 0);
        disp(prog, t);
        if (!strcmp(t, first)) break;
    }
    CHECK(!bad, "a program plays badly");
    /* random settings */
    srand(7);
    bad = 0;
    for (int r = 0; r < 80; r++) {
        for (int i = 3; i < 23; i++) e->setParameter(e, i, (float)rand() / RAND_MAX);
        if (!run(1.5f, 1, &peak, NULL) || peak > 4.01f) bad++;
    }
    printf("  80 random settings: %d bad\n", bad);
    CHECK(!bad, "random settings go bad");
    /* CPU: everything on (Fuzz, Pitch, Space, Cassette) */
    {
        for (int x = 0; x < 4; x++) e->setParameter(e, on[x], 1);
        e->setParameter(e, type[0], 0.5f); e->setParameter(e, type[1], 1); e->setParameter(e, type[2], 0.5f); e->setParameter(e, type[3], 0.5f);
        float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int b = 0; b < 2000; b++) { for (int k = 0; k < N; k++) il[k] = ir[k] = music((long)b * N + k); e->processReplacing(e, in, out, N); }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / 2000;
        printf("  CPU, four modules: %.2f%% of real time (this machine)\n", 100 * ms / (1000.0 * N / FS));
    }
    e->dispatcher(e, effClose, 0, 0, NULL, 0);
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
