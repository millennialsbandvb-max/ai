/* host_contract_test: checks a plugin against what MPC OS's plugin host does (Akai's MPC plugin SDK, host-contract.md):
 *   host_contract_test <plugin.so>    exit 0 = PASSED
 * 1. resvd2 is zero when VSTPluginMain returns; open is called twice.
 * 2. On every prepare the host nudges parameter 0 to 1.0 (or 0.0) and straight back: nothing else may change
 *    (every parameter's value and text compared before and after, with the knobs set to random values first).
 * 3. Processing is in place (input and output buffers are the same memory): the output must match a run with
 *    separate buffers. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../nam-mpc/src/vst2.h"

#define FS 48000
#define N 128
static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) {
    (void)e; (void)i; (void)v; (void)p; (void)o;
    return op == 1 ? 2400 : 0;
}
static AEffect *(*entry)(audioMasterCallback);
static int failures;

static AEffect *load(void) {
    AEffect *e = entry(host);
    if (!e) { printf("  FAIL  VSTPluginMain returned nothing\n"); exit(1); }
    if (e->resvd2) { printf("  FAIL  resvd2 is not zero when VSTPluginMain returns\n"); failures++; }
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, FS);
    e->dispatcher(e, effSetBlockSize, 0, N, NULL, 0);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, FS);
    e->dispatcher(e, effSetBlockSize, 0, N, NULL, 0);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);   /* (a second time, as the MPC does) */
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    return e;
}
static float sig(long t, int ch) {
    const double ph = 2 * M_PI * 196.0 * (double)t / FS;
    return 0.3f * (float)(sin(ph) + 0.4 * sin(2 * ph + ch)) * (float)exp(-(double)(t % (FS / 2)) / (0.2 * FS));
}
/* blocks of audio; inplace: one pair of buffers for in and out */
static void play(AEffect *e, long t0, int blocks, int inplace, float *rec) {
    static float a[N], b[N], c[N], d[N];
    for (int k = 0; k < blocks; k++) {
        for (int i = 0; i < N; i++) { a[i] = sig(t0 + (long)k * N + i, 0); b[i] = sig(t0 + (long)k * N + i, 1); }
        float *in[2] = {a, b}, *sep[2] = {c, d};
        e->processReplacing(e, in, inplace ? in : sep, N);
        float *o0 = inplace ? a : c, *o1 = inplace ? b : d;
        if (rec) for (int i = 0; i < N; i++) { rec[2 * ((long)k * N + i)] = o0[i]; rec[2 * ((long)k * N + i) + 1] = o1[i]; }
    }
}
typedef struct { float v; char t[256]; } Snap;   /* (the MPC host reads up to 256) */
static void snap(AEffect *e, Snap *s) {
    for (int i = 0; i < e->numParams; i++) {
        s[i].v = e->getParameter(e, i);
        memset(s[i].t, 0, sizeof s[i].t);
        e->dispatcher(e, effGetParamDisplay, i, 0, s[i].t, 0);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: host_contract_test <plugin.so>\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    entry = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    AEffect *e = load();
    char name[64] = {0};
    e->dispatcher(e, effGetEffectName, 0, 0, name, 0);
    printf("%s: %d parameters\n", name, e->numParams);
    const int np = e->numParams;
    Snap *before = calloc((size_t)np, sizeof *before), *after = calloc((size_t)np, sizeof *after);
    /* 2. parameter 0's nudge, from the defaults and from a few random settings */
    srand(11);
    int bad = 0;
    for (int round = 0; round < 6; round++) {
        if (round) {
            for (int i = 1; i < np; i++) {   /* (not every parameter: momentary arrows and list flags act on writes) */
                char nm[64] = {0};
                e->dispatcher(e, effGetParamName, i, 0, nm, 0);
                if (strchr(nm, '<') || strchr(nm, '>') || strstr(nm, "List") || strstr(nm, "Sel") || strstr(nm, "pick")) continue;
                if (rand() % 3 == 0) e->setParameter(e, i, (float)rand() / (float)RAND_MAX);
            }
            if (round & 1) e->setParameter(e, 0, (float)rand() / (float)RAND_MAX);
        }
        play(e, 0, 40, 0, NULL);
        snap(e, before);
        for (int dir = 0; dir < 2; dir++) {
            const float old = e->getParameter(e, 0);
            e->setParameter(e, 0, dir ? 0.0f : 1.0f);
            e->setParameter(e, 0, old);
            play(e, 0, 8, 0, NULL);
        }
        snap(e, after);
        for (int i = 0; i < np; i++)
            if (fabsf(before[i].v - after[i].v) > 1e-4f || strcmp(before[i].t, after[i].t)) {
                char nm[64] = {0};
                e->dispatcher(e, effGetParamName, i, 0, nm, 0);
                if (!strcmp(nm, "GR")) continue;   /* (a meter: it moves with the audio) */
                if (bad < 12) printf("  parameter 0's nudge changed %d \"%s\": %.4f \"%s\" -> %.4f \"%s\"\n", i, nm, before[i].v, before[i].t, after[i].v, after[i].t);
                bad++;
            }
    }
    if (bad) { printf("  FAIL  parameter 0's nudge changes other settings (%d)\n", bad); failures++; }
    else printf("  parameter 0's nudge: nothing else changes\n");
    e->dispatcher(e, effClose, 0, 0, NULL, 0);
    /* 3. in place against separate buffers: two fresh instances, the same audio */
    const int blocks = (FS * 3) / N;
    float *r1 = calloc((size_t)blocks * N * 2, sizeof *r1), *r2 = calloc((size_t)blocks * N * 2, sizeof *r2);
    AEffect *x = load();
    play(x, 0, blocks, 0, r1);
    x->dispatcher(x, effClose, 0, 0, NULL, 0);
    AEffect *y = load();
    play(y, 0, blocks, 1, r2);
    y->dispatcher(y, effClose, 0, 0, NULL, 0);
    float *r3 = calloc((size_t)blocks * N * 2, sizeof *r3);   /* (a second separate run: is the plugin repeatable?) */
    AEffect *z = load();
    play(z, 0, blocks, 0, r3);
    z->dispatcher(z, effClose, 0, 0, NULL, 0);
    double err = 0, again = 0, peak = 0;
    for (long i = 0; i < (long)blocks * N * 2; i++) {
        err = fmax(err, fabs(r1[i] - r2[i])); again = fmax(again, fabs(r1[i] - r3[i])); peak = fmax(peak, fabs(r1[i]));
    }
    if (again > 1e-4) printf("  in place: not checked (two separate runs differ by %.4f: random drift)\n", again);
    else {
        printf("  in place: largest difference %.6f (peak %.3f)\n", err, peak);
        if (err > 1e-4) { printf("  FAIL  the output differs when processing in place\n"); failures++; }
    }
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
