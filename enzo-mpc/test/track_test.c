/* track_test: how well Enz0 X's Mono mode follows a guitar.   track_test <Enz0-X.so> [max_ms max_bad%]   exit 0 = PASSED
 * Plucked strings (Karplus-Strong: harmonic-rich, a strong attack, a decaying tone) from low E2 (82 Hz) to E5 (659 Hz),
 * one note every 0.6 s, played straight into the next (as when playing a line). For each note: how long until the
 * synth plays it (within 35 cents, held for 20 ms), and how much of the rest of the note it spends more than 60 cents
 * off (wrong notes, octave jumps). The synth's pitch is measured on its output (Mono, square, filter open).
 * HARD=1: the string before keeps ringing under the new one (-12 dB) and the fundamental is weak (a pickup's high-pass
 * at 180 Hz), as in real playing. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../nam-mpc/src/vst2.h"

#define FS 48000
#define N 128
#define NOTE_LEN (FS * 6 / 10)
static AEffect *e;
static int param(const char *name) {
    for (int i = 0; i < e->numParams; i++) { char t[64] = {0}; e->dispatcher(e, effGetParamName, i, 0, t, 0); if (!strcmp(t, name)) return i; }
    fprintf(stderr, "no parameter %s\n", name); exit(2);
}
static void set_text(const char *name, const char *want) {
    const int i = param(name);
    for (int k = 0; k <= 400; k++) {
        e->setParameter(e, i, (float)k / 400);
        char t[64] = {0};
        e->dispatcher(e, effGetParamDisplay, i, 0, t, 0);
        if (!strcmp(t, want)) return;
    }
    fprintf(stderr, "%s can't show %s\n", name, want); exit(2);
}
/* the notes: an E2-E5 line, with some leaps (MIDI) */
static const int NOTES[] = {40, 45, 50, 55, 59, 64, 69, 76, 52, 47, 43, 57, 62, 67, 72, 48, 41, 60, 65, 71, 74, 46, 53, 58};
#define NN ((int)(sizeof NOTES / sizeof *NOTES))
static float in_buf[NN * NOTE_LEN], out_buf[NN * NOTE_LEN];

static void make_input(void) {   /* Karplus-Strong plucks, each a new string; a little pick noise */
    unsigned r = 12345;
    for (int n = 0; n < NN; n++) {
        const float hz = 440 * powf(2, (float)(NOTES[n] - 69) / 12);
        const int L = (int)(FS / hz);
        const float frac = FS / hz - (float)L;
        float *dl = calloc((size_t)L + 2, sizeof *dl);
        for (int i = 0; i < L + 2; i++) { r = r * 1664525u + 1013904223u; dl[i] = ((float)(r >> 8) / 16777216.0f - 0.5f) * 0.8f; }
        int w = 0;
        float lpz = 0;
        for (int i = 0; i < NOTE_LEN; i++) {
            /* read L + frac samples back (linear), average with the next (the string's damping), write back */
            const int a = w, b = (w + 1) % (L + 1);
            const float y = dl[a] * (1 - frac) + dl[b] * frac;
            lpz = 0.5f * (y + lpz);
            dl[w] = lpz * 0.996f;
            w = (w + 1) % (L + 1);
            in_buf[n * NOTE_LEN + i] = y * 0.5f;
        }
        free(dl);
    }
    if (getenv("HARD")) {
        static float tail[NN * NOTE_LEN];
        for (int n = 1; n < NN; n++)   /* the previous string still ringing, a quarter as loud */
            for (int i = 0; i < NOTE_LEN; i++) tail[n * NOTE_LEN + i] = in_buf[(n - 1) * NOTE_LEN + NOTE_LEN - 1 - (i % 4800)] * 0.25f * expf(-(float)i / (0.4f * FS));
        float z = 0;
        const float a = 1 - expf(-2 * (float)M_PI * 180 / FS);
        for (long i = 0; i < (long)NN * NOTE_LEN; i++) {   /* a weak fundamental: high-pass at 180 Hz, half blended */
            const float x = in_buf[i] + tail[i];
            z += a * (x - z);
            in_buf[i] = x - 0.7f * z;
        }
    }
}
static float pitch_at(const float *x, long at, int len, float lo_hz, float hi_hz) {   /* autocorrelation, shortest lag near the best */
    const int lmin = (int)(FS / hi_hz), lmax = (int)(FS / lo_hz);
    static double c[2000];
    double best = -1;
    int bl = 0;
    for (int lag = lmin; lag <= lmax; lag++) {
        double s = 0, e0 = 0, e1 = 0;
        for (int i = 0; i < len; i++) { s += x[at + i] * x[at + i + lag]; e0 += x[at + i] * x[at + i]; e1 += x[at + i + lag] * x[at + i + lag]; }
        c[lag] = s / sqrt(e0 * e1 + 1e-20);
        if (c[lag] > best) { best = c[lag]; bl = lag; }
    }
    if (best < 0.5) return 0;
    for (int lag = lmin + 1; lag < bl; lag++)
        if (c[lag] > best * 0.93 && c[lag] >= c[lag - 1] && c[lag] >= c[lag + 1]) { bl = lag; break; }
    const double a = c[bl - 1], b = c[bl], cc = c[bl + 1], den = a - 2 * b + cc;
    const double t = bl + (fabs(den) > 1e-12 ? 0.5 * (a - cc) / den : 0);
    return (float)(FS / t);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: track_test <Enz0-X.so> [max_ms max_bad%%]\n"); return 2; }
    const float max_ms = argc > 2 ? (float)atof(argv[2]) : 1e9f, max_bad = argc > 3 ? (float)atof(argv[3]) : 1e9f;
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    AEffect *(*entry)(audioMasterCallback) = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    e = entry(NULL);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, FS);
    e->dispatcher(e, effSetBlockSize, 0, N, NULL, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    set_text("Mode", "Mono"); set_text("Filter Type", "Low Pass"); set_text("Pitch", "+0 st");
    e->setParameter(e, param("Cutoff"), 1); e->setParameter(e, param("Env Amount"), 0.5f);
    e->setParameter(e, param("Wave"), 0.5f); e->setParameter(e, param("Sub"), 0); e->setParameter(e, param("Glide"), 0);
    e->setParameter(e, param("Mod Depth"), 0); e->setParameter(e, param("Delay Level"), 0); e->setParameter(e, param("Reverb"), 0);
    e->setParameter(e, param("Mix"), 1); e->setParameter(e, param("Sustain"), 0.3f);
    make_input();
    const long total = (long)NN * NOTE_LEN;
    float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
    for (long b = 0; b + N <= total; b += N) {
        for (int k = 0; k < N; k++) il[k] = ir[k] = in_buf[b + k];
        e->processReplacing(e, in, out, N);
        memcpy(out_buf + b, ol, sizeof ol);
    }
    double sum_ms = 0, worst_ms = 0, bad_all = 0;
    int missed = 0;
    for (int n = 0; n < NN; n++) {
        const float want = 440 * powf(2, (float)(NOTES[n] - 69) / 12);
        const long start = (long)n * NOTE_LEN;
        float land = -1;
        int ok_run = 0, bad = 0, cnt = 0;
        for (long t = start; t + 2400 < start + NOTE_LEN - 2400; t += FS / 400) {   /* every 2.5 ms */
            const float p = pitch_at(out_buf, t, 900, 60, 900);
            const float cents = p > 0 ? 1200 * log2f(p / want) : 9999;
            if (land < 0) {
                if (fabsf(cents) < 35) { if (++ok_run >= 8) land = (float)(t - start - 7 * FS / 400) * 1000 / FS; }
                else ok_run = 0;
            } else { cnt++; if (fabsf(cents) > 60) bad++; }
        }
        const float bad_pct = cnt ? 100.0f * (float)bad / (float)cnt : 100;
        if (land < 0) { missed++; printf("  note %2d (%6.1f Hz): never lands\n", NOTES[n], want); continue; }
        printf("  note %2d (%6.1f Hz): lands in %5.1f ms, then off %4.1f%% of the time\n", NOTES[n], want, land, bad_pct);
        sum_ms += land; if (land > worst_ms) worst_ms = land;
        bad_all += bad_pct;
    }
    const int got = NN - missed;
    const double avg = got ? sum_ms / got : 0, bad_avg = got ? bad_all / got : 100;
    printf("tracking: %d/%d notes, %.1f ms on average (worst %.1f), off %.1f%% of the time\n", got, NN, avg, worst_ms, bad_avg);
    const int pass = !missed && avg <= max_ms && bad_avg <= max_bad;
    printf(pass ? "PASSED\n" : "FAILED\n");
    return pass ? 0 : 1;
}
