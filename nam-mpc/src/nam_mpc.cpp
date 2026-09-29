/* NAM MPC: Neural Amp Modeler as a VST2 effect for MPC OS standalone devices (MPC Live II and other Gen1 units).
 *
 * Signal chain (mono in, the left input; the result goes to both outputs):
 *   input gain -> [NAM model, resampled to the model's rate when needed] -> loudness normalize -> noise gate
 *   -> bass/middle/treble -> DC block -> cab IR -> output gain
 *
 * Models (.nam) are read from <base>/Models and cab IRs (.wav) from <base>/IRs, where <base> is the NAM folder next
 * to the plugin (/sdcard/vst/NAM), /sdcard/NAM, or a NAM folder at the top of any mounted drive (/media/<drive>/NAM:
 * a USB stick or SD card). Sub-folders are scanned one level deep.
 *
 * Threads: MPC calls processReplacing on its audio thread and everything else from its UI thread (automation may
 * also arrive on the audio thread). Neither ever loads a file or allocates: they post requests to a per-instance
 * worker thread, which scans folders, loads and prewarms models and IRs, and hands them to the audio thread through
 * lock-free slots. The audio thread hands the old one back, and the worker frees it.
 *
 * Parameter indices are what the skin and saved projects bind to: append only, never reorder (see params.json). */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>

#include "NAM/activations.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"
#include "NAM/wav.h"
// The resampler (from the NAM plugin, via iPlug2) expects these two from iPlug2 itself.
namespace iplug { static constexpr double PI = 3.14159265358979323846; }
#define DEFAULT_BLOCK_SIZE 512
#define WDL_ALLOW_UNSIGNED_DEFAULT_CHAR 1   // char is unsigned on ARM; the resampler doesn't care
#include "ResamplingContainer/ResamplingContainer.h"

#include "vst2.h"

namespace fs = std::filesystem;

#define PLUG_NAME "NAM MPC"
#define PLUG_VENDOR "NAM-MPC"
#define PLUG_UID 0x4e414d50 /* 'NAMP' */
#define PLUG_VERSION 1000

static constexpr int kChunk = 512;       // audio is processed in pieces of at most this many frames
static constexpr int kModelMaxBlock = 1024;  // >= kChunk resampled up to the model's rate
static constexpr int kMaxIrTaps = 1024;  // ~23 ms at 44.1 kHz: plenty for a guitar cab, cheap enough for the CPU
static constexpr double kTargetLoudnessDb = -18.0;

/* ---- parameters ------------------------------------------------------------------------------------------ */
enum {
    kInput, kGate, kBass, kMiddle, kTreble, kOutput, kModel, kModelPrev, kModelNext,
    kCab, kIr, kIrPrev, kIrNext, kNormalize, kSize, kNumParams
};
struct ParamInfo { const char *key, *name, *unit; float min, max, def; };
static const ParamInfo PARAMS[kNumParams] = {
    {"input", "Input", "dB", -24, 24, 0},
    {"gate", "Gate", "dB", -100, -20, -100},
    {"bass", "Bass", "", 0, 10, 5},
    {"middle", "Middle", "", 0, 10, 5},
    {"treble", "Treble", "", 0, 10, 5},
    {"output", "Output", "dB", -24, 24, 0},
    {"model", "Model", "", 0, 1, 0},
    {"model_prev", "Model <", "", 0, 1, 0},
    {"model_next", "Model >", "", 0, 1, 0},
    {"cab", "Cab", "", 0, 1, 1},
    {"ir", "IR", "", 0, 1, 0},
    {"ir_prev", "IR <", "", 0, 1, 0},
    {"ir_next", "IR >", "", 0, 1, 0},
    {"normalize", "Normalize", "", 0, 1, 1},
    {"size", "Size", "%", 0, 100, 100},
};

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float db_to_gain(float db) { return std::pow(10.0f, db / 20.0f); }
static void copy_str(void *dst, const char *src, size_t max) {
    std::strncpy((char *)dst, src, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static bool is_finite_bits(float v) {   // std::isfinite is unreliable under -ffast-math
    uint32_t u;
    std::memcpy(&u, &v, 4);
    return (u & 0x7f800000u) != 0x7f800000u;
}

/* ARMv7's VFP keeps denormals by default and they are very slow; flush them while we run. */
struct DenormalGuard {
#if defined(__arm__)
    uint32_t saved;
    DenormalGuard() {
        __asm__ volatile("vmrs %0, fpscr" : "=r"(saved));
        uint32_t v = saved | (1u << 24);
        __asm__ volatile("vmsr fpscr, %0" : : "r"(v));
    }
    ~DenormalGuard() { __asm__ volatile("vmsr fpscr, %0" : : "r"(saved)); }
#endif
};

/* ---- DSP pieces ------------------------------------------------------------------------------------------ */
struct Biquad {   // RBJ cookbook, transposed direct form II
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    enum Kind { LowShelf, Peak, HighShelf };
    void set(Kind k, double sr, double f, double q, double db) {
        const double A = std::pow(10.0, db / 40.0), w = 2 * M_PI * f / sr, c = std::cos(w), s = std::sin(w);
        const double al = s / (2 * q);
        double B0, B1, B2, A0, A1, A2;
        if (k == Peak) {
            B0 = 1 + al * A; B1 = -2 * c; B2 = 1 - al * A; A0 = 1 + al / A; A1 = -2 * c; A2 = 1 - al / A;
        } else {
            const double sq = 2 * std::sqrt(A) * al, sg = k == LowShelf ? 1 : -1;
            B0 = A * ((A + 1) - sg * (A - 1) * c + sq);
            B1 = sg * 2 * A * ((A - 1) - sg * (A + 1) * c);
            B2 = A * ((A + 1) - sg * (A - 1) * c - sq);
            A0 = (A + 1) + sg * (A - 1) * c + sq;
            A1 = -sg * 2 * ((A - 1) + sg * (A + 1) * c);
            A2 = (A + 1) + sg * (A - 1) * c - sq;
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    float run(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void clear() { z1 = z2 = 0; }
};

using Resampler = dsp::ResamplingContainer<float, 1, 12>;

/* A loaded model, ready to run at the host rate. Built and freed on the worker thread only. */
struct Model {
    std::unique_ptr<nam::DSP> dsp;
    std::unique_ptr<Resampler> rs;   // when the model can't run at the host rate itself
    nam::SlimmableModel *slim = nullptr;
    float loudness_gain = 1;         // brings the model to kTargetLoudnessDb when Normalize is on
    std::string name;
};

/* A cab IR. The FIR history lives here too, so swapping cabs needs no allocation on the audio thread. */
struct Cab {
    std::vector<float> h;      // taps, h[k] multiplies x[n-k]; length a multiple of 4
    std::vector<float> hist;   // 2 * taps, mirrored ring buffer (see run())
    int pos = 0;
    std::string name;
    float run(float x) {
        const int n = (int)h.size();
        pos = pos == 0 ? n - 1 : pos - 1;
        hist[pos] = hist[pos + n] = x;
        const float *p = hist.data() + pos, *k = h.data();
        float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        for (int i = 0; i < n; i += 4) {
            a0 += k[i] * p[i]; a1 += k[i + 1] * p[i + 1]; a2 += k[i + 2] * p[i + 2]; a3 += k[i + 3] * p[i + 3];
        }
        return (a0 + a1) + (a2 + a3);
    }
};

/* One-slot, lock-free handover from the worker to the audio thread.
 *   worker: offer(new)  audio: take() at block start  worker: collect() frees what the audio thread let go of */
template <class T> struct Handover {
    std::atomic<T *> pending{nullptr}, retired{nullptr}, live{nullptr};   // live: written by the audio thread only
    void offer(T *t) { delete pending.exchange(t); }   // a newer offer replaces one the audio thread never took
    void take() {
        if (!pending.load(std::memory_order_acquire) || retired.load(std::memory_order_acquire)) return;
        T *old = live.load(std::memory_order_relaxed);
        live.store(pending.exchange(nullptr), std::memory_order_release);
        retired.store(old, std::memory_order_release);
    }
    void collect() { delete retired.exchange(nullptr); }
    void destroy() { delete pending.exchange(nullptr); delete retired.exchange(nullptr); delete live.exchange(nullptr); }
};

/* ---- the plugin instance ------------------------------------------------------------------------------ */
struct Library {   // a scanned folder of models or IRs
    std::vector<fs::path> files;
    int current = -1;      // index into files of what's loaded (or loading)
    std::string status;    // shown when nothing is loaded: "No models", "Loading...", an error
};

struct Plugin {
    AEffect fx{};
    audioMasterCallback master = nullptr;
    std::atomic<float> norm[kNumParams];

    // worker requests (any thread -> worker)
    std::mutex req_mutex;
    std::condition_variable req_cv;
    bool quit = false;
    int model_step = 0, ir_step = 0;       // +-1 per arrow press
    int model_pick = -1, ir_pick = -1;     // an absolute index
    std::string model_by_name, ir_by_name; // from a saved project
    bool rescan = true, reload = false, resize = false;

    // worker state; the UI thread reads names/status under lib_mutex
    std::mutex lib_mutex;
    Library models, irs;
    std::atomic<int> model_count{0}, model_index{0}, ir_count{0}, ir_index{0};
    std::atomic<double> sample_rate{44100.0};
    std::thread worker;

    // audio thread
    Handover<Model> model;
    Handover<Cab> cab;
    Biquad bass, mid, treble;
    float tone_set[3] = {-1, -1, -1};
    double tone_rate = 0;
    float in_gain = 1, out_gain = 1, gate_gain = 0, gate_power = 0, dc_x = 0, dc_y = 0;
    int gate_hold = 0;
    bool gate_open = false;
    std::atomic<bool> release[kNumParams];
    std::atomic<bool> update_display{false};
    float x[kChunk], y[kChunk], g[kChunk];

    std::string chunk;   // effGetChunk's buffer; must outlive the call
    std::vector<fs::path> bases;

    float value(int i) const { const ParamInfo &p = PARAMS[i]; return p.min + (p.max - p.min) * norm[i].load(); }
    template <class F> void post(F f) {   // a template, not std::function: never allocates (may run on the audio thread)
        { std::lock_guard<std::mutex> l(req_mutex); f(); }
        req_cv.notify_one();
    }
    void run_worker();
    void process(float **in, float **out, int n);
    void process_chunk(const float *in, float *l, float *r, int n);
};

/* ---- worker thread ---------------------------------------------------------------------------------- */
static std::string lower(std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; }

/* The fixed folders plus <drive>/NAM on every mounted drive (a USB stick or SD card), looked up at each scan. */
static std::vector<fs::path> all_bases(const std::vector<fs::path> &fixed) {
    std::vector<fs::path> bases = fixed;
    std::error_code ec;
    for (fs::directory_iterator it("/media", ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (fs::is_directory(it->path() / "NAM", e2)) bases.push_back(it->path() / "NAM");
    }
    return bases;
}

static std::vector<fs::path> scan(const std::vector<fs::path> &fixed, const char *sub, const char *ext) {
    std::vector<fs::path> out;
    for (const auto &b : all_bases(fixed)) {
        std::error_code ec;
        fs::path dir = b / sub;
        if (!fs::is_directory(dir, ec)) continue;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it.depth() > 1) { it.disable_recursion_pending(); continue; }
            // hidden files and folders: macOS writes a "._<name>" twin of every file it copies to a FAT/exFAT
            // card or stick; those twins aren't models (they fail to parse) and would fill the list
            if (it->path().filename().string().rfind('.', 0) == 0) {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec) && lower(it->path().extension().string()) == ext) out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end(), [](const fs::path &a, const fs::path &b) {
        return lower(a.stem().string()) < lower(b.stem().string());
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static Model *load_model(const fs::path &path, double sr, float size) {
    auto m = std::make_unique<Model>();
    nam::DspLoadOptions opt;
    opt.prewarm = false;
    m->dsp = nam::get_dsp(path, opt);
    if (!m->dsp) throw std::runtime_error("unsupported model");
    if (m->dsp->NumInputChannels() != 1 || m->dsp->NumOutputChannels() != 1)
        throw std::runtime_error("model isn't mono");
    double model_sr = m->dsp->GetExpectedSampleRate();
    if (model_sr <= 0 || std::fabs(model_sr - sr) < 0.5 || m->dsp->SupportsArbitrarySampleRate()) model_sr = sr;
    if (model_sr != sr) {
        m->rs = std::make_unique<Resampler>(model_sr);
        m->rs->Reset(sr, kChunk);
    }
    m->dsp->ResetAndPrewarm(model_sr, kModelMaxBlock);
    m->slim = dynamic_cast<nam::SlimmableModel *>(m->dsp.get());
    if (m->slim) m->slim->SetSlimmableSize(size);
    if (m->dsp->HasLoudness()) m->loudness_gain = db_to_gain((float)(kTargetLoudnessDb - m->dsp->GetLoudness()));
    m->name = path.stem().string();
    return m.release();
}

static Cab *load_cab(const fs::path &path, double sr) {
    double ir_sr = 0;
    int channels = 0;
    std::vector<float> w = nam::detail::load_wav_ir(path, ir_sr, channels);
    if (channels < 1 || w.empty()) throw std::runtime_error("empty IR");
    const size_t frames = w.size() / channels;   // channel-major: use the first channel
    if (ir_sr <= 0) ir_sr = sr;
    const double step = ir_sr / sr;              // resample to the host rate (linear; fine for a cab)
    size_t taps = std::min<size_t>((size_t)std::ceil(frames / step), kMaxIrTaps);
    auto c = std::make_unique<Cab>();
    c->h.assign((taps + 3) & ~size_t(3), 0.0f);
    const float gain = (float)(std::pow(10.0, -18 * 0.05) * 48000.0 / sr);   // NAM's own IR gain law
    for (size_t i = 0; i < taps; i++) {
        const double t = i * step;
        const size_t j = (size_t)t;
        const float f = (float)(t - j), a = w[j], b = j + 1 < frames ? w[j + 1] : 0.0f;
        c->h[i] = gain * (a + (b - a) * f);
    }
    const size_t fade = std::min<size_t>(64, taps);   // a truncated IR ends with a short fade, not a click
    if (frames / step > kMaxIrTaps)
        for (size_t i = 0; i < fade; i++) c->h[taps - 1 - i] *= (float)i / fade;
    c->hist.assign(c->h.size() * 2, 0.0f);
    c->name = path.stem().string();
    return c.release();
}

void Plugin::run_worker() {
    std::unique_lock<std::mutex> lk(req_mutex);
    while (!quit) {
        req_cv.wait_for(lk, std::chrono::milliseconds(100));
        if (quit) break;
        lk.unlock();   // freeing a model can take a while: never with req_mutex held
        model.collect();
        cab.collect();
        lk.lock();
        bool do_scan = rescan, do_reload = reload, do_resize = resize;
        int mstep = model_step, istep = ir_step, mpick = model_pick, ipick = ir_pick;
        std::string mname = model_by_name, iname = ir_by_name;
        rescan = reload = resize = false;
        model_step = ir_step = 0;
        model_pick = ir_pick = -1;
        model_by_name.clear();
        ir_by_name.clear();
        if (!do_scan && !do_reload && !do_resize && !mstep && !istep && mpick < 0 && ipick < 0 && mname.empty() && iname.empty())
            continue;
        lk.unlock();

        const double sr = sample_rate.load();
        if (do_scan || mstep || istep) {   // pick up files copied in since the last look
            auto mf = scan(bases, "Models", ".nam"), inf = scan(bases, "IRs", ".wav");
            std::lock_guard<std::mutex> l(lib_mutex);
            auto keep = [](Library &lib, std::vector<fs::path> files) {
                fs::path cur = lib.current >= 0 && lib.current < (int)lib.files.size() ? lib.files[lib.current] : fs::path();
                lib.files = std::move(files);
                auto it = std::find(lib.files.begin(), lib.files.end(), cur);
                lib.current = it == lib.files.end() ? -1 : (int)(it - lib.files.begin());
            };
            keep(models, mf);
            keep(irs, inf);
            if (models.files.empty()) models.status = "No models";
            if (irs.files.empty()) irs.status = "No IRs";
        }
        // resolve what to load next
        auto choose = [&](Library &lib, int step, int pick, const std::string &name, bool first) -> int {
            std::lock_guard<std::mutex> l(lib_mutex);
            const int n = (int)lib.files.size();
            if (!n) return -1;
            int target = -2;   // -2: no change
            if (!name.empty()) {
                for (int i = 0; i < n; i++)
                    if (lib.files[i].stem().string() == name) target = i;
                if (target == -2) { lib.status = "Missing: " + name; return -1; }
            } else if (pick >= 0) target = std::min(pick, n - 1);
            else if (step) target = lib.current < 0 ? 0 : ((lib.current + step) % n + n) % n;
            else if (first && lib.current < 0) target = 0;
            if (target == -2 || (target == lib.current && !do_reload)) return do_reload ? lib.current : -1;
            lib.current = target;
            lib.status = "Loading...";
            return target;
        };
        const int mi = choose(models, mstep, mpick, mname, do_scan);
        const int ii = choose(irs, istep, ipick, iname, do_scan);
        update_display = true;
        auto file_of = [&](Library &lib, int i) { std::lock_guard<std::mutex> l(lib_mutex); return lib.files[i]; };
        if (mi >= 0) {
            fs::path p = file_of(models, mi);
            try {
                model.offer(load_model(p, sr, value(kSize) / 100.0f));
                std::lock_guard<std::mutex> l(lib_mutex);
                models.status.clear();
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> l(lib_mutex);
                models.status = std::string("Error: ") + e.what();
            }
        }
        if (ii >= 0) {
            fs::path p = file_of(irs, ii);
            try {
                cab.offer(load_cab(p, sr));
                std::lock_guard<std::mutex> l(lib_mutex);
                irs.status.clear();
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> l(lib_mutex);
                irs.status = std::string("Error: ") + e.what();
            }
        }
        if (do_resize) {   // the audio thread may be running the model: SetSlimmableSize is made for that
            Model *m = model.pending.load();
            if (!m) m = model.live.load(std::memory_order_acquire);   // only this thread ever frees models
            if (m && m->slim) m->slim->SetSlimmableSize(value(kSize) / 100.0f);
        }
        {
            std::lock_guard<std::mutex> l(lib_mutex);
            model_count = (int)models.files.size();
            model_index = std::max(models.current, 0);
            ir_count = (int)irs.files.size();
            ir_index = std::max(irs.current, 0);
        }
        update_display = true;
        lk.lock();
    }
}

/* ---- audio ---------------------------------------------------------------------------------------- */
void Plugin::process(float **in, float **out, int n) {
    DenormalGuard dg;
    model.take();
    cab.take();
    if (master) {   // report momentary arrows back to 0, and new names, outside the host's own call into us
        for (int i = 0; i < kNumParams; i++)
            if (release[i].exchange(false)) master(&fx, audioMasterAutomate, i, 0, nullptr, 0.0f);
        if (update_display.exchange(false)) master(&fx, audioMasterUpdateDisplay, 0, 0, nullptr, 0.0f);
    }
    for (int off = 0; off < n; off += kChunk) {
        const int m = std::min(kChunk, n - off);
        process_chunk(in && in[0] ? in[0] + off : nullptr, out[0] + off, out[1] ? out[1] + off : nullptr, m);
    }
}

void Plugin::process_chunk(const float *in, float *l, float *r, int n) {
    const double sr = sample_rate.load();
    const float smooth = (float)(1.0 - std::exp(-1.0 / (0.02 * sr)));   // 20 ms gain glide
    const float t_in = db_to_gain(value(kInput)), t_out = db_to_gain(value(kOutput));
    for (int i = 0; i < n; i++) {
        in_gain += smooth * (t_in - in_gain);
        x[i] = (in ? in[i] : 0.0f) * in_gain;
    }

    // noise gate: detect on the input, apply after the amp (so the amp's own hiss is gated too)
    const float thr_db = value(kGate);
    const bool gate_on = thr_db > -99.5f;
    const float thr = std::pow(10.0f, thr_db / 10.0f), thr_close = thr * 0.25f;   // 6 dB hysteresis
    const float det = (float)(1.0 - std::exp(-1.0 / (0.005 * sr)));
    const float att = (float)(1.0 - std::exp(-1.0 / (0.0005 * sr))), rel = (float)(1.0 - std::exp(-1.0 / (0.06 * sr)));
    const int hold = (int)(0.05 * sr);
    for (int i = 0; i < n; i++) {
        gate_power += det * (x[i] * x[i] - gate_power);
        if (!gate_on || gate_power > thr) { gate_open = true; gate_hold = hold; }
        else if (gate_power < thr_close) { if (gate_hold > 0) gate_hold--; else gate_open = false; }
        const float target = gate_open ? 1.0f : 0.0f;
        gate_gain += (target > gate_gain ? att : rel) * (target - gate_gain);
        g[i] = gate_gain;
    }

    Model *m = model.live.load(std::memory_order_relaxed);
    if (m) {
        float *xp[1] = {x}, *yp[1] = {y};
        if (m->rs) {
            nam::DSP *d = m->dsp.get();
            m->rs->ProcessBlock(xp, yp, n, [d](float **a, float **b, int k) { d->process(a, b, k); });
        } else {
            m->dsp->process(xp, yp, n);
        }
        const float lg = norm[kNormalize].load() > 0.5f ? m->loudness_gain : 1.0f;
        for (int i = 0; i < n; i++) y[i] *= lg * g[i];
    } else {
        for (int i = 0; i < n; i++) y[i] = x[i] * g[i];
    }

    // tone stack: +-10 dB per band, 0 dB at 5
    const float tv[3] = {value(kBass), value(kMiddle), value(kTreble)};
    if (tv[0] != tone_set[0] || tv[1] != tone_set[1] || tv[2] != tone_set[2] || sr != tone_rate) {
        bass.set(Biquad::LowShelf, sr, 150, 0.707, (tv[0] - 5) * 2);
        mid.set(Biquad::Peak, sr, 650, 0.7, (tv[1] - 5) * 2);
        treble.set(Biquad::HighShelf, sr, 2500, 0.707, (tv[2] - 5) * 2);
        std::copy(tv, tv + 3, tone_set);
        tone_rate = sr;
    }
    const bool flat = tv[0] == 5 && tv[1] == 5 && tv[2] == 5;
    Cab *c = norm[kCab].load() > 0.5f ? cab.live.load(std::memory_order_relaxed) : nullptr;
    for (int i = 0; i < n; i++) {
        float v = y[i];
        if (!flat) v = treble.run(mid.run(bass.run(v)));
        const float dc = v - dc_x + 0.9995f * dc_y;   // ~3.5 Hz DC blocker
        dc_x = v;
        dc_y = dc;
        v = c ? c->run(dc) : dc;
        out_gain += smooth * (t_out - out_gain);
        v *= out_gain;
        if (!is_finite_bits(v)) { v = 0; dc_x = dc_y = 0; bass.clear(); mid.clear(); treble.clear(); }
        l[i] = v;
        if (r) r[i] = v;
    }
}

/* ---- VST2 entry points ---------------------------------------------------------------------------- */
static Plugin *P(AEffect *e) { return (Plugin *)e->object; }

static int library_count(Plugin *s, int i) { return i == kModel ? s->model_count.load() : s->ir_count.load(); }
static int library_index(Plugin *s, int i) { return i == kModel ? s->model_index.load() : s->ir_index.load(); }

static float getParameter(AEffect *e, int32_t i) {
    Plugin *s = P(e);
    if (i < 0 || i >= kNumParams) return 0;
    if (i == kModel || i == kIr) {
        const int n = library_count(s, i);
        return n > 1 ? (float)library_index(s, i) / (n - 1) : 0.0f;
    }
    return s->norm[i].load();
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = P(e);
    if (i < 0 || i >= kNumParams) return;
    v = clamp01(v);
    switch (i) {
    case kModel: case kIr: {
        // A value on an entry (a touch, automation) selects it; one in between is a Q-Link nudge: step one entry.
        const int n = library_count(s, i);
        if (n < 1) { s->post([&] { s->rescan = true; }); return; }
        const float pos = v * (n - 1), cur = (float)library_index(s, i);
        int step = 0, pick = -1;
        if (std::fabs(pos - std::round(pos)) > 0.001f) step = pos > cur ? 1 : -1;
        else pick = (int)std::lround(pos);
        if (pick == (int)cur) return;
        s->post([&] {
            if (i == kModel) { if (step) s->model_step += step; else s->model_pick = pick; }
            else { if (step) s->ir_step += step; else s->ir_pick = pick; }
        });
        return;
    }
    case kModelPrev: case kModelNext: case kIrPrev: case kIrNext:
        if (v > 0.5f) {
            const int d = (i == kModelNext || i == kIrNext) ? 1 : -1;
            s->post([&] { if (i == kModelPrev || i == kModelNext) s->model_step += d; else s->ir_step += d; });
            s->release[i] = true;
        }
        s->norm[i] = 0;
        return;
    case kCab: case kNormalize:
        s->norm[i] = v > 0.5f ? 1.0f : 0.0f;
        s->update_display = true;
        return;
    case kSize:
        s->norm[i] = v;
        s->post([&] { s->resize = true; });
        return;
    default:
        s->norm[i] = v;
    }
}

static void param_display(Plugin *s, int i, char *out) {
    const int n = 24;
    switch (i) {
    case kGate:
        if (s->value(i) <= -99.5f) copy_str(out, "Off", n);
        else std::snprintf(out, n, "%.0f", s->value(i));
        return;
    case kModel: case kIr: {
        std::lock_guard<std::mutex> l(s->lib_mutex);
        Library &lib = i == kModel ? s->models : s->irs;
        if (!lib.status.empty() || lib.current < 0 || lib.current >= (int)lib.files.size())
            copy_str(out, lib.status.empty() ? "-" : lib.status.c_str(), n);
        else copy_str(out, lib.files[lib.current].stem().string().c_str(), n);
        return;
    }
    case kModelPrev: case kIrPrev: copy_str(out, "<", n); return;
    case kModelNext: case kIrNext: copy_str(out, ">", n); return;
    case kCab: case kNormalize: copy_str(out, s->norm[i].load() > 0.5f ? "On" : "Off", n); return;
    case kSize: std::snprintf(out, n, "%.0f", s->value(i)); return;
    default: std::snprintf(out, n, "%.1f", s->value(i));
    }
}

/* Project state: every parameter by key plus the model and IR by name, as text. */
static void get_state(Plugin *s, std::string &out) {
    out = "NAMMPC 1\n";
    char buf[64];
    for (int i = 0; i < kNumParams; i++) {
        if (i == kModel || i == kIr) continue;
        std::snprintf(buf, sizeof buf, "%s=%.6f\n", PARAMS[i].key, s->norm[i].load());
        out += buf;
    }
    std::lock_guard<std::mutex> l(s->lib_mutex);
    if (s->models.current >= 0 && s->models.current < (int)s->models.files.size())
        out += "model_file=" + s->models.files[s->models.current].stem().string() + "\n";
    if (s->irs.current >= 0 && s->irs.current < (int)s->irs.files.size())
        out += "ir_file=" + s->irs.files[s->irs.current].stem().string() + "\n";
}

static void set_state(Plugin *s, const char *data, size_t len) {
    std::string text(data, strnlen(data, len));
    if (text.rfind("NAMMPC ", 0) != 0) return;
    std::string mname, iname;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "model_file") mname = v;
        else if (k == "ir_file") iname = v;
        else
            for (int i = 0; i < kNumParams; i++)
                if (k == PARAMS[i].key && i != kModel && i != kIr) s->norm[i] = clamp01((float)std::atof(v.c_str()));
    }
    s->post([&] {
        s->model_by_name = mname;
        s->ir_by_name = iname;
        s->resize = true;
    });
    s->update_display = true;
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float opt) {
    Plugin *s = P(e);
    switch (op) {
    case effOpen: return 1;
    case effClose:
        s->post([&] { s->quit = true; });
        if (s->worker.joinable()) s->worker.join();
        s->model.destroy();
        s->cab.destroy();
        delete s;
        return 1;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: if (idx >= 0 && idx < kNumParams) copy_str(p, PARAMS[idx].unit, 8); return 1;
    case effGetParamDisplay: if (idx >= 0 && idx < kNumParams) param_display(s, idx, (char *)p); return 1;
    case effSetSampleRate:
        if (opt > 0 && std::fabs(opt - s->sample_rate.load()) > 0.5) {
            s->sample_rate = opt;
            s->post([&] { s->reload = true; });
        }
        return 1;
    case effSetBlockSize: case effMainsChanged: return 1;
    case effCanDo: return (p && !std::strcmp((const char *)p, "bypass")) ? 0 : -1;
    case effGetChunk:
        get_state(s, s->chunk);
        *(void **)p = (void *)s->chunk.c_str();
        return (intptr_t)s->chunk.size() + 1;
    case effSetChunk:
        if (v > 0 && p) set_state(s, (const char *)p, (size_t)v);
        return 1;
    default: return 0;
    }
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) { P(e)->process(in, out, n); }
static void processAccumulate(AEffect *e, float **in, float **out, int32_t n) {   // legacy process(): adds
    Plugin *s = P(e);
    float l[kChunk], r[kChunk];
    for (int off = 0; off < n; off += kChunk) {
        const int m = std::min(kChunk, n - off);
        float *o[2] = {l, r};
        float *i2[2] = {in && in[0] ? in[0] + off : nullptr, nullptr};
        s->process(i2, o, m);
        for (int k = 0; k < m; k++) { out[0][off + k] += l[k]; if (out[1]) out[1][off + k] += r[k]; }
    }
}

static int anchor;
static fs::path plugin_dir() {
    Dl_info info;
    if (dladdr((void *)&anchor, &info) && info.dli_fname) return fs::path(info.dli_fname).parent_path();
    return {};
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    static std::once_flag once;
    std::call_once(once, [] { nam::activations::Activation::enable_fast_tanh(); });
    Plugin *s = new (std::nothrow) Plugin();
    if (!s) return nullptr;
    s->master = master;
    for (int i = 0; i < kNumParams; i++) {
        const ParamInfo &p = PARAMS[i];
        s->norm[i] = p.max > p.min ? (p.def - p.min) / (p.max - p.min) : 0.0f;
        s->release[i] = false;
    }
    if (const char *env = std::getenv("NAM_MPC_DIR")) s->bases.push_back(env);   // for testing off the device
    fs::path dir = plugin_dir();
    if (!dir.empty()) s->bases.push_back(dir / "NAM");
    s->bases.push_back("/sdcard/NAM");
    s->models.status = "Scanning...";
    s->irs.status = "Scanning...";
    s->worker = std::thread([s] { s->run_worker(); });

    AEffect *e = &s->fx;
    e->magic = VST_MAGIC;
    e->dispatcher = dispatcher;
    e->process = processAccumulate;
    e->processReplacing = processReplacing;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->numPrograms = 1;
    e->numParams = kNumParams;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = s;
    return e;
}
