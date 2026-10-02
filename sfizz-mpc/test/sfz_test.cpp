/* Offline host for SFZ MPC: loads it like MPC does, waits for instruments to load, plays them, and times it.
 *   sfz-test <SFZ-MPC.so>          (SFZ_MPC_DIR=<folder with .sfz instruments>; exit status 0 = PASSED)
 * Checks: the instrument list (included .sfz pieces left out), every instrument makes sound from a note, the arrows
 * step through them, a saved project restores the same instrument by name, output stays finite. Then times a held
 * 4-note chord in 128-frame blocks at 44.1 kHz. */
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>

#include "vst2.h"

enum { kInstrument, kPrev, kNext, kVolume, kVoices, kQuality, kNumParams };
enum { effProcessEvents = 25 };
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4];
                 char detune, noteOffVelocity, r1, r2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; void *events[8]; } VstEvents;

static int failures = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  ok    " : "  FAIL  "); std::printf(__VA_ARGS__); std::printf("\n"); if (!(c)) failures++; } while (0)
static intptr_t host(AEffect *, int32_t op, int32_t, intptr_t, void *, float) { return op == 1 ? 2400 : 0; }

static const int N = 128;
static float L[N], R[N];
static float *outs[2] = {L, R};

static double run(AEffect *e, int blocks) {   // -> peak, -1 if not finite
    double peak = 0;
    for (int b = 0; b < blocks; b++) {
        e->processReplacing(e, nullptr, outs, N);
        for (int k = 0; k < N; k++) {
            if (!std::isfinite(L[k]) || !std::isfinite(R[k])) return -1;
            peak = std::max(peak, (double)std::max(std::fabs(L[k]), std::fabs(R[k])));
        }
    }
    return peak;
}

static void midi(AEffect *e, std::vector<std::array<unsigned char, 3>> msgs) {
    VstMidiEvent ev[8] = {};
    VstEvents list = {};
    for (size_t i = 0; i < msgs.size() && i < 8; i++) {
        ev[i].type = 1; ev[i].byteSize = sizeof(VstMidiEvent); ev[i].deltaFrames = (int)i * 3;
        std::memcpy(ev[i].midiData, msgs[i].data(), 3);
        list.events[i] = &ev[i];
    }
    list.numEvents = (int)std::min<size_t>(msgs.size(), 8);
    e->dispatcher(e, effProcessEvents, 0, 0, &list, 0);
}

static std::string display(AEffect *e, int i) {
    char b[64] = {0};
    e->dispatcher(e, effGetParamDisplay, i, 0, b, 0);
    return b;
}

static std::string settle(AEffect *e, const std::string &not_this = "") {   // wait for a load, running audio
    for (int t = 0; t < 600; t++) {
        run(e, 4);
        std::string s = display(e, kInstrument);
        if (s != "Scanning..." && s != "Loading..." && s != not_this) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return display(e, kInstrument);
}

static AEffect *open(void *lib) {
    auto entry = (AEffect * (*)(audioMasterCallback)) dlsym(lib, "VSTPluginMain");
    AEffect *e = entry ? entry(host) : nullptr;
    if (!e) return nullptr;
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, 44100.0f);
    e->dispatcher(e, effSetBlockSize, 0, N, nullptr, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);
    return e;
}

static double play_note(AEffect *e, int note) {
    midi(e, {{0x90, (unsigned char)note, 100}});
    double peak = run(e, 150);
    midi(e, {{0x80, (unsigned char)note, 0}});
    run(e, 150);
    return peak;
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <SFZ-MPC.so>\n", argv[0]); return 2; }
    std::printf("SFZ MPC offline test: %s\n", argv[1]);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::printf("  FAIL  dlopen: %s\n", dlerror()); return 1; }
    AEffect *e = open(lib);
    CHECK(e && e->magic == VST_MAGIC, "loads, AEffect magic 'VstP'");
    if (!e) return 1;
    CHECK(e->numInputs == 0 && e->numOutputs == 2 && (e->flags & (1 << 8)), "instrument: 0 in / 2 out, synth flag");

    std::string first = settle(e);
    std::printf("        first instrument: %s\n", first.c_str());
    CHECK(first.rfind("Error", 0) != 0 && first != "No instruments" && first != "-", "an instrument loaded (%s)", first.c_str());

    // walk the whole list with the > arrow, playing each instrument
    std::set<std::string> seen;
    std::string cur = first;
    for (int i = 0; i < 20 && !seen.count(cur); i++) {
        seen.insert(cur);
        double peak = play_note(e, 60);
        CHECK(peak > 1e-3 && peak < 20, "%-24s plays middle C (peak %.3f)", cur.c_str(), peak);
        e->setParameter(e, kNext, 1.0f);
        cur = settle(e, cur);
    }
    std::printf("        instruments listed: %zu\n", seen.size());
    for (const auto &s : seen) CHECK(s.find("map") == std::string::npos, "not an included piece: %s", s.c_str());

    // a chord, extreme settings, pitch bend and CCs stay finite
    midi(e, {{0x90, 48, 127}, {0x90, 55, 110}, {0x90, 60, 90}, {0x90, 64, 60}, {0xe0, 0x7f, 0x7f}, {0xb0, 1, 127}, {0xb0, 111, 100}});
    CHECK(run(e, 200) >= 0, "chord + pitch bend + CCs: output finite");
    e->setParameter(e, kVolume, 1.0f);
    CHECK(run(e, 100) >= 0, "volume at +12 dB: output finite");
    e->setParameter(e, kVolume, 2.0f / 3.0f);
    midi(e, {{0xb0, 123, 0}});   // all notes off

    // stuck notes: a zero-length note (its on and off at the same moment) must not hang, and "all notes off" (CC123)
    // must release a held note
    {
        VstMidiEvent ev2[2] = {};
        VstEvents l2 = {};
        for (int i = 0; i < 2; i++) { ev2[i].type = 1; ev2[i].byteSize = sizeof(VstMidiEvent); ev2[i].deltaFrames = 10; l2.events[i] = &ev2[i]; }
        ev2[0].midiData[0] = 0x90; ev2[0].midiData[1] = 62; ev2[0].midiData[2] = 100;
        ev2[1].midiData[0] = 0x80; ev2[1].midiData[1] = 62; ev2[1].midiData[2] = 0;
        l2.numEvents = 2;
        midi(e, {{0xb0, 120, 0}});
        run(e, 400);
        for (int k = 0; k < 20; k++) { e->dispatcher(e, effProcessEvents, 0, 0, &l2, 0); run(e, 1); }
        run(e, 1200);   // let the release fade
        const double hung = run(e, 50);
        CHECK(hung < 1e-3, "zero-length notes don't hang (level after release %.5f)", hung);
        midi(e, {{0x90, 64, 100}});
        run(e, 100);
        midi(e, {{0xb0, 123, 0}});
        run(e, 1200);
        const double held = run(e, 50);
        CHECK(held < 1e-3, "all notes off (CC123) releases a held note (level %.5f)", held);
    }

    // save, then restore into a new instance: the same instrument by name
    e->setParameter(e, kQuality, 1.0f);
    void *data = nullptr;
    intptr_t len = e->dispatcher(e, effGetChunk, 0, 0, &data, 0);
    std::string saved((const char *)data, len > 0 ? len : 0);
    CHECK(len > 0, "project state saved (%d bytes)", (int)len);
    const std::string want = display(e, kInstrument);
    AEffect *f = open(lib);
    settle(f);
    f->dispatcher(f, effSetChunk, 0, (intptr_t)saved.size(), (void *)saved.data(), 0);
    std::string got;
    for (int t = 0; t < 50 && (got = settle(f)) != want; t++) {}
    CHECK(got == want, "restored instrument: %s", got.c_str());
    CHECK(display(f, kQuality) == "Best", "restored quality: %s", display(f, kQuality).c_str());
    f->dispatcher(f, effClose, 0, 0, nullptr, 0);

    // CPU: a held 4-note chord
    e->setParameter(e, kQuality, 0.0f);
    midi(e, {{0x90, 48, 100}, {0x90, 55, 100}, {0x90, 60, 100}, {0x90, 64, 100}});
    run(e, 20);
    const int blocks = 3000;
    auto t0 = std::chrono::steady_clock::now();
    run(e, blocks);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / blocks;
    std::printf("CPU (%s, 4-note chord, %s voices): %.4f ms per 128-frame block = %.1f%% of one core in real time\n",
                display(e, kInstrument).c_str(), display(e, kVoices).c_str(), ms, 100 * ms / (N / 44.1));
    e->dispatcher(e, effClose, 0, 0, nullptr, 0);
    std::printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
