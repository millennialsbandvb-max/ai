/* ergo_test: checks Ergo EUB offline (no MPC needed).   ergo_test <Ergo-EUB.so>   exit 0 = PASSED
 * (NAM_MPC_DIR=<folder with Models/ and IRs/> lets the amp find a model; without one it passes the sound as is.)
 * Every program loads (Program >) and plays a bass line clean; the EQ of each program measures as the handoff says
 * (the box cut at 496 Hz, the low-pass corner and slope, the B string's 31 Hz kept, the 60 Hz notch); the compressor
 * reduces gain and shows it; Arco passes a quiet bowed note (no gate anywhere); the halls ring after the note;
 * Volume and MIDI CC 11 (expression) work, 0 = silence; the state chunk brings everything back; CPU. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../nam-mpc/src/vst2.h"

#define FS 48000
#define N 128
static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL  " __VA_ARGS__); printf("\n"); failures++; } } while (0)
static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) { (void)e; (void)i; (void)v; (void)p; (void)o; return op == 1 ? 2400 : 0; }
static AEffect *(*entry)(audioMasterCallback);
static AEffect *make(void) {
    AEffect *e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, FS);
    e->dispatcher(e, effSetBlockSize, 0, N, NULL, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    return e;
}
static int param(AEffect *e, const char *name) {
    for (int i = 0; i < e->numParams; i++) { char t[64] = {0}; e->dispatcher(e, effGetParamName, i, 0, t, 0); if (!strcmp(t, name)) return i; }
    printf("  (no parameter %s)\n", name); failures++; return 0;
}
static void disp(AEffect *e, int i, char *t) { memset(t, 0, 64); e->dispatcher(e, effGetParamDisplay, i, 0, t, 0); }
static void set_text(AEffect *e, int i, const char *want) {   /* (options / toggles: exact values only) */
    for (int k = 0; k <= 20; k++) { e->setParameter(e, i, (float)k / 20); char t[64]; disp(e, i, t); if (!strcmp(t, want)) return; }
    printf("  (parameter %d can't show %s)\n", i, want); failures++;
}
/* run `secs` of input f(t) through; returns the output's RMS over the last half (dB) and its peak */
typedef float (*Sig)(long t);
static float g_hz, g_amp;
static float sine(long t) { return g_amp * sinf(2 * (float)M_PI * g_hz * (float)t / FS); }
static float bassline(long t) {   /* plucked B1, E1, A1, D2, G2 and back: the five strings */
    static const float NOTES[5] = {30.87f, 41.2f, 55, 73.42f, 98};
    const long n = t / (FS / 2), u = t % (FS / 2);
    return 0.4f * expf(-(float)u / (0.25f * FS)) * sinf(2 * (float)M_PI * NOTES[n % 5] * (float)t / FS);
}
static float out_l[12 * FS];
static int run(AEffect *e, Sig f, float secs, float *rms_db, float *peak) {
    const long n = (long)(secs * FS);
    float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
    double acc = 0;
    long cnt = 0;
    int ok = 1;
    *peak = 0;
    for (long b = 0; b < n; b += N) {
        for (int k = 0; k < N; k++) il[k] = ir[k] = f ? f(b + k) : 0;
        e->processReplacing(e, in, out, N);
        for (int k = 0; k < N && b + k < n; k++) {
            if (!isfinite(ol[k]) || !isfinite(orr[k])) ok = 0;
            out_l[b + k] = ol[k];
            if (fabsf(ol[k]) > *peak) *peak = fabsf(ol[k]);
            if (b + k >= n / 2) { acc += (double)ol[k] * ol[k]; cnt++; }
        }
    }
    *rms_db = (float)(10 * log10(acc / (double)(cnt ? cnt : 1) + 1e-20));
    return ok;
}
static float gain_at(AEffect *e, float hz) {   /* the output against the input, a quiet sine (dB) */
    float r, pk;
    g_hz = hz; g_amp = 0.01f;
    run(e, sine, hz < 50 ? 1.5f : 0.5f, &r, &pk);
    return r - (float)(20 * log10(0.01 / sqrt(2)));
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ergo_test <Ergo-EUB.so>\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    entry = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    AEffect *e = make();
    char name[64], t[64];
    e->dispatcher(e, effGetEffectName, 0, 0, name, 0);
    printf("%s, %d parameters\n", name, e->numParams);
    const int prog = param(e, "Program"), next = param(e, "Program >"), comp = param(e, "Comp"), amp = param(e, "Amp");
    const int gr = param(e, "GR"), vol = param(e, "Volume"), r1 = param(e, "Hall 1"), r2 = param(e, "Hall 2");
    float rms, peak;
    disp(e, prog, t);
    printf("  starts on \"%s\"\n", t);
    CHECK(!strcmp(t, "Ergo to Bass 1EQ"), "it doesn't start on Ergo to Bass 1EQ");

    /* every program: loads, and plays the five strings clean */
    char names[16][64];
    int np = 0;
    for (int k = 0; k < 16; k++) {
        e->setParameter(e, next, 1); e->setParameter(e, next, 0);
        disp(e, prog, t);
        if (np && !strcmp(t, names[0])) break;
        snprintf(names[np++], 64, "%s", t);
        const int ok = run(e, bassline, 5, &rms, &peak);
        char g[64]; disp(e, gr, g);
        printf("  %-18s peak %.2f, rms %.1f dB, GR %s\n", t, peak, rms, g);
        CHECK(ok && peak < 4.01f && peak > 0.001f, "\"%s\" doesn't play clean", t);
    }
    CHECK(np == 6, "%d programs, not 6", np);
    /* the EQs: measured with the compressor, the amp and the halls off */
    for (int p = 0; p < np; p++) {
        do { e->setParameter(e, next, 1); e->setParameter(e, next, 0); disp(e, prog, t); } while (strcmp(t, names[p]));
        set_text(e, comp, "Off"); set_text(e, amp, "Off"); set_text(e, r1, "Off"); set_text(e, r2, "Off");
        const float g31 = gain_at(e, 31), g60 = gain_at(e, 60), g100 = gain_at(e, 100), g496 = gain_at(e, 496);
        const float g794 = gain_at(e, 794), g1588 = gain_at(e, 1588), g398 = gain_at(e, 398), g3k = gain_at(e, 3000);
        printf("  %-18s EQ: 31 Hz %+.1f, 60 %+.1f, 100 %+.1f, 398 %+.1f, 496 %+.1f, 794 %+.1f, 1588 %+.1f, 3k %+.1f dB\n",
               names[p], g31, g60, g100, g398, g496, g794, g1588, g3k);
        const float in_db = !strcmp(names[p], "Ergo Arco") ? -9.6f : !strcmp(names[p], "Ergo Simple") ? 0.5f : strstr(names[p], "Pop") ? 3.4f + 8.2f : 0;
        CHECK(g31 - in_db > -6, "%s cuts the B string's 31 Hz (%.1f dB)", names[p], g31 - in_db);
        if (!strcmp(names[p], "Ergo Simple") || !strcmp(names[p], "Ergo Arco"))
            CHECK(g60 < g100 - 20, "%s: the 60 Hz notch is only %.1f dB", names[p], g100 - g60);
        if (!strcmp(names[p], "Ergo Simple")) CHECK(g794 < g100 - 20, "Simple's 24 dB low-pass at 398 Hz isn't there");
        if (!strcmp(names[p], "Ergo Arco")) CHECK(g1588 > g100 - 5 && g3k < g1588 - 10, "Arco's low-pass at 1784 Hz isn't right");
        if (strstr(names[p], "to Bass 1EQ") || strstr(names[p], "Pop Rock")) {
            /* 24 dB/oct at 794 Hz: about -3 dB there (and the peaks around), one octave up about -24 dB more */
            CHECK(g3k < g1588 - 18, "%s: the low-pass isn't 24 dB/oct (%.1f dB from 1588 Hz to 3 kHz)", names[p], g1588 - g3k);
            CHECK(g496 < g100 - 6, "%s: no box cut at 496 Hz", names[p]);
        }
        if (!strcmp(names[p], "Ergo to Bass")) CHECK(g1588 < g794 - 8 && g1588 > g794 - 20, "To Bass: the low-pass isn't 12 dB/oct");
    }
    /* the compressor: a loud note is turned down, and the GR shows it */
    do { e->setParameter(e, next, 1); e->setParameter(e, next, 0); disp(e, prog, t); } while (strcmp(t, "Ergo to Bass 1EQ"));
    g_hz = 98; g_amp = 0.5f;
    run(e, sine, 1, &rms, &peak);
    disp(e, gr, t);
    printf("  comp on a loud G string: GR %s\n", t);
    CHECK(atof(t + 1) > 3, "the compressor doesn't reduce gain on a loud note");
    /* Arco: a quiet bowed note (-50 dBFS) comes through whole (no gate anywhere) */
    do { e->setParameter(e, next, 1); e->setParameter(e, next, 0); disp(e, prog, t); } while (strcmp(t, "Ergo Arco"));
    g_hz = 41.2f; g_amp = 0.003f;
    run(e, sine, 3, &rms, &peak);
    float lo = 1e9f, hi = -1e9f;   /* its level, 100 ms at a time, over the last two seconds: steady (not chopped) */
    for (long b = FS; b + FS / 10 <= 3 * FS; b += FS / 10) {
        double a = 0;
        for (long k = b; k < b + FS / 10; k++) a += (double)out_l[k] * out_l[k];
        const float d = (float)(10 * log10(a / (FS / 10) + 1e-20));
        lo = fminf(lo, d); hi = fmaxf(hi, d);
    }
    printf("  Arco, a quiet bowed E: level %.1f .. %.1f dB\n", lo, hi);
    CHECK(lo > -90 && hi - lo < 3, "Arco chops or gates a quiet note");
    /* Pop Rock's halls ring on after the note */
    do { e->setParameter(e, next, 1); e->setParameter(e, next, 0); disp(e, prog, t); } while (strcmp(t, "Ergo Pop Rock"));
    g_hz = 73.42f; g_amp = 0.3f;
    run(e, sine, 1, &rms, &peak);
    run(e, NULL, 1, &rms, &peak);
    float tail = 0;
    for (long k = 0; k < FS / 4; k++) tail = fmaxf(tail, fabsf(out_l[k]));
    printf("  Pop Rock: the halls' tail after the note, peak %.3f\n", tail);
    CHECK(tail > 0.005f, "Pop Rock's halls don't ring");
    /* Volume: 0 = silence; MIDI CC 11 (expression) moves it */
    e->setParameter(e, vol, 0);
    g_hz = 55; g_amp = 0.3f;
    run(e, sine, 1, &rms, &peak);   /* (it fades) */
    run(e, sine, 0.5f, &rms, &peak);
    CHECK(peak < 1e-4f, "Volume 0 isn't silent (%.5f)", peak);
    {
        struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char d[4]; char a, b, c, x; } ev = {1, 32, 0, 0, 0, 0, {0xb3, 11, 127, 0}, 0, 0, 0, 0};
        struct { int32_t n; intptr_t r; void *ev[2]; } evs = {1, 0, {&ev, NULL}};
        e->dispatcher(e, 25, 0, 0, &evs, 0);
        disp(e, vol, t);
        printf("  CC 11 = 127: Volume %s\n", t);
        CHECK(e->getParameter(e, vol) > 0.99f, "CC 11 doesn't move Volume");
        ev.d[2] = 64;
        e->dispatcher(e, 25, 0, 0, &evs, 0);
        disp(e, vol, t);
        printf("  CC 11 = 64: Volume %s\n", t);
    }
    /* the state chunk */
    e->setParameter(e, param(e, "Peak 8 Gain"), 0.8f);
    void *chunk = NULL;
    const intptr_t cn = e->dispatcher(e, effGetChunk, 0, 0, &chunk, 0);
    AEffect *e2 = make();
    e2->dispatcher(e2, effSetChunk, 0, cn, chunk, 0);
    for (int w = 0; w < 50; w++) {   /* (the amp loads its model and IR in the background) */
        char m2[64];
        run(e2, NULL, 0.05f, &rms, &peak);
        disp(e2, param(e2, "Model"), m2);
        if (!strstr(m2, "Loading") && !strstr(m2, "Scanning")) break;
        usleep(100000);
    }
    int same = 1;
    for (int i = 0; i < e->numParams; i++) {
        char a[64], b[64];
        if (i == gr) continue;
        disp(e, i, a); disp(e2, i, b);
        if (strcmp(a, b) && !strstr(a, "Scanning") && !strstr(b, "Scanning")) { printf("  restored %d: \"%s\" vs \"%s\"\n", i, a, b); same = 0; }
    }
    printf("  state chunk: %ld bytes, %s\n", (long)cn, same ? "everything restored" : "differences");
    CHECK(cn > 100 && same, "the state chunk doesn't restore everything");
    e2->dispatcher(e2, effClose, 0, 0, NULL, 0);
    /* CPU: Pop Rock with the amp on (the heaviest) */
    do { e->setParameter(e, next, 1); e->setParameter(e, next, 0); disp(e, prog, t); } while (strcmp(t, "Ergo Pop Rock Amp"));
    e->setParameter(e, vol, 1);
    for (int w = 0; w < 30; w++) { run(e, bassline, 0.1f, &rms, &peak); usleep(50000); }
    {
        float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
        const int blocks = 2000;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int b = 0; b < blocks; b++) { for (int k = 0; k < N; k++) il[k] = ir[k] = bassline((long)b * N + k); e->processReplacing(e, in, out, N); }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / blocks;
        printf("  CPU, Pop Rock with the amp: %.2f%% of real time (this machine)\n", 100 * ms / (1000.0 * N / FS));
    }
    e->dispatcher(e, effClose, 0, 0, NULL, 0);
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
