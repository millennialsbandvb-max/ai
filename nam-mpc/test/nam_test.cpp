/* Offline host for NAM MPC: loads the plugin like MPC does and checks it, then measures its CPU cost.
 *   nam-test <NAM-MPC.so> [model name]      (exit status 0 = PASSED)
 * Off the device, point it at a test folder with NAM_MPC_DIR=<dir> (holding Models/ and IRs/). On the device it
 * reads the same folders as the plugin (/sdcard/vst/NAM, /sdcard/NAM), so it can check a model's CPU cost before
 * you load it in a project: over SSH, `./nam-test /sdcard/vst/NAM-MPC.so "<model name>"`. */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>

#include "../src/vst2.h"

enum { kInput, kGate, kBass, kMiddle, kTreble, kOutput, kModel, kModelPrev, kModelNext, kCab, kIr, kIrPrev, kIrNext,
       kNormalize, kSize, kNumParams };

static int failures = 0, automates = 0, updates = 0;
#define CHECK(c, ...) do { if (c) std::printf("  ok    " __VA_ARGS__); else { std::printf("  FAIL  " __VA_ARGS__); failures++; } std::printf("\n"); } while (0)

static intptr_t host(AEffect *, int32_t op, int32_t, intptr_t, void *, float) {
    if (op == audioMasterAutomate) automates++;
    if (op == audioMasterUpdateDisplay) updates++;
    return 0;
}

static std::string display(AEffect *e, int i) {
    char buf[64] = {0};
    e->dispatcher(e, effGetParamDisplay, i, 0, buf, 0);
    return buf;
}

static const int kBlock = 128;
static const double kRate = 44100;

struct Audio {
    std::vector<float> il, ir, ol, orr;
    double phase = 0;
    Audio() : il(kBlock), ir(kBlock), ol(kBlock), orr(kBlock) {}
    double run(AEffect *e, int blocks, float amp = 0.3f) {   // -> output RMS; -1 if anything wasn't finite
        double sum = 0;
        bool finite = true;
        for (int b = 0; b < blocks; b++) {
            for (int i = 0; i < kBlock; i++) {
                il[i] = ir[i] = amp * (float)std::sin(phase);
                phase += 2 * M_PI * 110 / kRate;
            }
            float *in[2] = {il.data(), ir.data()}, *out[2] = {ol.data(), orr.data()};
            e->processReplacing(e, in, out, kBlock);
            for (int i = 0; i < kBlock; i++) {
                finite &= std::isfinite(ol[i]) && std::isfinite(orr[i]);
                sum += ol[i] * ol[i];
            }
        }
        return finite ? std::sqrt(sum / (blocks * kBlock)) : -1;
    }
};

/* Wait (running audio, as MPC would) until parameter i shows something other than a busy status. */
static std::string settle(AEffect *e, Audio &a, int i, const std::string &not_this = "") {
    for (int t = 0; t < 400; t++) {
        a.run(e, 4);
        std::string s = display(e, i);
        if (s != "Scanning..." && s != "Loading..." && s != not_this) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return display(e, i);
}

static AEffect *open(void *lib) {
    auto entry = (AEffect * (*)(audioMasterCallback)) dlsym(lib, "VSTPluginMain");
    AEffect *e = entry ? entry(host) : nullptr;
    if (!e) return nullptr;
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, (float)kRate);
    e->dispatcher(e, effSetBlockSize, 0, kBlock, nullptr, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);
    return e;
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <NAM-MPC.so> [model name]\n", argv[0]); return 2; }
    std::printf("NAM MPC offline test: %s\n", argv[1]);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::printf("  FAIL  dlopen: %s\n", dlerror()); return 1; }
    CHECK(dlsym(lib, "VSTPluginMain"), "exports VSTPluginMain");
    AEffect *e = open(lib);
    CHECK(e && e->magic == VST_MAGIC, "AEffect magic 'VstP'");
    if (!e) return 1;
    CHECK(e->numInputs == 2 && e->numOutputs == 2 && e->numParams == kNumParams, "2 in / 2 out, %d params", e->numParams);
    CHECK(e->dispatcher(e, effGetPlugCategory, 0, 0, nullptr, 0) == kPlugCategEffect, "category: effect");
    CHECK((e->flags & effFlagsProgramChunks) != 0, "saves state as a chunk");
    for (int i = 0; i < e->numParams; i++) {
        char name[64] = {0};
        e->dispatcher(e, effGetParamName, i, 0, name, 0);
        std::printf("        param %2d %-10s = %s\n", i, name, display(e, i).c_str());
    }

    Audio a;
    std::string model = settle(e, a, kModel), ir = settle(e, a, kIr);
    std::printf("        model: %s, IR: %s\n", model.c_str(), ir.c_str());
    const bool have_model = model != "No models" && model.rfind("Error", 0) != 0 && model != "-";
    CHECK(have_model, "a model loaded (%s)", model.c_str());
    CHECK(model.rfind("._", 0) != 0 && model != "hidden", "hidden files (macOS ._ twins, dot-folders) are skipped");
    const double rms = a.run(e, 200);
    CHECK(rms > 1e-4 && rms < 10, "audio through the model: rms %.4f", rms);

    // parameters: round trip and text
    e->setParameter(e, kInput, 0.75f);
    CHECK(std::fabs(e->getParameter(e, kInput) - 0.75f) < 1e-6, "input round trip (%s dB)", display(e, kInput).c_str());
    e->setParameter(e, kGate, 0.0f);
    CHECK(display(e, kGate) == "Off", "gate at minimum shows Off");
    e->setParameter(e, kGate, 0.5f);
    a.run(e, 300, 1e-4f);   // let it close: detector, 50 ms hold, then a 60 ms fade
    const double quiet = a.run(e, 50, 1e-4f);
    CHECK(quiet >= 0 && quiet < 1e-5, "gate closes on a quiet input (rms %.2g)", quiet);
    e->setParameter(e, kGate, 0.0f);
    e->setParameter(e, kBass, 1.0f);
    e->setParameter(e, kTreble, 0.0f);
    CHECK(a.run(e, 50) > 0, "tone stack at extremes stays finite");
    e->setParameter(e, kInput, 1.0f);
    CHECK(a.run(e, 50, 1.0f) > 0, "+24 dB input into the model stays finite");
    e->setParameter(e, kInput, 0.5f);

    // arrows: step to the next model, report the arrow back to 0
    automates = 0;
    e->setParameter(e, kModelNext, 1.0f);
    std::string next = settle(e, a, kModel, model);
    CHECK(automates > 0, "arrow reported back to 0 (%d automate calls)", automates);
    std::printf("        next model: %s\n", next.c_str());
    CHECK(updates > 0, "host told to refresh names (%d)", updates);
    e->setParameter(e, kModelPrev, 1.0f);
    CHECK(settle(e, a, kModel, next) == model || next == model, "previous model is back");

    // project save and restore into a fresh instance
    e->setParameter(e, kBass, 0.2f);
    e->setParameter(e, kCab, 0.0f);
    void *data = nullptr;
    intptr_t len = e->dispatcher(e, effGetChunk, 0, 0, &data, 0);
    CHECK(len > 0 && data, "chunk saved (%d bytes)", (int)len);
    std::string saved((const char *)data, len > 0 ? len : 0);
    AEffect *f = open(lib);
    Audio b;
    settle(f, b, kModel);
    f->dispatcher(f, effSetChunk, 0, (intptr_t)saved.size(), (void *)saved.data(), 0);
    std::string restored;
    for (int t = 0; t < 100 && (restored = settle(f, b, kModel)) != display(e, kModel); t++) {}
    CHECK(restored == display(e, kModel), "restored model: %s", restored.c_str());
    CHECK(std::fabs(f->getParameter(f, kBass) - 0.2f) < 1e-5 && f->getParameter(f, kCab) < 0.5f, "restored parameters");
    f->dispatcher(f, effClose, 0, 0, nullptr, 0);

    // CPU: time the chosen (or current) model at MPC's 128-frame block
    if (argc > 2) {
        e->setParameter(e, kModelNext, 1.0f);   // make sure the list is fresh, then pick by name via a chunk
        settle(e, a, kModel);
        std::string st = "NAMMPC 1\nmodel_file=" + std::string(argv[2]) + "\ncab=1\n";
        e->dispatcher(e, effSetChunk, 0, (intptr_t)st.size() + 1, (void *)st.c_str(), 0);
        std::string got;
        for (int t = 0; t < 100 && (got = settle(e, a, kModel)) != argv[2] && got.rfind("Missing", 0) != 0; t++) {}
        CHECK(got == argv[2], "benchmark model: %s", got.c_str());
    }
    e->setParameter(e, kCab, 1.0f);
    a.run(e, 50);
    const int blocks = 3000;   // ~8.7 s of audio
    auto t0 = std::chrono::steady_clock::now();
    a.run(e, blocks);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double per_block_ms = secs * 1000 / blocks, budget_ms = kBlock / kRate * 1000;
    std::printf("CPU: %s (+ cab %s): %.3f ms per 128-frame block = %.1f%% of one core in real time\n",
                display(e, kModel).c_str(), display(e, kIr).c_str(), per_block_ms, 100 * per_block_ms / budget_ms);
    if (argc > 2)
        std::printf("     under 35%% is comfortable on an MPC; over 60%% will likely crackle alongside a project\n");

    e->dispatcher(e, effClose, 0, 0, nullptr, 0);
    std::printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
