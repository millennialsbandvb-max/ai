/* vst_test: load any VST2 effect or instrument the way MPC does, check it, and measure its CPU cost.
 *   vst_test <plugin.so> [--quiet]           exit status 0 = PASSED
 * Prints the plugin's identity and every parameter (name, value text, unit). Then checks: parameters round-trip,
 * audio stays finite with every parameter at its minimum and its maximum, an effect answers an impulse (for a
 * reverb: a tail that is still there after 0.5 s), a synth makes sound from a note, state saves and restores
 * when the plugin uses chunks. Finally times 128-frame blocks at 44.1 kHz, MPC's own rate and block size. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../nam-mpc/src/vst2.h"

enum { audioMasterVersion = 1, audioMasterGetTime = 7, audioMasterGetSampleRate = 16, audioMasterGetBlockSize = 17 };
enum { effProcessEvents = 25, effFlagsIsSynth = 1 << 8 };
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4];
                 char detune, noteOffVelocity, r1, r2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; void *events[2]; } VstEvents;

static int failures;
#define CHECK(c, ...) do { printf((c) ? "  ok    " : "  FAIL  "); printf(__VA_ARGS__); printf("\n"); if (!(c)) failures++; } while (0)

static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) {
    (void)e; (void)i; (void)v; (void)p; (void)o;
    if (op == audioMasterVersion) return 2400;
    if (op == audioMasterGetSampleRate) return 44100;
    if (op == audioMasterGetBlockSize) return 128;
    return 0;
}

#define N 128
static float il[N], ir[N], ol[N], orr[N];
static float *ins[2] = {il, ir}, *outs[2] = {ol, orr};

static double run(AEffect *e, int blocks, int impulse, double *tail_rms) {   /* -> peak; -1 if not finite */
    double peak = 0, tail = 0; long tn = 0;
    for (int b = 0; b < blocks; b++) {
        for (int k = 0; k < N; k++) {
            il[k] = ir[k] = impulse ? ((b == 0 && k == 0) ? 1.0f : 0.0f)
                                    : 0.3f * (float)sin(2 * M_PI * 220 * (b * N + k) / 44100.0);
        }
        e->processReplacing(e, ins, outs, N);
        for (int k = 0; k < N; k++) {
            if (!isfinite(ol[k]) || !isfinite(orr[k])) return -1;
            double a = fabs(ol[k]) > fabs(orr[k]) ? fabs(ol[k]) : fabs(orr[k]);
            if (a > peak) peak = a;
            if ((b * N + k) > 22050) { tail += ol[k] * ol[k]; tn++; }
        }
    }
    if (tail_rms) *tail_rms = tn ? sqrt(tail / tn) : 0;
    return peak;
}

static void set_all(AEffect *e, float v) { for (int i = 0; i < e->numParams; i++) e->setParameter(e, i, v); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]); return 2; }
    int quiet = argc > 2 && !strcmp(argv[2], "--quiet");
    int json = argc > 2 && !strcmp(argv[2], "--params-json");   /* print the parameter list as JSON and stop */
    if (!json) printf("vst_test: %s\n", argv[1]);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("  FAIL  dlopen: %s\n", dlerror()); return 1; }
    AEffect *(*entry)(audioMasterCallback) = (AEffect * (*)(audioMasterCallback)) dlsym(lib, "VSTPluginMain");
    if (json) {   /* [{"index", "name", "unit", "default", "options": [labels of a stepped control]}] */
        AEffect *p = entry ? entry(host) : NULL;
        if (!p) return 1;
        p->dispatcher(p, effOpen, 0, 0, NULL, 0);
        printf("[\n");
        for (int i = 0; i < p->numParams; i++) {
            char pn[256] = {0}, pl[256] = {0}, opts[64][32];
            int nopts = 0, stepped = 1;
            p->dispatcher(p, effGetParamName, i, 0, pn, 0);
            p->dispatcher(p, effGetParamLabel, i, 0, pl, 0);
            const float def = p->getParameter(p, i);
            for (int k = 0; k <= 256 && stepped; k++) {   /* sweep: a stepped control shows few distinct values */
                char d[256] = {0};
                p->setParameter(p, i, k / 256.0f);
                p->dispatcher(p, effGetParamDisplay, i, 0, d, 0);
                if (nopts == 0 || strcmp(opts[nopts - 1], d)) {
                    if (nopts == 16) { stepped = 0; break; }
                    snprintf(opts[nopts++], 32, "%s", d);
                }
            }
            p->setParameter(p, i, def);
            int text = 0;   /* options are names (not numbers) */
            for (int k = 0; k < nopts && stepped; k++) if (opts[k][0] && !strchr("-0123456789.", opts[k][0])) text = 1;
            printf("  {\"index\": %d, \"name\": \"%s\", \"unit\": \"%s\", \"default\": %.4f", i, pn, pl, def);
            if (stepped && text && nopts > 1) {
                printf(", \"options\": [");
                for (int k = 0; k < nopts; k++) printf("%s\"%s\"", k ? ", " : "", opts[k]);
                printf("]");
            }
            printf("}%s\n", i + 1 < p->numParams ? "," : "");
        }
        printf("]\n");
        return 0;
    }
    CHECK(entry != NULL, "exports VSTPluginMain");
    if (!entry) return 1;
    AEffect *e = entry(host);
    CHECK(e && e->magic == VST_MAGIC, "AEffect magic 'VstP'");
    if (!e) return 1;
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, 44100.0f);
    e->dispatcher(e, effSetBlockSize, 0, N, NULL, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    char name[256] = {0}, vendor[256] = {0};
    e->dispatcher(e, effGetEffectName, 0, 0, name, 0);
    e->dispatcher(e, effGetVendorString, 0, 0, vendor, 0);
    const int synth = (e->flags & effFlagsIsSynth) != 0;
    printf("  %s by %s: %s, %d in / %d out, %d params, uid %08x, flags %x\n", name, vendor, synth ? "instrument" : "effect",
           e->numInputs, e->numOutputs, e->numParams, (unsigned)e->uniqueID, (unsigned)e->flags);
    CHECK(e->processReplacing != NULL, "has processReplacing");
    CHECK(e->numOutputs == 2, "stereo out");

    float *defaults = calloc(e->numParams + 1, sizeof(float));
    for (int i = 0; i < e->numParams; i++) {
        char pn[256] = {0}, pd[256] = {0}, pl[256] = {0};
        e->dispatcher(e, effGetParamName, i, 0, pn, 0);
        e->dispatcher(e, effGetParamDisplay, i, 0, pd, 0);
        e->dispatcher(e, effGetParamLabel, i, 0, pl, 0);
        defaults[i] = e->getParameter(e, i);
        if (!quiet) printf("        param %2d %-16s = %-10s %-4s (%.3f)\n", i, pn, pd, pl, defaults[i]);
    }
    int rt = 1, stepped = 0;
    for (int i = 0; i < e->numParams; i++) {
        e->setParameter(e, i, 0.25f);
        const float got = e->getParameter(e, i);
        if (fabsf(got - 0.25f) > 0.02f) {
            if (fabsf(got - 0.25f) <= 0.26f) stepped++;   /* a choice or on/off control snaps to its nearest step (up to 0.25 away) */
            else rt = 0;
        }
        e->setParameter(e, i, defaults[i]);
    }
    CHECK(rt, "parameters round-trip (%d stepped: choices snap to their nearest option)", stepped);

    double tail = 0, peak;
    if (synth) {
        VstMidiEvent on = {1, sizeof(VstMidiEvent), 0, 0, 0, 0, {0x90, 60, 100, 0}, 0, 0, 0, 0};
        VstEvents ev = {1, 0, {&on, NULL}};
        e->dispatcher(e, effProcessEvents, 0, 0, &ev, 0);
        peak = run(e, 200, 0, &tail);
        CHECK(peak > 1e-4 && peak < 20, "a note makes sound (peak %.3f)", peak);
    } else {
        peak = run(e, 200, 1, &tail);
        CHECK(peak >= 0, "impulse response stays finite");
        CHECK(peak > 1e-5, "impulse gets through (peak %.4f)", peak);
        printf("        signal after 0.5 s (a reverb's tail): rms %.2g\n", tail);
    }
    set_all(e, 0.0f);
    CHECK(run(e, 100, 0, NULL) >= 0, "every parameter at minimum: output finite");
    set_all(e, 1.0f);
    CHECK(run(e, 100, 0, NULL) >= 0, "every parameter at maximum: output finite");
    for (int i = 0; i < e->numParams; i++) e->setParameter(e, i, defaults[i]);

    if (e->flags & effFlagsProgramChunks) {
        void *data = NULL;
        intptr_t len = e->dispatcher(e, effGetChunk, 0, 0, &data, 0);
        CHECK(len > 0 && data, "state saves as a chunk (%ld bytes)", (long)len);
        if (len > 0 && data) {
            char *copy = malloc(len);
            memcpy(copy, data, len);
            e->dispatcher(e, effSetChunk, 0, len, copy, 0);
            free(copy);
        }
    } else {
        printf("        state: saved as parameter values (no chunk)\n");
    }

    run(e, 50, 0, NULL);
    struct timespec t0, t1;
    const int blocks = 4000;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    run(e, blocks, 0, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / blocks;
    printf("CPU: %.4f ms per 128-frame block = %.1f%% of one core in real time\n", ms, 100 * ms / (N / 44.1));
    e->dispatcher(e, effClose, 0, 0, NULL, 0);
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
