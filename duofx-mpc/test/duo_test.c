/* duo_test: checks Duo FX offline (no MPC needed).
 *   duo_test <Duo-FX.so> [algorithm number]      exit status 0 = PASSED
 * For every algorithm: with its defaults, every knob at minimum, at maximum, at 80% and at random settings, in slot A
 * (series) and slot B (parallel), 20 s of sound then 15 s of silence stays finite, under the +12 dB ceiling and
 * (except with every knob at full, which may hold forever) dies away; with its defaults it changes the sound. Then what some should do: echoes
 * land at the Time shown, shifters and harmonizers hit the right notes, the tuner pulls a sharp note in. Also
 * times each algorithm (128-frame blocks at 44.1 kHz on this machine). */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../nam-mpc/src/vst2.h"

enum { P_ALGO, P_OPEN, P_MIX, P_K, P_SLOT = 20, ROUTING = 40, OUTPUT = 41 };
#define FS 44100
#define N 128
static int failures, nalgo;
static char names[64][64];
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) { (void)e; (void)op; (void)i; (void)v; (void)p; (void)o; return 0; }
static AEffect *(*entry)(audioMasterCallback);

static AEffect *make(void) {
    AEffect *e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, FS);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    return e;
}
static void algo(AEffect *e, int slot, int a) { e->setParameter(e, slot * P_SLOT + P_ALGO, (float)a / (float)(nalgo - 1)); }
static void knob(AEffect *e, int slot, int k, float v) { e->setParameter(e, slot * P_SLOT + P_K + k, v); }
static void mix(AEffect *e, int slot, float v) { e->setParameter(e, slot * P_SLOT + P_MIX, v); }
static int find(const char *name) { for (int a = 0; a < nalgo; a++) if (!strcmp(names[a], name)) return a; return -1; }
static int knob_index(AEffect *e, int slot, const char *name) {   /* by the knob's name for the current algorithm */
    for (int k = 0; k < 8; k++) {
        char t[64] = {0};
        e->dispatcher(e, effGetParamDisplay, slot * P_SLOT + 11 + k, 0, t, 0);
        if (!strcmp(t, name)) return k;
    }
    return -1;
}
/* set a knob by name to the value whose display starts with `want` (searching the knob's range) */
static int set_by_text(AEffect *e, int slot, const char *kn, const char *want) {
    const int k = knob_index(e, slot, kn);
    if (k < 0) { printf("  (no knob %s)\n", kn); return 0; }
    for (int i = 0; i <= 2000; i++) {
        knob(e, slot, k, (float)i / 2000);
        char t[64] = {0};
        e->dispatcher(e, effGetParamDisplay, slot * P_SLOT + P_K + k, 0, t, 0);
        if (!strncmp(t, want, strlen(want))) return 1;
    }
    printf("  (knob %s can't show %s)\n", kn, want);
    return 0;
}

static float buf[40 * FS], inbuf[40 * FS];
static int render(AEffect *e, float secs, float (*gen)(long), float *peak) {   /* left channel into buf; 0 if not finite */
    float il[N], ir[N], ol[N], orr[N];
    float *in[2] = {il, ir}, *out[2] = {ol, orr};
    const long total = (long)(secs * FS);
    *peak = 0;
    for (long t = 0; t < total; t += N) {
        for (int k = 0; k < N; k++) { il[k] = ir[k] = gen(t + k); if (t + k < total) inbuf[t + k] = il[k]; }
        e->processReplacing(e, in, out, N);
        for (int k = 0; k < N; k++) {
            if (!isfinite(ol[k]) || !isfinite(orr[k])) return 0;
            const float a = fmaxf(fabsf(ol[k]), fabsf(orr[k]));
            if (a > *peak) *peak = a;
            if (t + k < total) buf[t + k] = ol[k];
        }
    }
    return 1;
}
static unsigned rng = 1;
static float noise(void) { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 8388608.0f - 1; }
static float rand01(void) { return (noise() + 1) * 0.5f; }
/* guitar-ish: plucked notes (decaying harmonics) with a little noise, for 20 s, then silence */
static float g_music(long t) {
    if (t >= 20 * FS) return 0;
    static const float NOTES[8] = {220, 246.9f, 261.6f, 329.6f, 196, 293.7f, 164.8f, 392};
    const long n = t / (FS / 2), u = t % (FS / 2);
    const float f = NOTES[n % 8], env = expf(-(float)u / (0.25f * FS));
    float v = 0;
    for (int h = 1; h <= 5; h++) v += sinf(2 * (float)M_PI * f * (float)h * (float)u / FS) / (float)h;
    return 0.35f * env * v + 0.01f * noise();
}
static float g_music8(long t) { return t < 8 * FS ? g_music(t) : 0; }   /* 8 s of it, then silence */
static float g_zero(long t) { (void)t; return 0; }
static float g_impulse(long t) { return t == 0 ? 1.0f : 0; }
static float hz_in;
static float g_sine(long t) { return 0.5f * sinf(2 * (float)M_PI * hz_in * (float)t / FS); }
static void settle(AEffect *e) { float p; render(e, 0.6f, g_zero, &p); }

static double rms(const float *x, long a, long b) { double s = 0; for (long i = a; i < b; i++) s += x[i] * x[i]; return sqrt(s / (double)(b - a)); }
static double tone_level(const float *x, long a, long b, double f) {   /* averaged over 8192-sample windows */
    double sum = 0; int n = 0;
    for (long i = a; i + 8192 <= b; i += 8192, n++) {
        const double w = 2 * M_PI * f / FS, c = 2 * cos(w);
        double s1 = 0, s2 = 0;
        for (long j = i; j < i + 8192; j++) { const double s0 = x[j] + c * s1 - s2; s2 = s1; s1 = s0; }
        sum += sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / 8192;
    }
    return n ? sum / n : 0;
}
static double peak_hz(const float *x, long a, long b, double lo, double hi) {   /* the strongest frequency in [lo, hi] */
    double best = 0, bf = 0;
    for (double f = lo; f <= hi; f *= 1.002) { const double v = tone_level(x, a, b, f); if (v > best) { best = v; bf = f; } }
    return bf;
}

static void stability(int a) {
    static const char *SET[5] = {"defaults", "min", "max", "high", "random"};
    for (int slot = 0; slot < 2; slot++)
        for (int setting = 0; setting < (slot ? 1 : 5); setting++) {
            AEffect *e = make();
            algo(e, 0, find("Off")); algo(e, 1, find("Off"));
            algo(e, slot, a);
            e->setParameter(e, ROUTING, (float)slot);   /* slot A in series, slot B in parallel */
            for (int k = 0; k < 8 && setting; k++) knob(e, slot, k, setting == 1 ? 0.0f : setting == 2 ? 1.0f : setting == 3 ? 0.8f : rand01());
            if (setting == 2 || setting == 3) mix(e, slot, 1);
            float peak;
            const int ok = render(e, 35, g_music, &peak);
            const char *where = slot ? "B parallel" : "A series";
            CHECK(ok, "%s %s %s: not finite", names[a], where, SET[setting]);
            CHECK(peak < 3.99f, "%s %s %s: hits the +12 dB ceiling (peak %.2f)", names[a], where, SET[setting], peak);
            const double end = rms(buf, 34 * FS, 35 * FS), before = rms(buf, 29 * FS, 30 * FS);
            /* every knob at full may hold forever (full Feedback / Decay), by design: then it only has to stay in bounds */
            const int holds = setting == 2 || (!strcmp(names[a], "Blackhol3") && setting >= 3);
            CHECK(holds || end < 0.003 || end < (setting >= 3 ? 0.85 : 0.5) * before, "%s %s %s: still %.4f rms 14 s after the input stopped (%.4f 5 s before)",
                  names[a], where, SET[setting], end, before);
            if (setting == 0 && slot == 0 && strcmp(names[a], "Off")) {
                double diff = 0;
                for (long i = FS; i < 19 * FS; i++) { const double d = buf[i] - inbuf[i]; diff += d * d; }
                diff = sqrt(diff / (18.0 * FS));
                CHECK(diff > 0.01, "%s with its defaults doesn't change the sound (differs by %.4f rms)", names[a], diff);
            }
            e->dispatcher(e, effClose, 0, 0, NULL, 0);
        }
}

static AEffect *one(const char *name, float mixv) {
    AEffect *e = make();
    algo(e, 1, find("Off")); algo(e, 0, find(name));
    mix(e, 0, mixv);
    return e;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <Duo-FX.so> [algorithm]\n", argv[0]); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h || !(*(void **)&entry = dlsym(h, "VSTPluginMain"))) { fprintf(stderr, "can't load %s\n", argv[1]); return 1; }
    {   /* the algorithm names, from the plugin (56 options: the last is Off) */
        nalgo = 56;
        AEffect *e = make();
        for (int a = 0; a < nalgo; a++) {
            e->setParameter(e, P_ALGO, (float)a / (float)(nalgo - 1));
            e->dispatcher(e, effGetParamDisplay, P_ALGO, 0, names[a], 0);
        }
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
        printf("%d algorithms: %s ... %s\n", nalgo, names[0], names[nalgo - 1]);
        if (strcmp(names[nalgo - 1], "Off")) { printf("FAILED: the algorithm count changed\n"); return 1; }
    }
    const int only = argc > 2 ? atoi(argv[2]) : -1;

    printf("stability\n");
    for (int a = 0; a < nalgo; a++) if (only < 0 || only == a) { stability(a); fflush(stdout); }

    if (only < 0) {
        printf("behaviour\n");
        {   /* Digital Delay: an echo at the Time shown */
            AEffect *e = one("Digital Delay", 1);
            set_by_text(e, 0, "Feedback", "0%");
            set_by_text(e, 0, "Time", "250 ms");
            settle(e);
            float p;
            render(e, 1, g_impulse, &p);
            long at = 0; float best = 0;
            for (long i = 0; i < FS; i++) if (fabsf(buf[i]) > best) { best = fabsf(buf[i]); at = i; }
            printf("  Digital Delay at 250 ms: echo at %.1f ms\n", 1000.0 * at / FS);
            CHECK(fabs(1000.0 * at / FS - 250) < 3, "Digital Delay's echo isn't at its Time");
            e->dispatcher(e, effClose, 0, 0, NULL, 0);
        }
        struct { const char *algo; float in; double want; const char *k1, *v1, *k2, *v2; } P[] = {
            {"Polyphny", 220, 330, "Level A", "0%", "Pitch B", "+7 st"},         /* a fifth up */
            {"Octaver", 220, 110, "Dry", "0%", NULL, NULL},
            {"Diatonik", 220, 261.6, "Level 2", "0%", "Voice 1", "3rd Up"},  /* A, a third up in C major: C */
            {"Diatonik", 261.6, 329.6, "Level 2", "0%", "Voice 1", "3rd Up"}, /* C, a third up: E */
            {"VocalTun", 226, 220, NULL, NULL, NULL, NULL},                     /* a sharp A pulled to A */
            {"PitchFlx", 220, 440, "Bend", "100%", NULL, NULL},                    /* toe: +12 */
            {"MicroPtch", 220, 221.15, "Delay L", "1.0", NULL, NULL},               /* +9 cents on the left */
        };
        for (size_t i = 0; i < sizeof P / sizeof *P; i++) {
            AEffect *e = one(P[i].algo, 1);
            if (P[i].k1) set_by_text(e, 0, P[i].k1, P[i].v1);
            if (P[i].k2) set_by_text(e, 0, P[i].k2, P[i].v2);
            settle(e);
            hz_in = P[i].in;
            float p;
            render(e, 4, g_sine, &p);
            const double got = peak_hz(buf, 2 * FS, 4 * FS, P[i].want * 0.8, P[i].want * 1.25);
            const double cents = 1200 * log2(got / P[i].want);
            printf("  %s, %.1f Hz in: strongest %.1f Hz (want %.1f, %+.0f cents)\n", P[i].algo, P[i].in, got, P[i].want, cents);
            CHECK(fabs(cents) < 15, "%s doesn't land on %.1f Hz", P[i].algo, P[i].want);
            e->dispatcher(e, effClose, 0, 0, NULL, 0);
        }
        {   /* Shimmer climbs: an octave above a held note builds up in the tail */
            AEffect *e = one("ShimmrVerb", 1);
            set_by_text(e, 0, "Shimmer", "100%");
            settle(e);
            hz_in = 220;
            float p;
            render(e, 6, g_sine, &p);
            const double o = tone_level(buf, 3 * FS, 6 * FS, 440), f = tone_level(buf, 3 * FS, 6 * FS, 220);
            printf("  Shimmer: 440 Hz %.4f, 220 Hz %.4f\n", o, f);
            CHECK(o > 0.1 * f, "Shimmer's octave is too faint");
            e->dispatcher(e, effClose, 0, 0, NULL, 0);
        }
        {   /* the reverbs leave a tail */
            const char *R[] = {"Hall", "Room", "Plate", "Spring", "ShimmrVerb", "Blackhol3", "SP2O16", "MangldVerb", "TremoloVrb", "Wormhol3"};
            for (size_t i = 0; i < sizeof R / sizeof *R; i++) {
                AEffect *e = one(R[i], 1);
                settle(e);
                float p;
                render(e, 2, g_impulse, &p);
                const double tail = rms(buf, FS / 4, FS / 2);
                printf("  %-13s tail 0.25-0.5 s: %.5f\n", R[i], tail);
                CHECK(tail > 2e-5, "%s leaves no tail", R[i]);
                e->dispatcher(e, effClose, 0, 0, NULL, 0);
            }
        }
        {   /* choosing an algorithm loads its defaults */
            AEffect *e = make();
            algo(e, 0, find("Digital Delay"));
            char t[64] = {0};
            e->dispatcher(e, effGetParamDisplay, P_K, 0, t, 0);
            printf("  Digital Delay's Time on choosing it: %s\n", t);
            CHECK(!strcmp(t, "375 ms"), "Digital Delay didn't load its default Time");
            e->dispatcher(e, effGetParamDisplay, 11, 0, t, 0);
            CHECK(!strcmp(t, "Time"), "knob 1's name isn't Time for Digital Delay");
            e->dispatcher(e, effClose, 0, 0, NULL, 0);
        }

        {   /* presets: > on slot A loads the first of its algorithm's, a project's saved index doesn't touch knobs */
            AEffect *e = make();
            char t[64] = {0}, al[64] = {0};
            e->dispatcher(e, effGetParamDisplay, P_ALGO, 0, al, 0);
            e->setParameter(e, 44, 1);   /* A Preset > */
            e->dispatcher(e, effGetParamDisplay, 42, 0, t, 0);
            char al2[64] = {0};
            e->dispatcher(e, effGetParamDisplay, P_ALGO, 0, al2, 0);
            printf("  presets: %s, > gives \"%s\" (algorithm still %s)\n", al, t, al2);
            CHECK(strcmp(t, "-") && !strcmp(al, al2), "Preset > didn't load one of the algorithm's presets");
            const float k1 = e->getParameter(e, P_K);
            e->setParameter(e, P_K, 0.123f);
            e->setParameter(e, 42, e->getParameter(e, 42));   /* (as a project load sets it) */
            CHECK(fabsf(e->getParameter(e, P_K) - 0.123f) < 1e-6f, "setting the preset index overwrote a knob");
            (void)k1;
            e->dispatcher(e, effClose, 0, 0, NULL, 0);
        }
        {   /* every preset: 8 s of sound, 8 s of silence: finite, under the ceiling, dying away (unless it holds) */
            AEffect *e = make();
            algo(e, 1, find("Off"));
            algo(e, 0, 0);
            int n = 0, bad = 0;
            char first[64] = "", t[64];
            for (;;) {
                e->setParameter(e, 44, 1);
                e->dispatcher(e, effGetParamDisplay, 42, 0, t, 0);
                if (!strcmp(t, first)) break;
                if (!n) strcpy(first, t);
                n++;
                e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);   /* (each from silence) */
                float peak;
                const int ok = render(e, 16, g_music8, &peak);
                char al[64] = {0};
                e->dispatcher(e, effGetParamDisplay, P_ALGO, 0, al, 0);
                const double end = rms(buf, 15 * FS, 16 * FS), before = rms(buf, 12 * FS, 13 * FS);
                int holds = 0;   /* a Decay / Feedback at full: meant to hold */
                for (int k = 0; k < 8; k++) {
                    char kn[64] = {0};
                    e->dispatcher(e, effGetParamName, P_K + k, 0, kn, 0);
                    if ((!strcmp(kn, "Decay") || !strcmp(kn, "Feedback")) && e->getParameter(e, P_K + k) > 0.999f) holds = 1;
                }
                if (!ok || peak >= 3.99f || !(holds || end < 0.003 || end < 0.85 * before)) {
                    printf("  FAIL  preset %s / %s: %s (peak %.2f, end %.4f, before %.4f)\n", al, t, !ok ? "not finite" : peak >= 3.99f ? "hits the ceiling" : "doesn't die away", peak, end, before);
                    bad++;
                }
                if (n > 2000) break;
            }
            printf("  presets: %d played, %d bad\n", n, bad);
            CHECK(n > 500 && !bad, "the presets");
            e->dispatcher(e, effClose, 0, 0, NULL, 0);
        }

        printf("CPU (128-frame blocks, this machine)\n");
        for (int a = 0; a < nalgo; a++) {
            AEffect *e = one(names[a], 0.5f);
            float il[N], ir[N], ol[N], orr[N];
            float *in[2] = {il, ir}, *out[2] = {ol, orr};
            const int blocks = 2000;
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int b = 0; b < blocks; b++) { for (int k = 0; k < N; k++) il[k] = ir[k] = g_music((long)b * N + k); e->processReplacing(e, in, out, N); }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            const double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / blocks;
            printf("  %-18s %.4f ms per block = %.2f%% of real time\n", names[a], ms, 100 * ms / (1000.0 * N / FS));
            e->dispatcher(e, effClose, 0, 0, NULL, 0);
        }
    }
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
