/* tri_test: checks Tri Chorus offline.   tri_test <Tri-Chorus.so>      exit status 0 = PASSED
 * With its defaults, every knob at minimum, at maximum and at random settings: 10 s of sound then 5 s of silence
 * stays finite, under the +12 dB ceiling and dies away. Then: the chorus changes the sound and spreads it in
 * stereo, Vibrato mode moves the pitch (no dry), Shimmer adds the octave above. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../nam-mpc/src/vst2.h"

enum { kMix, kRate, kDepth, kEnsemble, kWidth, kTone, kDelay, kShimmer, kShimPitch, kMode, kOutput, kNumParams };
#define FS 44100
#define N 128
static int failures;
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
static float bl[16 * FS], br[16 * FS], inb[16 * FS];
static unsigned rng = 1;
static float noise(void) { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 8388608.0f - 1; }
static float hz = 220;
static float g_music(long t) {
    if (t >= 10 * FS) return 0;
    const long u = t % (FS / 2);
    float v = 0;
    for (int h = 1; h <= 5; h++) v += sinf(2 * (float)M_PI * hz * (float)h * (float)u / FS) / (float)h;
    return 0.35f * expf(-(float)u / (0.3f * FS)) * v + 0.01f * noise();
}
static float g_sine(long t) { return 0.5f * sinf(2 * (float)M_PI * hz * (float)t / FS); }
static int render(AEffect *e, float secs, float (*g)(long), float *peak) {
    float il[N], ir[N], ol[N], orr[N];
    float *in[2] = {il, ir}, *out[2] = {ol, orr};
    const long total = (long)(secs * FS);
    *peak = 0;
    for (long t = 0; t < total; t += N) {
        for (int k = 0; k < N; k++) { il[k] = ir[k] = g(t + k); if (t + k < total) inb[t + k] = il[k]; }
        e->processReplacing(e, in, out, N);
        for (int k = 0; k < N; k++) {
            if (!isfinite(ol[k]) || !isfinite(orr[k])) return 0;
            *peak = fmaxf(*peak, fmaxf(fabsf(ol[k]), fabsf(orr[k])));
            if (t + k < total) { bl[t + k] = ol[k]; br[t + k] = orr[k]; }
        }
    }
    return 1;
}
static double rms(const float *x, long a, long b) { double s = 0; for (long i = a; i < b; i++) s += x[i] * x[i]; return sqrt(s / (double)(b - a)); }
static double tone(const float *x, long a, long b, double f) {
    double sum = 0; int n = 0;
    for (long i = a; i + 8192 <= b; i += 8192, n++) {
        const double c = 2 * cos(2 * M_PI * f / FS);
        double s1 = 0, s2 = 0;
        for (long j = i; j < i + 8192; j++) { const double s0 = x[j] + c * s1 - s2; s2 = s1; s1 = s0; }
        sum += sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / 8192;
    }
    return n ? sum / n : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <Tri-Chorus.so>\n", argv[0]); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h || !(*(void **)&entry = dlsym(h, "VSTPluginMain"))) { fprintf(stderr, "can't load %s\n", argv[1]); return 1; }
    static const char *SET[4] = {"defaults", "min", "max", "random"};
    for (int setting = 0; setting < 4; setting++)
        for (int rep = 0; rep < (setting == 3 ? 6 : 1); rep++) {
            AEffect *e = make();
            for (int i = 0; i < kNumParams && setting; i++)
                if (i != kOutput) e->setParameter(e, i, setting == 1 ? 0.0f : setting == 2 ? 1.0f : (noise() + 1) * 0.5f);
            float peak;
            const int ok = render(e, 15, g_music, &peak);
            CHECK(ok, "%s: not finite", SET[setting]);
            CHECK(peak < 3.99f, "%s: hits the +12 dB ceiling (%.2f)", SET[setting], peak);
            CHECK(rms(bl, 14 * FS, 15 * FS) < 0.001, "%s: still sounding 4 s after the input stopped", SET[setting]);
            e->dispatcher(e, effClose, 0, 0, NULL, 0);
        }
    {   /* the chorus: changes the sound, left and right differ */
        AEffect *e = make();
        float p;
        render(e, 10, g_music, &p);
        double d = 0, lr = 0;
        for (long i = FS; i < 9 * FS; i++) { d += (bl[i] - inb[i]) * (bl[i] - inb[i]); lr += (bl[i] - br[i]) * (bl[i] - br[i]); }
        d = sqrt(d / (8.0 * FS)); lr = sqrt(lr / (8.0 * FS));
        printf("  chorus: differs from the input by %.3f, left from right by %.3f\n", d, lr);
        CHECK(d > 0.02 && lr > 0.01, "the chorus doesn't change or spread the sound");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    {   /* vibrato: a 1 kHz tone smears into neighbouring frequencies, little left exactly at 1 kHz */
        hz = 1000;
        AEffect *e = make();
        e->setParameter(e, kMode, 1);
        e->setParameter(e, kDepth, 1);
        float p;
        render(e, 4, g_sine, &p);
        const double at = tone(bl, FS, 4 * FS, 1000), in_at = tone(inb, FS, 4 * FS, 1000);
        printf("  vibrato: 1 kHz kept %.3f of %.3f\n", at, in_at);
        CHECK(at < 0.8 * in_at, "Vibrato doesn't move the pitch");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    {   /* shimmer: the octave above appears */
        hz = 220;
        float p;
        AEffect *e = make();
        render(e, 4, g_sine, &p);
        const double off = tone(bl, FS, 4 * FS, 440);
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
        e = make();
        e->setParameter(e, kShimmer, 1);
        render(e, 4, g_sine, &p);
        const double on = tone(bl, FS, 4 * FS, 440), base = tone(bl, FS, 4 * FS, 220);
        printf("  shimmer: 440 Hz %.4f (off: %.4f), 220 Hz %.4f\n", on, off, base);
        CHECK(on > 10 * off + 0.01, "Shimmer adds no octave");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
    }
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
