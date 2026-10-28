/* vocal_test: checks the vocal plugins offline (no MPC needed).
 *   vocal_test <plugin.so>        exit status 0 = PASSED
 * The input is a sung vowel made here: a band-limited pulse train (the glottis) through three formant filters (an
 * "ah": 700, 1220, 2600 Hz), with a little vibrato. Every plugin: silence stays silent, a NaN in doesn't come out,
 * random settings stay finite and under the ceiling, CPU. Then per plugin: VoxLive 3X's voices hit the scale's
 * notes (a 3rd up from A3 in C major = C4), keep the vowel (spectral centroid) unless Gender moves it, Lead Tune
 * pulls a sharp note in, MIDI Notes mode sings the held note; HedRush Harmony's two voices; Vocal Rack's tuner,
 * de-esser, gate, its effects and presets. */
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
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL  %s\n", msg); failures++; } } while (0)
static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) { (void)e; (void)op; (void)i; (void)v; (void)p; (void)o; return 0; }
static AEffect *(*entry)(audioMasterCallback);
static char product[64];
static AEffect *make(void) {
    AEffect *e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, FS);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    return e;
}
static int param(AEffect *e, const char *name) {
    for (int i = 0; i < e->numParams; i++) {
        char t[64] = {0};
        e->dispatcher(e, effGetParamName, i, 0, t, 0);
        if (!strcmp(t, name)) return i;
    }
    printf("  (no parameter %s)\n", name);
    failures++;
    return -1;
}
static void set(AEffect *e, const char *name, float v) { const int i = param(e, name); if (i >= 0) e->setParameter(e, i, v); }
/* set a parameter to the value whose display starts with `want` */
static int set_text(AEffect *e, const char *name, const char *want) {
    const int i = param(e, name);
    if (i < 0) return 0;
    for (int k = 0; k <= 2000; k++) {
        e->setParameter(e, i, (float)k / 2000);
        char t[64] = {0};
        e->dispatcher(e, effGetParamDisplay, i, 0, t, 0);
        if (!strncmp(t, want, strlen(want))) return 1;
    }
    printf("  (%s can't show %s)\n", name, want);
    failures++;
    return 0;
}
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4];
                 char detune, noteOffVelocity, reserved1, reserved2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstMidiEvent *events[2]; } VstEvents;
static void midi(AEffect *e, int a, int b, int c) {
    VstMidiEvent m = {1, (int32_t)sizeof m, 0, 0, 0, 0, {(unsigned char)a, (unsigned char)b, (unsigned char)c, 0}, 0, 0, 0, 0};
    VstEvents ev = {1, 0, {&m, NULL}};
    e->dispatcher(e, 25, 0, 0, &ev, 0);
}

/* ---- the sung vowel -------------------------------------------------------------------------------------------- */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } Bp;
static Bp bp(float hz, float q) {
    const float w = 2 * (float)M_PI * hz / FS, al = sinf(w) / (2 * q), a0 = 1 + al;
    return (Bp){al / a0, 0, -al / a0, -2 * cosf(w) / a0, (1 - al) / a0, 0, 0};
}
static float bpr(Bp *f, float x) { const float y = f->b0 * x + f->z1; f->z1 = f->b1 * x - f->a1 * y + f->z2; f->z2 = f->b2 * x - f->a2 * y; return y; }
static float vowel_hz, vowel_level = 0.3f, vowel_vib = 0.3f;
static double vph;
static Bp F[3];
static void vowel_reset(void) { vph = 0; F[0] = bp(700, 4); F[1] = bp(1220, 6); F[2] = bp(2600, 8); }
static float vowel(long t) {   /* band-limited pulse train (sum of harmonics) through the formants */
    const double f = vowel_hz * pow(2, vowel_vib / 12 * sin(2 * M_PI * 5.5 * (double)t / FS) * 0.15);
    vph += f / FS;
    vph -= floor(vph);
    float x = 0;
    const int nh = (int)(9000 / f);
    for (int h = 1; h <= nh; h++) x += (float)sin(2 * M_PI * vph * h) / (float)sqrt(h);
    x *= 0.15f;
    return vowel_level * (bpr(&F[0], x) * 1.0f + bpr(&F[1], x) * 0.6f + bpr(&F[2], x) * 0.35f) * 4;
}
static float noise_s(long t) { (void)t; return ((float)rand() / RAND_MAX * 2 - 1) * 0.3f; }
static float zero(long t) { (void)t; return 0; }

static float bufl[12 * FS], bufr[12 * FS];
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
/* the pitch of x[a..b) (mono of both channels): autocorrelation peak, 60..1100 Hz, refined */
static double pitch_of(long a, long b) {
    static float m[12 * FS];
    for (long i = a; i < b; i++) m[i - a] = bufl[i] + bufr[i];
    const long len = b - a, W = len - FS / 50;
    static double cor[FS / 60 + 1];
    double best = -1;
    for (int tau = FS / 1100; tau <= FS / 60; tau++) {
        double sm = 0, e0 = 0, e1 = 0;
        for (long i = 0; i < W; i += 2) { sm += m[i] * m[i + tau]; e0 += m[i] * m[i]; e1 += m[i + tau] * m[i + tau]; }
        cor[tau] = sm / sqrt(e0 * e1 + 1e-12);
        if (cor[tau] > best) best = cor[tau];
    }
    int bt = FS / 60;   /* the shortest period whose peak is nearly the best (a periodic sound repeats at 2T too) */
    for (int tau = FS / 1100 + 1; tau < FS / 60; tau++)
        if (cor[tau] >= 0.97 * best && cor[tau] >= cor[tau - 1] && cor[tau] >= cor[tau + 1]) { bt = tau; break; }
    return (double)FS / bt;
}
static double centroid(long a, long b) {   /* spectral centroid, Hann-windowed DFT every 10 Hz up to 6 kHz */
    double num = 0, den = 0;
    const long n = b - a;
    for (int k = 5; k <= 600; k++) {
        const double f = k * 10.0;
        double re = 0, im = 0;
        for (long i = 0; i < n; i++) {
            const double w = 0.5 - 0.5 * cos(2 * M_PI * i / n), v = (bufl[a + i] + bufr[a + i]) * w;
            re += v * cos(2 * M_PI * f * i / FS); im += v * sin(2 * M_PI * f * i / FS);
        }
        const double p = re * re + im * im;
        num += f * p; den += p;
    }
    return num / (den + 1e-12);
}
static double rms(long a, long b) { double s = 0; for (long i = a; i < b; i++) s += bufl[i] * bufl[i]; return sqrt(s / (double)(b - a)); }
static double cents(double f, double ref) { return 1200 * log2(f / ref); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vocal_test <plugin.so>\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    entry = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    AEffect *e = make();
    e->dispatcher(e, effGetEffectName, 0, 0, product, 0);
    char vendor[64] = {0};
    e->dispatcher(e, effGetVendorString, 0, 0, vendor, 0);
    printf("%s (%s), %d parameters\n", product, vendor, e->numParams);
    CHECK(!strcmp(vendor, "Other Things"), "vendor isn't Other Things");
    float peak;
    vowel_reset();

    /* silence in, silence out; a NaN in doesn't come out */
    CHECK(render(e, 1, zero, &peak) && peak < 1e-5f, "silence doesn't stay silent");
    {
        float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
        for (int k = 0; k < N; k++) il[k] = ir[k] = k == 5 ? NAN : 0.1f;
        e->processReplacing(e, in, out, N);
        int ok = 1;
        for (int k = 0; k < N; k++) if (!isfinite(ol[k]) || !isfinite(orr[k])) ok = 0;
        CHECK(ok, "a NaN in came out");
    }
    /* random settings: finite, under the ceiling */
    srand(7);
    for (int trial = 0; trial < 40; trial++) {
        for (int i = 0; i < e->numParams; i++) {
            char nm[64] = {0};
            e->dispatcher(e, effGetParamName, i, 0, nm, 0);
            if (strstr(nm, "<") || strstr(nm, ">") || strstr(nm, "List")) continue;
            e->setParameter(e, i, (float)rand() / RAND_MAX);
        }
        vowel_hz = 90 + (float)(rand() % 500);
        int ok = render(e, 1.5f, trial % 3 ? vowel : noise_s, &peak);
        if (!ok || peak >= 3.99f) { printf("  FAIL  random settings %d: %s (peak %.2f)\n", trial, ok ? "ceiling" : "not finite", peak); failures++; }
    }
    e->dispatcher(e, effClose, 0, 0, NULL, 0);

    if (!strcmp(product, "VoxLive 3X")) {
        e = make();
        set_text(e, "Mode", "Scale"); set_text(e, "Key", "C"); set_text(e, "Scale", "Major");
        set(e, "Lead", 0); set(e, "Double", 0); set(e, "Humanize", 0); set_text(e, "Lead Tune", "0%");
        set_text(e, "Voice 1", "3rd Up"); set_text(e, "Voice 2", "Off"); set_text(e, "Voice 3", "Off"); set_text(e, "Voice 4", "Off");
        set_text(e, "V1 Gender", "0");
        vowel_hz = 220; vowel_vib = 0; vowel_reset();
        render(e, 3, vowel, &peak);
        const double f = pitch_of(FS, 3 * FS), c_in = 0;
        printf("  3rd up from A3 (220 Hz) in C major: %.1f Hz (%+.0f cents from C4)\n", f, cents(f, 261.63));
        CHECK(fabs(cents(f, 261.63)) < 20, "the 3rd up isn't C4");
        const double cen = centroid(FS, FS + 4096);
        (void)c_in;
        /* the same vowel sung at C4, for comparison (formants kept: the harmony should sound like it) */
        AEffect *d = make();
        set(d, "Lead", 1); set(d, "Harmony", 0); set_text(d, "Lead Tune", "0%"); set(d, "Double", 0);
        vowel_hz = 261.63f; vowel_reset();
        render(d, 2, vowel, &peak);
        const double cen_dry = centroid(FS, FS + 4096);
        d->dispatcher(d, effClose, 0, 0, NULL, 0);
        vowel_hz = 220;
        printf("  vowel kept: centroid %.0f Hz (the vowel sung at C4: %.0f Hz)\n", cen, cen_dry);
        CHECK(fabs(cen / cen_dry - 1) < 0.2, "the harmony doesn't keep the vowel (formants)");
        set_text(e, "V1 Gender", "50");
        vowel_reset();
        render(e, 2, vowel, &peak);
        const double cen_g = centroid(FS, FS + 4096);
        printf("  Gender +50: centroid %.0f Hz\n", cen_g);
        CHECK(cen_g > cen * 1.15, "Gender +50 didn't raise the formants");
        set_text(e, "V1 Gender", "0");
        /* the 5th up and an octave down */
        set_text(e, "Voice 1", "5th Up");
        vowel_reset(); render(e, 3, vowel, &peak);
        double f2 = pitch_of(FS, 3 * FS);
        printf("  5th up from A3: %.1f Hz (%+.0f cents from E4)\n", f2, cents(f2, 329.63));
        CHECK(fabs(cents(f2, 329.63)) < 20, "the 5th up isn't E4");
        set_text(e, "Voice 1", "Oct Down");
        vowel_reset(); render(e, 3, vowel, &peak);
        f2 = pitch_of(FS, 3 * FS);
        printf("  octave down from A3: %.1f Hz (%+.0f cents from A2)\n", f2, cents(f2, 110));
        CHECK(fabs(cents(f2, 110)) < 20, "the octave down isn't A2");
        /* MIDI Notes mode: the voice sings the held note */
        set_text(e, "Voice 1", "3rd Up");
        set_text(e, "Mode", "MIDI Notes");
        midi(e, 0x90, 67, 100);   /* G4 */
        vowel_reset(); render(e, 3, vowel, &peak);
        f2 = pitch_of(FS, 3 * FS);
        printf("  MIDI Notes, G4 held: %.1f Hz (%+.0f cents from G4)\n", f2, cents(f2, 392.0));
        CHECK(fabs(cents(f2, 392.0)) < 20, "MIDI Notes mode doesn't sing the held note");
        midi(e, 0x80, 67, 0);
        /* MIDI Chord mode: A minor held (A C E), lead A3, voice "3rd Up" = the next chord tone above = C4 */
        set_text(e, "Mode", "MIDI Chord");
        midi(e, 0x90, 57, 100); midi(e, 0x90, 60, 100); midi(e, 0x90, 64, 100);
        set_text(e, "Voice 1", "5th Up");   /* the 2nd chord tone above A: E4 */
        vowel_reset(); render(e, 3, vowel, &peak);
        f2 = pitch_of(FS, 3 * FS);
        printf("  MIDI Chord (A C E held), \"5th Up\" from A3: %.1f Hz (%+.0f cents from E4)\n", f2, cents(f2, 329.63));
        CHECK(fabs(cents(f2, 329.63)) < 20, "MIDI Chord mode doesn't take the chord's tones");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
        /* Lead Tune: 50 cents sharp of A3 comes out as A3 */
        e = make();
        set(e, "Harmony", 0); set(e, "Double", 0); set(e, "Lead", 1);
        set_text(e, "Key", "C"); set_text(e, "Scale", "Major");
        set_text(e, "Lead Tune", "100%"); set_text(e, "Tune Speed", "1.0");
        vowel_hz = 220 * powf(2, 40.0f / 1200); vowel_reset();
        render(e, 3, vowel, &peak);
        f2 = pitch_of(FS, 3 * FS);
        printf("  Lead Tune 100%% on A3 + 40 cents: %.1f Hz (%+.0f cents from A3)\n", f2, cents(f2, 220));
        CHECK(fabs(cents(f2, 220)) < 15, "Lead Tune didn't pull the note in");
        /* styles step and set voices */
        char t[64] = {0}, t2[64] = {0};
        e->setParameter(e, param(e, "Style >"), 1);
        e->dispatcher(e, effGetParamDisplay, param(e, "Style"), 0, t, 0);
        e->setParameter(e, param(e, "Style >"), 1);
        e->dispatcher(e, effGetParamDisplay, param(e, "Style"), 0, t2, 0);
        printf("  styles: \"%s\", then \"%s\"\n", t, t2);
        CHECK(strcmp(t, "-") && strcmp(t, t2), "Style > doesn't step");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
        vowel_vib = 0.3f;
    }
    if (!strcmp(product, "HedRush Harmony")) {
        e = make();
        set_text(e, "Key", "G"); set_text(e, "Scale", "Major");
        set_text(e, "Voice 1", "3rd Up"); set(e, "V1 Level", 1); set_text(e, "Voice 2", "Off"); set(e, "Dry", 0);
        vowel_hz = 196; vowel_vib = 0; vowel_reset();   /* G3: a 3rd up in G major = B3 */
        render(e, 3, vowel, &peak);
        double f = pitch_of(FS, 3 * FS);
        printf("  3rd up from G3 in G major: %.1f Hz (%+.0f cents from B3)\n", f, cents(f, 246.94));
        CHECK(fabs(cents(f, 246.94)) < 20, "voice 1 isn't B3");
        set_text(e, "Voice 1", "Off"); set_text(e, "Voice 2", "Oct Up"); set(e, "V2 Level", 1);
        vowel_reset(); render(e, 3, vowel, &peak);
        f = pitch_of(FS, 3 * FS);
        printf("  voice 2 an octave up from G3: %.1f Hz (%+.0f cents from G4)\n", f, cents(f, 392));
        CHECK(fabs(cents(f, 392)) < 20, "voice 2 isn't G4");
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
        vowel_vib = 0.3f;
    }
    if (!strcmp(product, "Vocal Rack")) {
        e = make();
        set(e, "Tune", 1); set_text(e, "Key", "C"); set_text(e, "Scale", "Major"); set(e, "Tune Amount", 1);
        set_text(e, "Tune Speed", "1.0");
        set(e, "Reverb", 0); set(e, "Delay", 0); set(e, "Double", 0); set(e, "Gate", 0); set(e, "Comp", 0);
        set(e, "De-Ess", 0); set(e, "EQ", 0); set_text(e, "FX", "Off");
        vowel_hz = 220 * powf(2, 40.0f / 1200); vowel_vib = 0; vowel_reset();
        render(e, 3, vowel, &peak);
        double f = pitch_of(FS, 3 * FS);
        printf("  tuner on A3 + 40 cents: %.1f Hz (%+.0f cents from A3)\n", f, cents(f, 220));
        CHECK(fabs(cents(f, 220)) < 15, "the tuner didn't pull the note in");
        set(e, "Tune", 0);
        set_text(e, "FX", "Robot");
        vowel_hz = 233; vowel_reset(); render(e, 3, vowel, &peak);
        f = pitch_of(FS, 3 * FS);
        printf("  Robot: %.1f Hz (one fixed note: C3 = 130.8 Hz)\n", f);
        CHECK(fabs(cents(f, 130.81)) < 25, "Robot doesn't hold its note");
        set_text(e, "FX", "Octave Down");
        vowel_hz = 220; vowel_reset(); render(e, 3, vowel, &peak);
        f = pitch_of(FS, 3 * FS);
        printf("  Octave Down on A3: %.1f Hz\n", f);
        CHECK(fabs(cents(f, 110)) < 25, "Octave Down isn't an octave down");
        set_text(e, "FX", "Off");
        /* the gate: a quiet hum under the threshold is shut */
        set(e, "Gate", 1); set_text(e, "Gate Thresh", "-40");
        vowel_level = 0.003f; vowel_reset(); render(e, 2, vowel, &peak);
        const double gated = rms(FS, 2 * FS);
        vowel_level = 0.3f;
        printf("  gate at -40 dB, a -50 dB voice: rms %.5f\n", gated);
        CHECK(gated < 0.0003, "the gate doesn't shut");
        set(e, "Gate", 0);
        /* the de-esser cuts a hiss more than the vowel */
        set(e, "De-Ess", 1); set(e, "De-Ess Amount", 1);
        render(e, 2, noise_s, &peak);
        const double hiss_on = rms(FS, 2 * FS);
        set(e, "De-Ess", 0);
        render(e, 2, noise_s, &peak);
        const double hiss_off = rms(FS, 2 * FS);
        printf("  de-esser on a hiss: rms %.4f -> %.4f\n", hiss_off, hiss_on);
        CHECK(hiss_on < hiss_off * 0.8, "the de-esser doesn't cut a hiss");
        /* presets step */
        char t[64] = {0}, t2[64] = {0};
        e->setParameter(e, param(e, "Preset >"), 1);
        e->dispatcher(e, effGetParamDisplay, param(e, "Preset"), 0, t, 0);
        e->setParameter(e, param(e, "Preset >"), 1);
        e->dispatcher(e, effGetParamDisplay, param(e, "Preset"), 0, t2, 0);
        printf("  presets: \"%s\", then \"%s\"\n", t, t2);
        CHECK(strcmp(t, "-") && strcmp(t, t2), "Preset > doesn't step");
        /* every preset: finite, under the ceiling */
        char first[64] = "";
        for (int k = 0; k < 64; k++) {
            e->setParameter(e, param(e, "Preset >"), 1);
            e->dispatcher(e, effGetParamDisplay, param(e, "Preset"), 0, t, 0);
            if (!strcmp(t, first)) break;
            if (!k) strcpy(first, t);
            vowel_hz = 180; vowel_reset();
            const int ok = render(e, 2, vowel, &peak);
            if (!ok || peak >= 3.99f) { printf("  FAIL  preset %s (peak %.2f)\n", t, peak); failures++; }
        }
        e->dispatcher(e, effClose, 0, 0, NULL, 0);
        vowel_vib = 0.3f;
    }

    /* CPU, at its defaults, on the vowel */
    e = make();
    {
        float il[N], ir[N], ol[N], orr[N], *in[2] = {il, ir}, *out[2] = {ol, orr};
        vowel_hz = 200; vowel_reset();
        const int blocks = 3000;
        for (long i = 0; i < (long)blocks * N && i < 12 * FS; i++) bufl[i] = vowel(i);   /* (made before the clock starts) */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int b = 0; b < blocks; b++) { for (int k = 0; k < N; k++) il[k] = ir[k] = bufl[(long)b * N + k]; e->processReplacing(e, in, out, N); }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / blocks;
        printf("  CPU: %.4f ms per 128-frame block = %.2f%% of real time (this machine)\n", ms, 100 * ms / (1000.0 * N / FS));
    }
    e->dispatcher(e, effClose, 0, 0, NULL, 0);
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
