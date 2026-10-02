/* SFZ MPC: the sfizz SFZ sample player as a VST2 instrument for MPC OS standalone devices (MPC Live II etc.).
 *
 * Plays .sfz instruments (a saxophone, pianos, strings...) found in an SFZ folder at the top of any mounted drive
 * (/media/<drive>/SFZ: the SD card or a USB stick), next to the plugin (<plugin dir>/SFZ) or in /sdcard/SFZ.
 * Instrument folders are scanned three levels deep, since SFZ libraries often keep their .sfz files in sub-folders.
 *
 * Threads, following sfizz's own rules (sfizz.h): loading an instrument and changing the voice count may only happen
 * while that synth isn't rendering, so a worker thread builds a complete new synth and hands it to the audio thread
 * through a lock-free slot (the same pattern as NAM MPC); the audio thread frees nothing and loads nothing. Volume
 * and sample quality are real-time settings, applied by the audio thread at the start of a block.
 * MIDI arrives via effProcessEvents just before each processReplacing, and is passed to sfizz with its frame offset,
 * so timing is sample-accurate.
 *
 * Parameter indices are what the page and saved projects bind to: append only, never reorder (see params.json). */
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

#include "sfizz.h"
#include "vst2.h"

namespace fs = std::filesystem;

#define PLUG_NAME "SFZ MPC"
#define PLUG_VENDOR "SFZ-MPC"
#define PLUG_UID 0x53465a4d /* 'SFZM' */
#define PLUG_VERSION 1000

static constexpr int kChunk = 512;      // rendered in pieces of at most this many frames
static constexpr int kMaxEvents = 1024; // MIDI events kept per block

enum { effProcessEvents = 25, effFlagsIsSynth = 1 << 8, kPlugCategSynth = 2 };
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4];
                 char detune, noteOffVelocity, reserved1, reserved2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;

/* ---- parameters ------------------------------------------------------------------------------------------ */
enum { kInstrument, kPrev, kNext, kVolume, kVoices, kQuality, kNumParams };
struct ParamInfo { const char *key, *name, *unit; float min, max, def; };
static const ParamInfo PARAMS[kNumParams] = {
    {"instrument", "Instrument", "", 0, 1, 0},
    {"instrument_prev", "Instrument <", "", 0, 1, 0},
    {"instrument_next", "Instrument >", "", 0, 1, 0},
    {"volume", "Volume", "dB", -24, 12, 0},
    {"voices", "Voices", "", 0, 1, 0.4f},      // an option: VOICES[]
    {"quality", "Quality", "", 0, 1, 0.0f},    // an option: QUALITY[]
};
static const int VOICES[] = {8, 16, 24, 32, 48, 64};
static const char *const VOICE_NAMES[] = {"8", "16", "24", "32", "48", "64"};
static constexpr int kNumVoiceOpts = 6;
static const int QUALITY[] = {1, 2, 3};         // sfizz sample quality (resampling), 0..10; 1 is sfizz's live default
static const char *const QUALITY_NAMES[] = {"Normal", "High", "Best"};
static constexpr int kNumQualityOpts = 3;

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int option_of(float norm, int n) { return (int)std::lround(clamp01(norm) * (n - 1)); }
static void copy_str(void *dst, const char *src, size_t max) {
    std::strncpy((char *)dst, src, max - 1);
    ((char *)dst)[max - 1] = 0;
}

/* A loaded instrument: one sfizz synth, built and freed on the worker thread only. */
struct Synth {
    sfizz_synth_t *s = nullptr;
    std::string name;
    int voices = 0;
    int quality = -1;      // last value the audio thread set
    float volume = 1e9f;   // last value the audio thread set
    ~Synth() { if (s) sfizz_free(s); }
};

template <class T> struct Handover {   // one-slot, lock-free handover worker -> audio thread (as in NAM MPC)
    std::atomic<T *> pending{nullptr}, retired{nullptr}, live{nullptr};
    void offer(T *t) { delete pending.exchange(t); }
    void take() {
        if (!pending.load(std::memory_order_acquire) || retired.load(std::memory_order_acquire)) return;
        T *old = live.load(std::memory_order_relaxed);
        live.store(pending.exchange(nullptr), std::memory_order_release);
        retired.store(old, std::memory_order_release);
    }
    void collect() { delete retired.exchange(nullptr); }
    void destroy() { delete pending.exchange(nullptr); delete retired.exchange(nullptr); delete live.exchange(nullptr); }
};

struct Plugin {
    AEffect fx{};
    audioMasterCallback master = nullptr;
    std::atomic<float> norm[kNumParams];

    // worker requests (any thread -> worker)
    std::mutex req_mutex;
    std::condition_variable req_cv;
    bool quit = false, rescan = true, reload = false;
    int step = 0, pick = -1;
    std::string by_name;

    // worker state; the UI thread reads the names under lib_mutex
    std::mutex lib_mutex;
    std::vector<fs::path> files;
    int current = -1;
    std::string status = "Scanning...";
    std::atomic<int> count{0}, index{0};
    std::atomic<double> sample_rate{44100.0};
    std::thread worker;
    std::vector<fs::path> bases;

    // audio thread
    Handover<Synth> synth;
    VstMidiEvent events[kMaxEvents];
    std::atomic<bool> panic{false};
    static constexpr int kQueue = 64;   // MIDI carried to the next chunk (audio thread)
    unsigned char queued[kQueue][3];
    int nqueued = 0;
    bool queued_on[128] = {};
    int nevents = 0;
    std::atomic<bool> release[kNumParams];
    std::atomic<bool> update_display{false};
    std::string chunk;

    float value(int i) const { const ParamInfo &p = PARAMS[i]; return p.min + (p.max - p.min) * norm[i].load(); }
    template <class F> void post(F f) {
        { std::lock_guard<std::mutex> l(req_mutex); f(); }
        req_cv.notify_one();
    }
    void run_worker();
    void process(float **out, int n);
};

/* ---- worker ---------------------------------------------------------------------------------------------- */
static std::string lower(std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; }

static std::vector<fs::path> all_bases(const std::vector<fs::path> &fixed) {
    std::vector<fs::path> bases = fixed;
    std::error_code ec;
    for (fs::directory_iterator it("/media", ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (fs::is_directory(it->path() / "SFZ", e2)) bases.push_back(it->path() / "SFZ");
    }
    return bases;
}

static std::vector<fs::path> scan(const std::vector<fs::path> &fixed) {
    std::vector<fs::path> out;
    for (const auto &b : all_bases(fixed)) {
        std::error_code ec;
        if (!fs::is_directory(b, ec)) continue;
        for (fs::recursive_directory_iterator it(b, fs::directory_options::skip_permission_denied | fs::directory_options::follow_directory_symlink, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
            if (it->path().filename().string().rfind('.', 0) == 0) {   // hidden, e.g. macOS "._" twins
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec) && lower(it->path().extension().string()) == ".sfz") out.push_back(it->path());
        }
    }
    // Libraries split instruments into pieces pulled in with #include "x.sfz"; those pieces aren't instruments on
    // their own, so any .sfz that another .sfz includes is left out of the list.
    std::vector<std::string> included;
    for (const auto &f : out) {
        FILE *fp = std::fopen(f.string().c_str(), "rb");
        if (!fp) continue;
        char line[1024];
        while (std::fgets(line, sizeof line, fp)) {
            const char *inc = std::strstr(line, "#include");
            if (!inc) continue;
            const char *q1 = std::strchr(inc, '"'), *q2 = q1 ? std::strchr(q1 + 1, '"') : nullptr;
            if (q1 && q2) included.push_back(lower(fs::path(std::string(q1 + 1, q2)).filename().string()));
        }
        std::fclose(fp);
    }
    out.erase(std::remove_if(out.begin(), out.end(), [&](const fs::path &f) {
        return std::find(included.begin(), included.end(), lower(f.filename().string())) != included.end();
    }), out.end());
    std::sort(out.begin(), out.end(), [](const fs::path &a, const fs::path &b) {
        return lower(a.stem().string()) < lower(b.stem().string());
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static Synth *load_synth(const fs::path &path, double sr, int voices) {
    auto y = std::make_unique<Synth>();
    y->s = sfizz_create_synth();
    if (!y->s) throw std::runtime_error("out of memory");
    sfizz_set_sample_rate(y->s, (float)sr);
    sfizz_set_samples_per_block(y->s, kChunk);
    sfizz_set_num_voices(y->s, voices);
    if (!sfizz_load_file(y->s, path.string().c_str())) throw std::runtime_error("can't read this .sfz");
    if (sfizz_get_num_regions(y->s) == 0) throw std::runtime_error("no sounds in this .sfz");
    y->voices = voices;
    y->name = path.stem().string();
    return y.release();
}

void Plugin::run_worker() {
    std::unique_lock<std::mutex> lk(req_mutex);
    int loaded_voices = 0;
    while (!quit) {
        req_cv.wait_for(lk, std::chrono::milliseconds(100));
        if (quit) break;
        lk.unlock();
        synth.collect();   // freeing an instrument can take a while: never with req_mutex held
        lk.lock();
        const int voices = VOICES[option_of(norm[kVoices].load(), kNumVoiceOpts)];
        bool do_scan = rescan, do_reload = reload || (loaded_voices && voices != loaded_voices);
        int st = step, pk = pick;
        std::string name = by_name;
        rescan = reload = false;
        step = 0;
        pick = -1;
        by_name.clear();
        if (!do_scan && !do_reload && !st && pk < 0 && name.empty()) continue;
        lk.unlock();

        if (do_scan || st) {   // pick up instruments copied in since the last look
            auto found = scan(bases);
            std::lock_guard<std::mutex> l(lib_mutex);
            fs::path cur = current >= 0 && current < (int)files.size() ? files[current] : fs::path();
            files = std::move(found);
            auto it = std::find(files.begin(), files.end(), cur);
            current = it == files.end() ? -1 : (int)(it - files.begin());
            if (files.empty()) status = "No instruments";
        }
        int target = -2;   // -2: nothing to load
        fs::path path;
        {
            std::lock_guard<std::mutex> l(lib_mutex);
            const int n = (int)files.size();
            if (n) {
                if (!name.empty()) {
                    for (int i = 0; i < n; i++)
                        if (files[i].stem().string() == name) target = i;
                    if (target == -2) status = "Missing: " + name;
                } else if (pk >= 0) target = std::min(pk, n - 1);
                else if (st) target = current < 0 ? 0 : ((current + st) % n + n) % n;
                else if (do_scan && current < 0) target = 0;
                if (target == -2 && do_reload && current >= 0) target = current;
                if (target >= 0 && (target != current || do_reload || status.rfind("Error", 0) == 0 || status == "Loading...")) {
                    current = target;
                    status = "Loading...";
                    path = files[target];
                } else if (target == current) target = -2;
            }
        }
        update_display = true;
        if (target >= 0) {
            try {
                synth.offer(load_synth(path, sample_rate.load(), voices));
                loaded_voices = voices;
                std::lock_guard<std::mutex> l(lib_mutex);
                status.clear();
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> l(lib_mutex);
                status = std::string("Error: ") + e.what();
            }
        }
        {
            std::lock_guard<std::mutex> l(lib_mutex);
            count = (int)files.size();
            index = std::max(current, 0);
        }
        update_display = true;
        lk.lock();
    }
}

/* ---- audio ----------------------------------------------------------------------------------------------- */
static void send_midi(sfizz_synth_t *s, const unsigned char *m, int delay) {
    const int st = m[0] & 0xf0;
    switch (st) {
    case 0x90: if (m[2]) { sfizz_send_note_on(s, delay, m[1], m[2]); break; } /* velocity 0 = note off */ [[fallthrough]];
    case 0x80: sfizz_send_note_off(s, delay, m[1], m[2]); break;
    case 0xb0:
        if (m[1] == 120) sfizz_all_sound_off(s);   // all sound off: silence now
        else if (m[1] == 123) { for (int k = 0; k < 128; k++) sfizz_send_note_off(s, delay, k, 0); }   // all notes off
        else sfizz_send_cc(s, delay, m[1], m[2]);
        break;
    case 0xe0: sfizz_send_pitch_wheel(s, delay, (int)((m[2] << 7) | m[1]) - 8192); break;
    case 0xd0: sfizz_send_channel_aftertouch(s, delay, m[1]); break;
    case 0xa0: sfizz_send_poly_aftertouch(s, delay, m[1], m[2]); break;
    case 0xc0: sfizz_send_program_change(s, delay, m[1]); break;
    default: break;
    }
}

void Plugin::process(float **out, int n) {
    synth.take();
    Synth *y = synth.live.load(std::memory_order_relaxed);
    if (master) {
        for (int i = 0; i < kNumParams; i++)
            if (release[i].exchange(false)) master(&fx, audioMasterAutomate, i, 0, nullptr, 0.0f);
        if (update_display.exchange(false)) master(&fx, audioMasterUpdateDisplay, 0, 0, nullptr, 0.0f);
    }
    if (!y) {
        for (int i = 0; i < n; i++) { out[0][i] = 0; if (out[1]) out[1][i] = 0; }
        nevents = 0;
        return;
    }
    // real-time settings (sfizz: RT thread only)
    const float vol = value(kVolume);
    if (vol != y->volume) { sfizz_set_volume(y->s, vol); y->volume = vol; }
    const int q = QUALITY[option_of(norm[kQuality].load(), kNumQualityOpts)];
    if (q != y->quality) { sfizz_set_sample_quality(y->s, SFIZZ_PROCESS_LIVE, q); y->quality = q; }

    int e = 0;
    // stable: events at the same moment keep MPC's order (a note's on before its off), or the note would hang
    std::stable_sort(events, events + nevents, [](const VstMidiEvent &a, const VstMidiEvent &b) { return a.deltaFrames < b.deltaFrames; });
    if (panic.exchange(false)) sfizz_all_sound_off(y->s);   // MPC stopped or bypassed us: nothing keeps sounding
    // Two sfizz quirks would leave notes hanging, so notes are spaced out by a sample where needed (inaudible):
    //  - an off on the same sample as its own on is ignored (the voice hasn't started): the off goes 1 sample later
    //  - notes starting on the same sample, in an instrument where a new note cuts the last (a sax), fail to cut it
    //    (a chord leaves a note sounding forever): each note-on goes at least 1 sample after the previous one
    // Anything pushed past the end of a chunk waits in a small queue for the start of the next chunk.
    int on_at[128], last_on = -1;
    auto queue = [&](const unsigned char *md) {
        if (nqueued < kQueue) { std::memcpy(queued[nqueued], md, 3); nqueued++; return true; }
        return false;
    };
    auto send = [&](const unsigned char *md, int d, int m) {
        const int st = md[0] & 0xf0, key = md[1] & 0x7f;
        const bool on = st == 0x90 && md[2], offn = st == 0x80 || (st == 0x90 && !md[2]);
        if (on) {
            if (d <= last_on) d = last_on + 1;
            if (d >= m && queue(md)) { queued_on[key] = true; return; }
            d = std::min(d, m - 1);
            last_on = d;
            on_at[key] = d;
            sfizz_send_note_on(y->s, d, key, md[2]);
            return;
        }
        if (offn) {
            if (queued_on[key] && queue(md)) return;   // its note hasn't been sent yet: keep the order
            if (on_at[key] >= d) {
                if (on_at[key] + 1 < m) sfizz_send_note_off(y->s, on_at[key] + 1, key, md[2]);
                else if (!queue(md)) sfizz_send_note_off(y->s, m - 1, key, md[2]);
                return;
            }
        }
        send_midi(y->s, md, d);
    };
    for (int off = 0; off < n; off += kChunk) {
        const int m = std::min(kChunk, n - off);
        std::fill(on_at, on_at + 128, -1);
        last_on = -1;
        if (nqueued) {   // what the last chunk couldn't fit, first, in order
            unsigned char q[kQueue][3];
            const int nq = nqueued;
            std::memcpy(q, queued, sizeof q);
            nqueued = 0;
            std::fill(queued_on, queued_on + 128, false);
            for (int k = 0; k < nq; k++) send(q[k], 0, m);
        }
        for (; e < nevents && events[e].deltaFrames < off + m; e++)
            send(events[e].midiData, std::max(0, events[e].deltaFrames - off), m);
        float *o[2] = {out[0] + off, (out[1] ? out[1] : out[0]) + off};
        sfizz_render_block(y->s, o, 2, m);
        for (int c = 0; c < 2; c++)   // never hand MPC a non-number (it would silence the whole mix) or anything absurd
            for (int i = 0; i < m; i++) {
                float v = o[c][i];
                uint32_t u;
                std::memcpy(&u, &v, 4);
                o[c][i] = (u & 0x7f800000u) == 0x7f800000u ? 0.0f : v > 4.0f ? 4.0f : v < -4.0f ? -4.0f : v;
            }
    }
    for (; e < nevents; e++) send(events[e].midiData, std::max(0, n - 1), n);   // late events: at the end
    nevents = 0;
}

/* ---- VST2 entry points ----------------------------------------------------------------------------------- */
static Plugin *P(AEffect *e) { return (Plugin *)e->object; }

static float getParameter(AEffect *e, int32_t i) {
    Plugin *s = P(e);
    if (i < 0 || i >= kNumParams) return 0;
    if (i == kInstrument) { const int n = s->count.load(); return n > 1 ? (float)s->index.load() / (n - 1) : 0.0f; }
    return s->norm[i].load();
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = P(e);
    if (i < 0 || i >= kNumParams) return;
    v = clamp01(v);
    switch (i) {
    case kInstrument: {   // on an entry: select it; between entries (a Q-Link nudge): step one
        const int n = s->count.load();
        if (n < 1) { s->post([&] { s->rescan = true; }); return; }
        const float pos = v * (n - 1), cur = (float)s->index.load();
        if (std::fabs(pos - std::round(pos)) > 0.001f) { const int d = pos > cur ? 1 : -1; s->post([&] { s->step += d; }); }
        else if ((int)std::lround(pos) != (int)cur) { const int p = (int)std::lround(pos); s->post([&] { s->pick = p; }); }
        return;
    }
    case kPrev: case kNext:
        if (v > 0.5f) { const int d = i == kNext ? 1 : -1; s->post([&] { s->step += d; }); s->release[i] = true; }
        s->norm[i] = 0;
        return;
    case kVoices: {   // an option; a value between options is a Q-Link nudge: step one
        const float pos = v * (kNumVoiceOpts - 1), cur = (float)option_of(s->norm[i].load(), kNumVoiceOpts);
        int opt = (std::fabs(pos - std::round(pos)) > 0.001f) ? (int)cur + (pos > cur ? 1 : -1) : (int)std::lround(pos);
        opt = std::max(0, std::min(kNumVoiceOpts - 1, opt));
        s->norm[i] = (float)opt / (kNumVoiceOpts - 1);
        s->post([] {});   // the worker rebuilds the synth with the new voice count
        s->update_display = true;
        return;
    }
    case kQuality: {
        const float pos = v * (kNumQualityOpts - 1), cur = (float)option_of(s->norm[i].load(), kNumQualityOpts);
        int opt = (std::fabs(pos - std::round(pos)) > 0.001f) ? (int)cur + (pos > cur ? 1 : -1) : (int)std::lround(pos);
        s->norm[i] = (float)std::max(0, std::min(kNumQualityOpts - 1, opt)) / (kNumQualityOpts - 1);
        s->update_display = true;
        return;
    }
    default: s->norm[i] = v;
    }
}

static void param_display(Plugin *s, int i, char *out) {
    const int n = 24;
    switch (i) {
    case kInstrument: {
        std::lock_guard<std::mutex> l(s->lib_mutex);
        if (!s->status.empty() || s->current < 0 || s->current >= (int)s->files.size())
            copy_str(out, s->status.empty() ? "-" : s->status.c_str(), n);
        else copy_str(out, s->files[s->current].stem().string().c_str(), n);
        return;
    }
    case kPrev: copy_str(out, "<", n); return;
    case kNext: copy_str(out, ">", n); return;
    case kVolume: std::snprintf(out, n, "%.1f", s->value(i)); return;
    case kVoices: copy_str(out, VOICE_NAMES[option_of(s->norm[i].load(), kNumVoiceOpts)], n); return;
    case kQuality: copy_str(out, QUALITY_NAMES[option_of(s->norm[i].load(), kNumQualityOpts)], n); return;
    }
}

static void get_state(Plugin *s, std::string &out) {
    out = "SFZMPC 1\n";
    char buf[64];
    for (int i = kVolume; i < kNumParams; i++) {
        std::snprintf(buf, sizeof buf, "%s=%.6f\n", PARAMS[i].key, s->norm[i].load());
        out += buf;
    }
    std::lock_guard<std::mutex> l(s->lib_mutex);
    if (s->current >= 0 && s->current < (int)s->files.size())
        out += "instrument_file=" + s->files[s->current].stem().string() + "\n";
}

static void set_state(Plugin *s, const char *data, size_t len) {
    std::string text(data, strnlen(data, len));
    if (text.rfind("SFZMPC ", 0) != 0) return;
    std::string name;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "instrument_file") name = v;
        else
            for (int i = kVolume; i < kNumParams; i++)
                if (k == PARAMS[i].key) s->norm[i] = clamp01((float)std::atof(v.c_str()));
    }
    s->post([&] { s->by_name = name; s->reload = name.empty(); });
    s->update_display = true;
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float opt) {
    Plugin *s = P(e);
    switch (op) {
    case effOpen: return 1;
    case effClose:
        s->post([&] { s->quit = true; });
        if (s->worker.joinable()) s->worker.join();
        s->synth.destroy();
        delete s;
        return 1;
    case effGetPlugCategory: return kPlugCategSynth;
    case effGetEffectName: case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: if (idx >= 0 && idx < kNumParams) copy_str(p, PARAMS[idx].unit, 8); return 1;
    case effGetParamDisplay: if (idx >= 0 && idx < kNumParams) param_display(s, idx, (char *)p); return 1;
    case effSetSampleRate:
        if (opt > 0 && std::fabs(opt - s->sample_rate.load()) > 0.5) { s->sample_rate = opt; s->post([&] { s->reload = true; }); }
        return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (!v) s->panic = true; return 1;
    case effProcessEvents: {
        const VstEvents *ev = (const VstEvents *)p;
        for (int k = 0; ev && k < ev->numEvents && s->nevents < kMaxEvents; k++)
            if (ev->events[k] && ev->events[k]->type == 1) s->events[s->nevents++] = *(const VstMidiEvent *)ev->events[k];
        return 1;
    }
    case effCanDo:
        return (p && (!std::strcmp((const char *)p, "receiveVstEvents") || !std::strcmp((const char *)p, "receiveVstMidiEvent")))
                   ? 1 : -1;
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

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) { (void)in; P(e)->process(out, n); }
static void processAccumulate(AEffect *e, float **in, float **out, int32_t n) {
    (void)in;
    std::vector<float> l(n), r(n);
    float *o[2] = {l.data(), r.data()};
    P(e)->process(o, n);
    for (int k = 0; k < n; k++) { out[0][k] += l[k]; if (out[1]) out[1][k] += r[k]; }
}

static int anchor;
static fs::path plugin_dir() {
    Dl_info info;
    if (dladdr((void *)&anchor, &info) && info.dli_fname) return fs::path(info.dli_fname).parent_path();
    return {};
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    Plugin *s = new (std::nothrow) Plugin();
    if (!s) return nullptr;
    s->master = master;
    for (int i = 0; i < kNumParams; i++) {
        const ParamInfo &p = PARAMS[i];
        s->norm[i] = (i == kVoices || i == kQuality) ? p.def : (p.max > p.min ? (p.def - p.min) / (p.max - p.min) : 0.0f);
        s->release[i] = false;
    }
    if (const char *env = std::getenv("SFZ_MPC_DIR")) s->bases.push_back(env);   // for testing off the device
    fs::path dir = plugin_dir();
    if (!dir.empty()) s->bases.push_back(dir / "SFZ");
    s->bases.push_back("/sdcard/SFZ");
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
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks | effFlagsIsSynth;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = s;
    return e;
}
