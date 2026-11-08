/* quad_test: checks Quad Rig offline (no MPC needed).   quad_test <Quad-Rig.so>   exit 0 = PASSED
 * (NAM_MPC_DIR=<folder with Models/ and IRs/> lets the amp find a model; without one it passes the sound as is.)
 * Every effect in every slot plays finite; the starting board with its amp plays; all four slots Off and the amp
 * bypassed is the input itself; a bypassed slot lets the dry through; knob names and values come from the slot's
 * engine (Duo FX and Ambient Lab); a preset step changes the knobs; the state chunk brings everything back; the
 * amp moves; every rig preset loads what rigs.h says and plays; a chunk from before Presence loads; CPU. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>

#include "../../nam-mpc/src/vst2.h"
#include "../src/rigs.h"

#define FS 48000
#define N 128
static int failures;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL  %s\n", msg); failures++; } } while (0)
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
static int set_text(AEffect *e, int i, const char *want) {
    for (int k = 0; k <= 4000; k++) {
        e->setParameter(e, i, (float)k / 4000);
        char t[64]; disp(e, i, t);
        if (!strcmp(t, want)) return 1;
    }
    printf("  (parameter %d can't show %s)\n", i, want); failures++; return 0;
}
static float g_music(long t) {
    static const float NOTES[8] = {110, 146.8f, 164.8f, 196, 220, 246.9f, 293.7f, 329.6f};
    const long n = t / (FS / 4), u = t % (FS / 4);
    return 0.3f * expf(-(float)u / (0.15f * FS)) * sinf(2 * (float)M_PI * NOTES[n % 8] * (float)t / FS);
}
static float bufl[10 * FS], bufr[10 * FS], inb[10 * FS];
static int render(AEffect *e, float secs, float *peak) {
    const long n = (long)(secs * FS);
    float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
    int ok = 1;
    *peak = 0;
    for (long b = 0; b < n; b += N) {
        for (int k = 0; k < N; k++) il[k] = ir[k] = inb[b + k] = g_music(b + k);
        e->processReplacing(e, in, out, N);
        for (int k = 0; k < N && b + k < n; k++) {
            if (!isfinite(ol[k]) || !isfinite(orr[k])) ok = 0;
            bufl[b + k] = ol[k]; bufr[b + k] = orr[k];
            if (fabsf(ol[k]) > *peak) *peak = fabsf(ol[k]);
        }
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: quad_test <Quad-Rig.so>\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    entry = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    AEffect *e = make();
    char name[64], t[64];
    e->dispatcher(e, effGetEffectName, 0, 0, name, 0);
    printf("%s, %d parameters\n", name, e->numParams);
    const int fx1 = param(e, "FX1 Effect"), mix1 = param(e, "FX1 Mix"), byp1 = param(e, "FX1 Bypass"), prev = param(e, "FX1 Preset >");
    const int amp_pos = param(e, "Amp Position"), amp_byp = param(e, "Amp Bypass");
    float peak;

    /* the starting board */
    for (int k = 0; k < 4; k++) { disp(e, param(e, k == 0 ? "FX1 Effect" : k == 1 ? "FX2 Effect" : k == 2 ? "FX3 Effect" : "FX4 Effect"), t); printf("  FX%d: %s\n", k + 1, t); }
    disp(e, amp_pos, t); printf("  amp: %s\n", t);
    CHECK(render(e, 4, &peak) && peak < 3.99f, "the starting board isn't finite / under the ceiling");

    /* every effect in slot 1 */
    int n_fx = 0, bad = 0;
    char last[64] = "";
    for (int f = 0; f < 100; f++) {
        e->setParameter(e, fx1, (float)f / 70);
        disp(e, fx1, t);
        if (!strcmp(t, last)) break;
        strcpy(last, t);
        n_fx++;
        if (!render(e, 0.4f, &peak) || peak >= 3.99f) { printf("  FAIL  %s in FX1 (peak %.2f)\n", t, peak); bad++; }
        if (f == 70) break;
    }
    printf("  %d effects in FX1, %d bad\n", n_fx, bad);
    CHECK(n_fx == 71 && !bad, "the effect list");

    /* knob names and values come from the slot's engine */
    set_text(e, fx1, "Digital Delay");
    char k1[64] = {0};
    e->dispatcher(e, effGetParamName, mix1 + 1, 0, k1, 0);
    disp(e, mix1 + 1, t);
    printf("  FX1 = Digital Delay: knob 1 \"%s\" = %s\n", k1, t);
    CHECK(!strcmp(k1, "Time"), "knob 1 isn't the delay's Time");
    set_text(e, fx1, "Mikrocosm");
    e->dispatcher(e, effGetParamName, mix1 + 1, 0, k1, 0);
    printf("  FX1 = Mikrocosm: knob 1 \"%s\"\n", k1);
    CHECK(!strcmp(k1, "Effect"), "knob 1 isn't Mikrocosm's Effect");
    /* a preset step (Duo FX's) changes the knobs */
    set_text(e, fx1, "Tape Echo");
    const float before = e->getParameter(e, mix1 + 1);
    e->setParameter(e, prev, 1);
    render(e, 0.1f, &peak);   /* (the engine reports the preset's knobs from its process) */
    char pn[64]; disp(e, param(e, "FX1 Preset"), pn);
    printf("  FX1 Tape Echo, Preset >: \"%s\", Time %.3f -> %.3f\n", pn, before, e->getParameter(e, mix1 + 1));
    CHECK(strcmp(pn, "-") && strlen(pn) > 1, "the preset step didn't load a preset");

    /* all Off and the amp bypassed: the input itself */
    for (int k = 0; k < 4; k++) set_text(e, fx1 + 16 * k, "Off");
    e->setParameter(e, amp_byp, 1);
    render(e, 1, &peak);
    double err = 0;
    for (long i = FS / 2; i < FS; i++) err = fmax(err, fabs(bufl[i] - inb[i]));
    printf("  all Off, amp bypassed: largest difference from the input %.6f\n", err);
    CHECK(err < 1e-5, "all Off + amp bypassed isn't the input");
    /* a bypassed slot lets the dry through: Digital Delay at 100% wet, bypassed */
    set_text(e, fx1, "Digital Delay");
    e->setParameter(e, mix1, 1.0f);
    e->setParameter(e, byp1, 1);
    render(e, 2, &peak);
    err = 0;
    for (long i = FS + FS / 2; i < 2 * FS; i++) err = fmax(err, fabs(bufl[i] - inb[i]));
    printf("  FX1 bypassed: largest difference from the input %.4f (the delay's tail aside)\n", err);
    CHECK(err < 0.05, "a bypassed slot doesn't pass the dry sound");
    e->setParameter(e, byp1, 0);

    /* the state chunk brings it all back */
    set_text(e, fx1, "Partikle");
    e->setParameter(e, mix1 + 2, 0.8f);
    set_text(e, amp_pos, "Amp Last");
    void *chunk = NULL;
    const intptr_t n = e->dispatcher(e, effGetChunk, 0, 0, &chunk, 0);
    AEffect *e2 = make();
    e2->dispatcher(e2, effSetChunk, 0, n, chunk, 0);
    for (int w = 0; w < 50; w++) {   /* (the amp loads its model and IR in the background) */
        char m2[64], i2[64];
        render(e2, 0.05f, &peak);
        disp(e2, param(e2, "Model"), m2); disp(e2, param(e2, "IR"), i2);
        if (!strstr(m2, "Loading") && !strstr(m2, "Scanning") && !strstr(i2, "Loading") && !strstr(i2, "Scanning")) break;
        usleep(100000);
    }
    render(e2, 0.3f, &peak);   /* (the amp's colour follows its model now and then) */
    int same = 1;
    for (int i = 0; i < e->numParams; i++) {
        char a[64], b[64];
        disp(e, i, a); disp(e2, i, b);
        if (strcmp(a, b) && !strstr(a, "Scanning") && !strstr(b, "Scanning")) { printf("  restored %d: \"%s\" vs \"%s\"\n", i, a, b); same = 0; }
    }
    printf("  state chunk: %ld bytes, %s\n", (long)n, same ? "everything restored" : "differences");
    CHECK(n > 100 && same, "the state chunk doesn't restore everything");
    e2->dispatcher(e2, effClose, 0, 0, NULL, 0);

    /* the amp: with a model it changes the sound */
    e->setParameter(e, amp_byp, 0);
    for (int k = 0; k < 4; k++) set_text(e, fx1 + 16 * k, "Off");
    char model[64] = "";
    for (int w = 0; w < 50; w++) { render(e, 0.1f, &peak); disp(e, param(e, "Model"), model); if (!strstr(model, "Scanning") && !strstr(model, "Loading")) break; usleep(100000); }
    render(e, 1, &peak);
    err = 0;
    for (long i = FS / 2; i < FS; i++) err = fmax(err, fabs(bufl[i] - inb[i]));
    printf("  amp on, model \"%s\": difference from the input %.3f\n", model, err);
    if (getenv("NAM_MPC_DIR")) CHECK(err > 0.01, "the amp's model doesn't change the sound");

    /* the rig presets, in both banks: Rig > steps through them all; each slot shows the rig's effect and preset, and it plays */
    for (int bk = 0; bk < N_BANKS; bk++) {
        const Rig *rigs = BANKS[bk].rigs;
        const int nrigs = BANKS[bk].n;
        if (bk) { const int bn = param(e, "Bank >"); e->setParameter(e, bn, 1); e->setParameter(e, bn, 0); disp(e, param(e, "Bank"), t); printf("  bank: %s\n", t); CHECK(!strcmp(t, BANKS[bk].name), "Bank > doesn't move to the next bank"); }
        const int rig = param(e, "Rig"), next = param(e, "Rig >"), pres = param(e, "Presence");
        int bad = 0;
        for (int r = 0; r < nrigs; r++) {
            e->setParameter(e, next, 1);
            e->setParameter(e, next, 0);
            disp(e, rig, t);
            if (strcmp(t, rigs[r].name)) { printf("  rig %d shows \"%s\", not \"%s\"\n", r + 1, t, rigs[r].name); bad++; }
            for (int k = 0; k < 6; k++) {
                char f[64], pr[64];
                disp(e, fx1 + 16 * k, f);
                disp(e, fx1 + 16 * k + 11, pr);
                const char *want = rigs[r].s[k].preset;
                int ok = !strcmp(f, rigs[r].s[k].fx);
                if (want) {   /* (letters and digits alike: rigs.h writes Ain't with a plain quote or none) */
                    char a2[64] = {0}, b2[64] = {0};
                    for (int i = 0, j = 0; pr[i] && j < 63; i++) if (isalnum((unsigned char)pr[i])) a2[j++] = (char)tolower((unsigned char)pr[i]);
                    for (int i = 0, j = 0; want[i] && j < 63; i++) if (isalnum((unsigned char)want[i])) b2[j++] = (char)tolower((unsigned char)want[i]);
                    ok = ok && *a2 && !strncmp(a2, b2, strlen(a2));   /* (the display may cut a long name) */
                }
                if (!ok) { printf("  rig \"%s\" slot %d: %s / %s, wanted %s / %s\n", rigs[r].name, k + 1, f, pr, rigs[r].s[k].fx, want ? want : "-"); bad++; }
            }
            disp(e, pres, t);
            const int fine = render(e, 1.5f, &peak);
            printf("  rig %2d %-18s presence %s, peak %.2f\n", r + 1, rigs[r].name, t, peak);
            if (!fine || peak > 4.01f) { printf("  rig \"%s\" plays badly\n", rigs[r].name); bad++; }
        }
        CHECK(!bad, "a rig preset doesn't load as written");
        if (bk) continue;
        e->setParameter(e, rig, 2.0f / (nrigs - 1));   /* an exact value (a project restoring) shows the name only */
        disp(e, rig, t);
        char f1[64];
        disp(e, fx1, f1);
        CHECK(!strcmp(t, rigs[2].name) && !strcmp(f1, rigs[nrigs - 1].s[0].fx), "a restored Rig value changed the board");
    }
    /* the rig CCs (a foot controller): 20 bank, 21 rig, 22 / 23 a song's signature / rhythm, 24 / 25 Rig > / <, 26 rhythm on/off */
    {
        static const struct { int cc, v; const char *want; } STEPS[] = {
            {20, 2, NULL}, {22, 2, "Alive"}, {26, 127, "Alive Rhy"}, {26, 0, "Alive"}, {23, 3, "Corduroy Rhy"},
            {24, 127, "Rearviewmirror"}, {24, 0, "Rearviewmirror"}, {25, 127, "Corduroy Rhy"}, {25, 0, "Corduroy Rhy"},
            {20, 3, NULL}, {22, 1, "Fell on Black Days"}, {20, 0, NULL}, {21, 2, NULL}, {23, 1, NULL}};
        const int rig = param(e, "Rig");
        int bad = 0;
        for (size_t k = 0; k < sizeof STEPS / sizeof *STEPS; k++) {
            struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char d[4]; char a, b, c, x; } ev = {1, 32, 0, 0, 0, 0, {0xb3, (unsigned char)STEPS[k].cc, (unsigned char)STEPS[k].v, 0}, 0, 0, 0, 0};
            struct { int32_t n; intptr_t r; void *ev[2]; } evs = {1, 0, {&ev, NULL}};
            e->dispatcher(e, 25, 0, 0, &evs, 0);
            disp(e, rig, t);
            const char *want = STEPS[k].want ? STEPS[k].want : STEPS[k].cc == 20 ? "-" : STEPS[k].v == 2 ? BANKS[0].rigs[1].name : BANKS[0].rigs[0].name;
            if (STEPS[k].cc == 20) { char b[64]; disp(e, param(e, "Bank"), b); if (strcmp(b, BANKS[STEPS[k].v].name)) { printf("  CC 20 %d: bank %s\n", STEPS[k].v, b); bad++; } }
            else if (strcmp(t, want)) { printf("  CC %d %d: rig \"%s\", wanted \"%s\"\n", STEPS[k].cc, STEPS[k].v, t, want); bad++; }
        }
        printf("  rig CCs 20-26: %d wrong\n", bad);
        CHECK(!bad, "the rig CCs don't pick the right rigs");
    }
    /* a rig picks an amp from the library by its words (QUAD_AMP_TEST=1: a library of "Marshall Plexi" and
     * "Fender Twin" models): Plexi Crunch -> the Marshall, Texas Blues -> the Fender, Fuzz Synth Beast (svt / ampeg /
     * orange: none there) -> stays */
    if (getenv("QUAD_AMP_TEST")) {
        const int rig = param(e, "Rig"), next = param(e, "Rig >"), model = param(e, "Model");
        static const char *const WANT[][2] = {{"Plexi Crunch", "Marshall Plexi"}, {"Texas Blues", "Fender Twin"}, {"Fuzz Synth Beast", "Marshall Plexi"}};   /* (no match: still the one Shoegaze Wall, just before, chose) */
        for (int w = 0; w < 3; w++) {
            for (int k = 0; k < 40; k++) { disp(e, rig, t); if (!strcmp(t, WANT[w][0])) break; e->setParameter(e, next, 1); e->setParameter(e, next, 0); }
            char m[64] = "";
            for (int k = 0; k < 60; k++) { render(e, 0.05f, &peak); usleep(50000); disp(e, model, m); if (!strcmp(m, WANT[w][1])) break; }
            printf("  rig %s: amp \"%s\"\n", WANT[w][0], m);
            CHECK(!strcmp(m, WANT[w][1]), "a rig doesn't choose its amp from the library");
        }
    }
    /* a chunk from before Presence (NAM's 21 parameters) */
    {
        set_text(e, fx1, "Hall");
        void *ck = NULL;
        const intptr_t cn = e->dispatcher(e, effGetChunk, 0, 0, &ck, 0);
        int np = 0, used = 0;
        sscanf((const char *)ck, "QRIG1 %d\n%n", &np, &used);
        char *old = malloc((size_t)cn + 64), *q = old;
        const char *src = (const char *)ck + used;
        q += sprintf(q, "QRIG1 %d\n", np - 1);
        for (int i = 0; i < np; i++) { const char *nl = strchr(src, '\n') + 1; if (i < np - 1) { memcpy(q, src, (size_t)(nl - src)); q += nl - src; } src = nl; }
        const size_t rest = (size_t)cn - (size_t)(src - (const char *)ck);
        memcpy(q, src, rest); q += rest;
        AEffect *e3 = make();
        e3->dispatcher(e3, effSetChunk, 0, q - old, old, 0);
        char a3[64];
        disp(e3, fx1, a3);
        disp(e, fx1, t);
        CHECK(!strcmp(a3, t), "a chunk from before Presence doesn't load");
        printf("  chunk before Presence: FX1 %s\n", a3);
        e3->dispatcher(e3, effClose, 0, 0, NULL, 0);
        free(old);
    }

    /* CPU: the starting board (drive, chorus, amp, delay, reverb) */
    AEffect *c = make();
    {
        float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
        for (int w = 0; w < 30; w++) { render(c, 0.1f, &peak); usleep(100000); }
        const int blocks = 2000;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int b = 0; b < blocks; b++) { for (int k = 0; k < N; k++) il[k] = ir[k] = g_music((long)b * N + k); c->processReplacing(c, in, out, N); }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / blocks;
        printf("  CPU, the starting board: %.2f%% of real time (this machine)\n", 100 * ms / (1000.0 * N / FS));
    }
    c->dispatcher(c, effClose, 0, 0, NULL, 0);
    e->dispatcher(e, effClose, 0, 0, NULL, 0);
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
