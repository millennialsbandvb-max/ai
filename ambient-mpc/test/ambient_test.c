/* ambient_test: checks Ambient Lab offline (no MPC needed).   ambient_test <Ambient-Lab.so>   exit 0 = PASSED
 * Every machine at its defaults, all knobs at minimum, at maximum, at 80% and at random settings: 6 s of playing then
 * 10 s of silence stays finite and under the +12 dB ceiling, and (except at maximum, where feedback / decay may hold)
 * dies away. Then what some should do: Rainbow Mchn +12 makes the octave, Data Korrupt's /2 sub is an octave under,
 * Tensr at half speed is an octave down, Hedr4's voice repeats after its Time, Mikrocosm's Hold keeps sounding after
 * you stop, NiteSky / Cloudbrst ring on; every preset plays; CPU per machine. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../nam-mpc/src/vst2.h"

#define FS 48000
#define N 128
enum { kMachine, kMachineOpen, kMix, kK, kHold = kK + 8, kPreset, kPresetPrev, kPresetNext, kOutput };
#define NM 11
static int failures;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL  %s\n", msg); failures++; } } while (0)
static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) { (void)e; (void)op; (void)i; (void)v; (void)p; (void)o; return 0; }
static AEffect *(*entry)(audioMasterCallback);
static AEffect *make(int machine, float mix) {
    AEffect *e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, FS);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    e->setParameter(e, kMachine, (float)machine / (NM - 1));
    e->setParameter(e, kMix, mix);
    return e;
}
static int knob_by_name(AEffect *e, const char *name) {
    for (int k = 0; k < 8; k++) { char t[64] = {0}; e->dispatcher(e, effGetParamName, kK + k, 0, t, 0); if (!strcmp(t, name)) return kK + k; }
    printf("  (no knob %s)\n", name); failures++; return -1;
}
static void set_text(AEffect *e, const char *name, const char *want) {
    const int i = knob_by_name(e, name);
    if (i < 0) return;
    for (int k = 0; k <= 2000; k++) {
        e->setParameter(e, i, (float)k / 2000);
        char t[64] = {0};
        e->dispatcher(e, effGetParamDisplay, i, 0, t, 0);
        if (!strncmp(t, want, strlen(want))) return;
    }
    printf("  (%s can't show %s)\n", name, want); failures++;
}
static float hz_in;
static float g_sine(long t) { return 0.4f * sinf(2 * (float)M_PI * hz_in * (float)t / FS); }
static float g_zero(long t) { (void)t; return 0; }
static float g_music(long t) {   /* notes every half second, decaying, for 6 s */
    if (t >= 6 * FS) return 0;
    static const float NOTES[8] = {220, 277.2f, 329.6f, 440, 392, 293.7f, 246.9f, 196};
    const long n = t / (FS / 2), u = t % (FS / 2);
    const float f = NOTES[n % 8], a = 0.4f * expf(-(float)u / (0.25f * FS));
    return a * (sinf(2 * (float)M_PI * f * (float)t / FS) + 0.3f * sinf(4 * (float)M_PI * f * (float)t / FS));
}
static float bufl[20 * FS], bufr[20 * FS];
static int render(AEffect *e, float secs, float (*gen)(long), float *peak) {
    const long n = (long)(secs * FS);
    float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
    int ok = 1;
    *peak = 0;
    for (long b = 0; b < n; b += N) {
        for (int k = 0; k < N; k++) il[k] = ir[k] = gen(b + k);
        e->processReplacing(e, in, out, N);
        for (int k = 0; k < N && b + k < n; k++) {
            if (!isfinite(ol[k]) || !isfinite(orr[k])) ok = 0;
            bufl[b + k] = ol[k]; bufr[b + k] = orr[k];
            if (fabsf(ol[k]) > *peak) *peak = fabsf(ol[k]);
            if (fabsf(orr[k]) > *peak) *peak = fabsf(orr[k]);
        }
    }
    return ok;
}
static double rms(long a, long b) { double s = 0; for (long i = a; i < b; i++) s += 0.5 * (bufl[i] * bufl[i] + bufr[i] * bufr[i]); return sqrt(s / (double)(b - a)); }
static double tone(long a, long b, double f) {   /* the amplitude of frequency f in [a, b) */
    double re = 0, im = 0;
    for (long i = a; i < b; i++) { const double v = bufl[i] + bufr[i]; re += v * cos(2 * M_PI * f * i / FS); im += v * sin(2 * M_PI * f * i / FS); }
    return sqrt(re * re + im * im) / (double)(b - a);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ambient_test <Ambient-Lab.so>\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    entry = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    char names[NM][64];
    srand(3);
    printf("stability\n");
    for (int m = 0; m < NM; m++) {
        AEffect *e = make(m, 0.6f);
        e->dispatcher(e, effGetParamDisplay, kMachine, 0, names[m], 0);
        for (int mode = 0; mode < 6; mode++) {   /* defaults, min, max, 80%, random, random */
            AEffect *x = make(m, 0.6f);
            if (mode > 0) for (int k = 0; k < 8; k++) x->setParameter(x, kK + k, mode == 1 ? 0.0f : mode == 2 ? 1.0f : mode == 3 ? 0.8f : (float)rand() / RAND_MAX);
            float peak;
            const int ok = render(x, 16, g_music, &peak);
            const double end = rms(15 * FS, 16 * FS), before = rms(12 * FS, 13 * FS);
            const int dies = mode == 2 || end < 0.003 || end < 0.85 * before;
            if (!ok || peak >= 3.99f || !dies) {
                static const char *const MODE[] = {"defaults", "min", "max", "80%", "random", "random"};
                printf("  FAIL  %s %s: %s (peak %.2f, end %.4f, before %.4f)\n", names[m], MODE[mode], !ok ? "not finite" : peak >= 3.99f ? "hits the ceiling" : "doesn't die away", peak, end, before);
                failures++;
            }
            x->dispatcher(x, effClose, 0, 0, NULL, 0);
        }
        printf("  %-13s ok\n", names[m]);
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    float peak;
    {   /* Rainbow Mchn: +12, tracking and mod off: the octave */
        AEffect *e = make(9, 1.0f);
        set_text(e, "Pitch", "+12"); set_text(e, "Tracking", "0%"); set_text(e, "Mod Depth", "0%"); set_text(e, "Magic", "0%");
        set_text(e, "Second", "Off"); set_text(e, "Tone", "100%");
        hz_in = 220;
        render(e, 2, g_sine, &peak);
        const double up = tone(FS / 2, 2 * FS, 440), base = tone(FS / 2, 2 * FS, 220);
        printf("  Rainbow Mchn +12 on 220 Hz: 440 Hz %.3f, 220 Hz %.3f\n", up, base);
        CHECK(up > 4 * base, "Rainbow Mchn doesn't make the octave");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    {   /* Data Korrupt: only the /2 sub, on 220 Hz: 110 Hz */
        AEffect *e = make(10, 1.0f);
        set_text(e, "Interval", "+0"); set_text(e, "Sub", "/2"); set_text(e, "Square", "0%"); set_text(e, "Sub Level", "100%");
        set_text(e, "Fuzz", "0%"); set_text(e, "Tone", "100%");
        hz_in = 220;
        render(e, 3, g_sine, &peak);
        const double sub = tone(FS, 3 * FS, 110), other = tone(FS, 3 * FS, 220);
        printf("  Data Korrupt /2 on 220 Hz: 110 Hz %.3f, 220 Hz %.3f\n", sub, other);
        CHECK(sub > 2 * other && sub > 0.02, "Data Korrupt's sub isn't an octave under");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    {   /* Tensr at half speed: an octave down */
        AEffect *e = make(3, 1.0f);
        set_text(e, "Speed", "0.50x"); set_text(e, "Stretch", "1.00x"); set_text(e, "Chance", "0%"); set_text(e, "Wow", "0%");
        set_text(e, "Pitch", "+0"); set_text(e, "Tone", "100%"); set_text(e, "Length", "4.00");
        hz_in = 440;
        render(e, 3, g_sine, &peak);
        const double dn = tone(FS, 3 * FS, 220), base = tone(FS, 3 * FS, 440);
        printf("  Tensr half speed on 440 Hz: 220 Hz %.3f, 440 Hz %.3f\n", dn, base);
        CHECK(dn > 3 * base, "Tensr at half speed isn't an octave down");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    {   /* Hedr4: a click comes back after Time (450 ms), shifted */
        AEffect *e = make(7, 1.0f);
        set_text(e, "Time", "450"); set_text(e, "Feedback", "0%"); set_text(e, "Rhythm", "Even");
        static float imp_t;
        (void)imp_t;
        hz_in = 440;
        render(e, 0.5f, g_sine, &peak);   /* half a second of tone, then silence */
        float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
        double early = 0, late = 0;
        for (int b = 0; b < (int)(1.5 * FS / N); b++) {
            for (int k = 0; k < N; k++) il[k] = ir[k] = 0;
            e->processReplacing(e, in, out, N);
            for (int k = 0; k < N; k++) {
                const double t = (double)(b * N + k) / FS, p = ol[k] * ol[k] + orr[k] * orr[k];
                if (t < 0.3) early += p; else if (t > 0.35 && t < 1.0) late += p;
            }
        }
        printf("  Hedr4 after the tone stops: %.4f in the first 0.3 s, %.4f later (the repeats)\n", early, late);
        CHECK(late > 0.01, "Hedr4 doesn't repeat");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    {   /* Mikrocosm: Hold keeps the clouds going after you stop */
        AEffect *e = make(0, 1.0f);
        render(e, 3, g_music, &peak);
        e->setParameter(e, kHold, 1);
        render(e, 5, g_zero, &peak);
        const double held = rms(3 * FS, 5 * FS);
        printf("  Mikrocosm held, 3-5 s after the input stops: rms %.4f\n", held);
        CHECK(held > 0.003, "Hold doesn't keep Mikrocosm sounding");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    for (int m = 4; m <= 5; m++) {   /* the reverbs ring on */
        AEffect *e = make(m, 1.0f);
        render(e, 8, g_music, &peak);
        const double tail = rms(6 * FS + FS / 2, 7 * FS + FS / 2);
        printf("  %s, 0.5-1.5 s after the playing stops: rms %.4f\n", names[m], tail);
        CHECK(tail > 0.003, "a reverb machine doesn't ring on");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    {   /* every preset */
        AEffect *e = make(0, 0.5f);
        int n = 0, bad = 0;
        char first[64] = "", t[64];
        for (;;) {
            e->setParameter(e, kPresetNext, 1);
            e->dispatcher(e, effGetParamDisplay, kPreset, 0, t, 0);
            if (!strcmp(t, first)) break;
            if (!n) strcpy(first, t);
            n++;
            e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
            const int ok = render(e, 10, g_music, &peak);
            if (!ok || peak >= 3.99f) { printf("  FAIL  preset %s (peak %.2f)\n", t, peak); bad++; }
            if (n > 100) break;
        }
        printf("  presets: %d played, %d bad\n", n, bad);
        CHECK(n >= 30 && !bad, "the presets");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    printf("CPU (128-frame blocks at 48 kHz, this machine)\n");
    for (int m = 0; m < NM; m++) {
        AEffect *e = make(m, 0.5f);
        float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
        for (long i = 0; i < 4 * FS; i++) bufl[i] = g_music(i % (6 * FS));
        const int blocks = (int)(4 * FS / N);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int b = 0; b < blocks; b++) { for (int k = 0; k < N; k++) il[k] = ir[k] = bufl[b * N + k]; e->processReplacing(e, in, out, N); }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / blocks;
        printf("  %-13s %.2f%% of real time\n", names[m], 100 * ms / (1000.0 * N / FS));
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
