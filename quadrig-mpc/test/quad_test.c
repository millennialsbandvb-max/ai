/* quad_test: checks Quad Rig offline (no MPC needed).   quad_test <Quad-Rig.so>   exit 0 = PASSED
 * (NAM_MPC_DIR=<folder with Models/ and IRs/> lets the amp find a model; without one it passes the sound as is.)
 * Every effect in every slot plays finite; the starting board with its amp plays; all four slots Off and the amp
 * bypassed is the input itself; a bypassed slot lets the dry through; knob names and values come from the slot's
 * engine (Duo FX and Ambient Lab); a preset step changes the knobs; the state chunk brings everything back; the
 * amp moves; CPU. */
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
        e->setParameter(e, fx1, (float)f / 68);
        disp(e, fx1, t);
        if (!strcmp(t, last)) break;
        strcpy(last, t);
        n_fx++;
        if (!render(e, 0.4f, &peak) || peak >= 3.99f) { printf("  FAIL  %s in FX1 (peak %.2f)\n", t, peak); bad++; }
        if (f == 68) break;
    }
    printf("  %d effects in FX1, %d bad\n", n_fx, bad);
    CHECK(n_fx == 69 && !bad, "the effect list");

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
    CHECK(!strcmp(k1, "Activity"), "knob 1 isn't Mikrocosm's Activity");
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
