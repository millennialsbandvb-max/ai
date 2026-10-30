/* Quad Rig: four effect slots and a NAM amp in one plugin for MPC OS (VST2, stereo), like a small pedalboard.
 * Each slot is one of Duo FX's algorithms or one of Ambient Lab's machines (or Off), with its eight knobs, Mix,
 * presets, Bypass (its tail rings out) and Hold (Ambient Lab's freeze). The amp is NAM MPC (models, cab IR, EQ)
 * and sits wherever Amp Position puts it: first, after FX 1, 2 or 3, or last.
 * It doesn't copy their code: it hosts them. Inside are four Duo FX engines (slot A only), four Ambient Lab engines
 * and one NAM MPC engine, built from their own sources (their VSTPluginMain renamed: duo_entry, amb_entry,
 * nam_entry); only a slot's chosen engine runs. Parameters are passed through, and the engines' own reports
 * (knob values after a preset or algorithm change) come back through our host callback and on to MPC.
 * State: a chunk with our parameters and NAM's own chunk (its model and IR paths).
 * MIDI (any channel): CC 81-84 bypass FX 1-4 (64+ = bypassed), CC 85 the amp, CC 64 Hold on every slot. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vst2.h"
#include "../../tools/picklist.h"

enum { audioMasterVersion = 1 };

AEffect *duo_entry(audioMasterCallback);
AEffect *amb_entry(audioMasterCallback);
AEffect *nam_entry(audioMasterCallback);
int duo_pick(AEffect *, int, int, int, char *, int);   /* their full-page lists (tools/picklist.h) */
int amb_pick(AEffect *, int, int, int, char *, int);
int nam_pick(AEffect *, int, int, int, char *, int);

#define NS 4
/* the engines' own parameter numbers we use */
enum { DUO_ALGO = 0, DUO_MIX = 2, DUO_K = 3, DUO_B_ALGO = 20, DUO_PRESET = 42, DUO_NALGO = 58 };   /* (Duo: 57 + Off) */
enum { AMB_MACHINE = 0, AMB_MIX = 2, AMB_K = 3, AMB_HOLD = 11, AMB_PRESET = 12, AMB_NMACH = 11 };
#define N_DUO (DUO_NALGO - 1)                  /* Duo's algorithms without its Off */
#define N_FX (N_DUO + AMB_NMACH + 1)           /* our list: Duo's, Ambient Lab's, Off */
#define FX_OFF (N_FX - 1)
#define NAM_N 21

enum { kAmpPos, kAmpBypass, kOutput, kSlot0 };
enum { S_FX, S_OPEN, S_MIX, S_K, S_PRESET = S_K + 8, S_PREV, S_NEXT, S_BYPASS, S_HOLD, SP };
#define SLOT(s, k) (kSlot0 + (s) * SP + (k))
enum { kNam0 = kSlot0 + NS * SP, kNumParams = kNam0 + NAM_N };
static const char *const AMP_POS[] = {"Amp First", "After FX 1", "After FX 2", "After FX 3", "Amp Last"};

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    AEffect *duo[NS], *amb[NS], *nam;
    int kind[NS];                  /* what each slot runs now: 0 Duo, 1 Ambient Lab, 2 Off */
    float byp_s[NS];
    volatile int update_display;
    volatile unsigned char automate[kNumParams];
    char *chunk;
} Plugin;

static Plugin *all_plugins[64];
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int option_of(float v, int n) { return (int)lrintf(clampf(v, 0, 1) * (float)(n - 1)); }
static void copy_str(void *dst, const char *src, size_t max) { const size_t n = strnlen(src, max - 1); memcpy(dst, src, n); ((char *)dst)[n] = 0; }
static int finite_bits(float v) { uint32_t u; memcpy(&u, &v, 4); return (u & 0x7f800000u) != 0x7f800000u; }
static int fx_of(const Plugin *s, int k) { return option_of(s->norm[SLOT(k, S_FX)], N_FX); }
static int kind_of(int fx) { return fx < N_DUO ? 0 : fx < N_DUO + AMB_NMACH ? 1 : 2; }

/* ---- the engines' reports back -------------------------------------------------------------------------------- */
static Plugin *owner_of(AEffect *e, int *slot, int *what) {   /* what: 0 duo, 1 ambient, 2 nam */
    for (int i = 0; i < 64; i++) {
        Plugin *s = all_plugins[i];
        if (!s) continue;
        if (s->nam == e) { *what = 2; *slot = -1; return s; }
        for (int k = 0; k < NS; k++) {
            if (s->duo[k] == e) { *what = 0; *slot = k; return s; }
            if (s->amb[k] == e) { *what = 1; *slot = k; return s; }
        }
    }
    return NULL;
}
static intptr_t engine_host(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    (void)val; (void)p;
    if (op == audioMasterVersion) return 2400;
    if (!e) return 0;
    int slot, what;
    Plugin *s = owner_of(e, &slot, &what);
    if (!s) return 0;
    if (op == audioMasterUpdateDisplay) { s->update_display = 1; return 1; }
    if (op != audioMasterAutomate) return 0;
    int ours = -1;
    float v = opt;
    if (what == 2) { if (idx >= 0 && idx < NAM_N) ours = kNam0 + idx; }
    else if (s->kind[slot] == what) {   /* only the slot's running engine speaks for it */
        if (idx == (what ? AMB_MIX : DUO_MIX)) ours = SLOT(slot, S_MIX);
        else if (idx >= DUO_K && idx < DUO_K + 8) ours = SLOT(slot, S_K + idx - DUO_K);
        else if (what == 1 && idx == AMB_HOLD) ours = SLOT(slot, S_HOLD);
        else if (idx == (what ? AMB_PRESET : DUO_PRESET)) ours = SLOT(slot, S_PRESET);
        else if (idx == (what ? AMB_MACHINE : DUO_ALGO)) {   /* a preset can change the algorithm / machine */
            const int fx = what ? N_DUO + option_of(opt, AMB_NMACH) : option_of(opt, DUO_NALGO);
            if (fx < N_FX && (what || fx < N_DUO)) { ours = SLOT(slot, S_FX); v = (float)fx / (float)(N_FX - 1); }
        }
    }
    if (ours >= 0) { s->norm[ours] = v; s->automate[ours] = 1; }
    return 1;
}

/* ---- the slots ------------------------------------------------------------------------------------------------- */
static AEffect *engine(Plugin *s, int k) { return s->kind[k] == 0 ? s->duo[k] : s->kind[k] == 1 ? s->amb[k] : NULL; }
/* which engine parameter a slot parameter is (-1: ours alone) */
static int engine_param(const Plugin *s, int k, int w) {
    const int kind = s->kind[k];
    if (kind == 2) return -1;
    switch (w) {
    case S_MIX: return kind ? AMB_MIX : DUO_MIX;
    case S_PRESET: return kind ? AMB_PRESET : DUO_PRESET;
    case S_PREV: return (kind ? AMB_PRESET : DUO_PRESET) + 1;
    case S_NEXT: return (kind ? AMB_PRESET : DUO_PRESET) + 2;
    case S_HOLD: return kind ? AMB_HOLD : -1;
    default: return w >= S_K && w < S_K + 8 ? (kind ? AMB_K : DUO_K) + w - S_K : -1;
    }
}
static void choose_fx(Plugin *s, int k, int fx) {   /* point the slot at an engine and set its algorithm / machine */
    const int kind = kind_of(fx);
    if (kind != s->kind[k] && kind < 2) {   /* the engine coming in starts from silence */
        AEffect *e = kind ? s->amb[k] : s->duo[k];
        e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    }
    s->kind[k] = kind;
    if (kind == 0) s->duo[k]->setParameter(s->duo[k], DUO_ALGO, (float)fx / (float)(DUO_NALGO - 1));
    else if (kind == 1) s->amb[k]->setParameter(s->amb[k], AMB_MACHINE, (float)(fx - N_DUO) / (float)(AMB_NMACH - 1));
    /* (the engine reports its defaults back; until then show them as they are) */
    AEffect *e = engine(s, k);
    if (e) {
        s->norm[SLOT(k, S_MIX)] = e->getParameter(e, engine_param(s, k, S_MIX));
        for (int i = 0; i < 8; i++) s->norm[SLOT(k, S_K + i)] = e->getParameter(e, engine_param(s, k, S_K + i));
    }
    s->update_display = 1;
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams) return;
    v = clampf(v, 0, 1);
    if (i >= kNam0) { s->nam->setParameter(s->nam, i - kNam0, v); s->norm[i] = s->nam->getParameter(s->nam, i - kNam0); return; }
    if (i == kAmpPos) { s->norm[i] = (float)option_of(v, 5) / 4; s->update_display = 1; return; }
    if (i == kAmpBypass) { s->norm[i] = v > 0.5f ? 1 : 0; s->update_display = 1; return; }
    if (i == kOutput) { s->norm[i] = v; return; }
    const int k = (i - kSlot0) / SP, w = (i - kSlot0) % SP;
    if (w == S_FX) {   /* a pick from the list (which closes it), or a Q-Link nudge (steps one) */
        const float pos = v * (float)(N_FX - 1), cur = (float)fx_of(s, k);
        const int between = fabsf(pos - roundf(pos)) > 0.001f;
        int opt = between && fabsf(pos - cur) < 1 ? (int)cur + (pos > cur ? 1 : -1) : (int)lrintf(pos);
        opt = opt < 0 ? 0 : opt > N_FX - 1 ? N_FX - 1 : opt;
        s->norm[i] = (float)opt / (float)(N_FX - 1);
        if (opt != (int)cur || kind_of(opt) != s->kind[k]) choose_fx(s, k, opt);
        if (!between && s->norm[SLOT(k, S_OPEN)] > 0.5f) { s->norm[SLOT(k, S_OPEN)] = 0; s->automate[SLOT(k, S_OPEN)] = 1; }
        s->update_display = 1;
        return;
    }
    if (w == S_OPEN || w == S_BYPASS) { s->norm[i] = v > 0.5f ? 1 : 0; s->update_display = 1; return; }
    const int ep = engine_param(s, k, w);
    AEffect *en = engine(s, k);
    if (ep < 0 || !en) { s->norm[i] = w == S_PREV || w == S_NEXT ? 0 : v; return; }
    en->setParameter(en, ep, v);
    s->norm[i] = en->getParameter(en, ep);   /* (the engine may snap it, or a button resets to 0) */
    if (w == S_PREV || w == S_NEXT || w == S_PRESET) s->update_display = 1;
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }

/* ---- names and values on the page ------------------------------------------------------------------------------- */
static char FX_NAMES[N_FX][32];
static void fill_fx_names(void) {
    if (FX_NAMES[0][0]) return;
    AEffect *d = duo_entry(engine_host), *a = amb_entry(engine_host);
    for (int f = 0; f < N_FX; f++) {
        char t[64] = {0};
        if (f < N_DUO) { d->setParameter(d, DUO_ALGO, (float)f / (float)(DUO_NALGO - 1)); d->dispatcher(d, effGetParamDisplay, DUO_ALGO, 0, t, 0); }
        else if (f < N_DUO + AMB_NMACH) { a->setParameter(a, AMB_MACHINE, (float)(f - N_DUO) / (float)(AMB_NMACH - 1)); a->dispatcher(a, effGetParamDisplay, AMB_MACHINE, 0, t, 0); }
        else snprintf(t, sizeof t, "Off");
        copy_str(FX_NAMES[f], t, sizeof FX_NAMES[f]);
    }
    d->dispatcher(d, effClose, 0, 0, NULL, 0);
    a->dispatcher(a, effClose, 0, 0, NULL, 0);
}

static void param_name(Plugin *s, int i, char *t) {
    if (i >= kNam0) { s->nam->dispatcher(s->nam, effGetParamName, i - kNam0, 0, t, 0); return; }
    if (i < kSlot0) { static const char *const N[] = {"Amp Position", "Amp Bypass", "Output"}; snprintf(t, 32, "%s", N[i]); return; }
    const int k = (i - kSlot0) / SP, w = (i - kSlot0) % SP;
    const int ep = engine_param(s, k, w);
    if (w >= S_K && w < S_K + 8) {   /* the algorithm's own name for the knob (MPC draws these live) */
        AEffect *en = engine(s, k);
        if (en && ep >= 0) { char n[64] = {0}; en->dispatcher(en, effGetParamName, ep, 0, n, 0); snprintf(t, 32, "%s", n); }
        else snprintf(t, 32, "-");
        return;
    }
    static const char *const W[SP] = {"Effect", "Effect List", "Mix", "", "", "", "", "", "", "", "", "Preset", "Preset <", "Preset >", "Bypass", "Hold"};
    snprintf(t, 32, "FX%d %s", k + 1, W[w]);
}
static void param_display(Plugin *s, int i, char *t) {
    if (i >= kNam0) { s->nam->dispatcher(s->nam, effGetParamDisplay, i - kNam0, 0, t, 0); return; }
    if (i == kAmpPos) { snprintf(t, 32, "%s", AMP_POS[option_of(s->norm[i], 5)]); return; }
    if (i == kAmpBypass) { snprintf(t, 32, "%s", s->norm[i] > 0.5f ? "Bypassed" : "On"); return; }
    if (i == kOutput) { snprintf(t, 32, "%+.1f dB", -24 + 36 * s->norm[i]); return; }
    const int k = (i - kSlot0) / SP, w = (i - kSlot0) % SP;
    if (w == S_FX) { snprintf(t, 32, "%s", FX_NAMES[fx_of(s, k)]); return; }
    if (w == S_OPEN) { snprintf(t, 32, "%s", s->norm[i] > 0.5f ? "Open" : "Closed"); return; }
    if (w == S_BYPASS) { snprintf(t, 32, "%s", s->norm[i] > 0.5f ? "Bypassed" : "On"); return; }
    const int ep = engine_param(s, k, w);
    AEffect *en = engine(s, k);
    if (ep < 0 || !en) { snprintf(t, 32, w == S_HOLD ? "Off" : w == S_PRESET ? "-" : " "); return; }
    char n[96] = {0};
    en->dispatcher(en, effGetParamDisplay, ep, 0, n, 0);
    snprintf(t, 32, "%s", n);
}

/* ---- state: our parameters (as text) and NAM's own chunk -------------------------------------------------------- */
static int get_chunk(Plugin *s, void **out) {
    void *nc = NULL;
    const intptr_t nn = s->nam->dispatcher(s->nam, effGetChunk, 0, 0, &nc, 0);
    const size_t cap = 64 + (size_t)kNumParams * 16 + (nn > 0 ? (size_t)nn : 0);
    free(s->chunk);
    s->chunk = malloc(cap);
    if (!s->chunk) return 0;
    size_t o = (size_t)snprintf(s->chunk, cap, "QRIG1 %d\n", kNumParams);
    for (int i = 0; i < kNumParams; i++) o += (size_t)snprintf(s->chunk + o, cap - o, "%.6f\n", s->norm[i]);
    o += (size_t)snprintf(s->chunk + o, cap - o, "NAM %ld\n", (long)(nn > 0 ? nn : 0));
    if (nn > 0 && nc) { memcpy(s->chunk + o, nc, (size_t)nn); o += (size_t)nn; }
    *out = s->chunk;
    return (int)o;
}
static void set_chunk(Plugin *s, const char *c, size_t n) {
    int np = 0, used = 0;
    if (n < 8 || sscanf(c, "QRIG1 %d\n%n", &np, &used) != 1 || np != kNumParams) return;
    float v[kNumParams];
    size_t o = (size_t)used;
    for (int i = 0; i < kNumParams; i++) {
        int u = 0;
        if (o >= n || sscanf(c + o, "%f\n%n", &v[i], &u) != 1) return;
        o += (size_t)u;
    }
    /* each slot's effect first (it loads that effect's defaults), then everything else over it */
    for (int k = 0; k < NS; k++) setParameter(&s->fx, SLOT(k, S_FX), v[SLOT(k, S_FX)]);
    for (int i = 0; i < kNam0; i++) {
        const int w = i >= kSlot0 ? (i - kSlot0) % SP : -1;
        if (w == S_FX || w == S_PREV || w == S_NEXT) continue;
        setParameter(&s->fx, i, v[i]);
    }
    long nn = 0;
    int u = 0;
    if (o < n && sscanf(c + o, "NAM %ld\n%n", &nn, &u) == 1 && nn > 0 && o + (size_t)u + (size_t)nn <= n)
        s->nam->dispatcher(s->nam, effSetChunk, 0, (intptr_t)nn, (void *)(c + o + (size_t)u), 0);
    for (int i = kNam0; i < kNumParams; i++) s->norm[i] = s->nam->getParameter(s->nam, i - kNam0);
    s->update_display = 1;
}

/* ---- sound ------------------------------------------------------------------------------------------------------ */
#define BLK 512
static void run_engine(AEffect *e, float *l, float *r, float *tl, float *tr, int n) {
    float *in[2] = {l, r}, *out[2] = {tl, tr};
    e->processReplacing(e, in, out, n);
}
static void process(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *s = (Plugin *)e->object;
    if (s->master) {
        for (int i = 0; i < kNumParams; i++)
            if (s->automate[i]) { s->automate[i] = 0; s->master(e, audioMasterAutomate, i, 0, NULL, s->norm[i]); s->update_display = 1; }
        if (s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    }
    const int pos = option_of(s->norm[kAmpPos], 5), amp_on = s->norm[kAmpBypass] < 0.5f;
    const float og = powf(10, (-24 + 36 * s->norm[kOutput]) / 20);
    for (int off = 0; off < n; off += BLK) {
        const int m = n - off < BLK ? n - off : BLK;
        float l[BLK], r[BLK], tl[BLK], tr[BLK];
        for (int k = 0; k < m; k++) {
            float a = in && in[0] ? in[0][off + k] : 0, b = in && in[1] ? in[1][off + k] : a;
            if (!finite_bits(a) || a > 8 || a < -8) a = 0;   /* only clean audio in */
            if (!finite_bits(b) || b > 8 || b < -8) b = 0;
            l[k] = a; r[k] = b;
        }
        for (int stage = 0; stage <= NS; stage++) {
            if (stage == pos && amp_on) {   /* the amp, where Amp Position puts it */
                run_engine(s->nam, l, r, tl, tr, m);
                memcpy(l, tl, sizeof(float) * (size_t)m); memcpy(r, tr, sizeof(float) * (size_t)m);
            }
            if (stage == NS) break;
            AEffect *en = engine(s, stage);
            const float want = s->norm[SLOT(stage, S_BYPASS)] > 0.5f ? 0 : 1;
            if (!en) { s->byp_s[stage] = want; continue; }
            /* bypassed: the engine hears silence (its tail rings out) and the sound passes by it */
            float gl[BLK], gr[BLK], g[BLK];
            for (int k = 0; k < m; k++) {
                s->byp_s[stage] += 0.003f * (want - s->byp_s[stage]);
                g[k] = s->byp_s[stage];
                gl[k] = l[k] * g[k]; gr[k] = r[k] * g[k];
            }
            run_engine(en, gl, gr, tl, tr, m);
            for (int k = 0; k < m; k++) { l[k] = tl[k] + l[k] * (1 - g[k]); r[k] = tr[k] + r[k] * (1 - g[k]); }
        }
        for (int k = 0; k < m; k++) {
            float a = l[k] * og, b = r[k] * og;
            if (!finite_bits(a) || !finite_bits(b)) a = b = 0;
            out[0][off + k] = clampf(a, -4, 4);   /* +12 dB ceiling (each engine has its own safety too) */
            out[1][off + k] = clampf(b, -4, 4);
        }
    }
}

static void close_all(Plugin *s) {
    for (int k = 0; k < NS; k++) {
        if (s->duo[k]) s->duo[k]->dispatcher(s->duo[k], effClose, 0, 0, NULL, 0);
        if (s->amb[k]) s->amb[k]->dispatcher(s->amb[k], effClose, 0, 0, NULL, 0);
    }
    if (s->nam) s->nam->dispatcher(s->nam, effClose, 0, 0, NULL, 0);
    for (int i = 0; i < 64; i++) if (all_plugins[i] == s) all_plugins[i] = NULL;
    free(s->chunk);
    free(s);
}
static void all_engines(Plugin *s, int32_t op, intptr_t val, float opt) {
    for (int k = 0; k < NS; k++) {
        s->duo[k]->dispatcher(s->duo[k], op, 0, val, NULL, opt);
        s->amb[k]->dispatcher(s->amb[k], op, 0, val, NULL, opt);
    }
    s->nam->dispatcher(s->nam, op, 0, val, NULL, opt);
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: close_all(s); return 1;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) { char t[64] = {0}; param_name(s, idx, t); copy_str(p, t, 32); } return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay: if (idx >= 0 && idx < kNumParams) { char t[96] = {0}; param_display(s, idx, t); copy_str(p, t, 24); } return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "Quad Rig", 32); return 1;
    case effGetVendorString: copy_str(p, "Other Things", 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate: s->fs = opt; all_engines(s, effSetSampleRate, 0, opt); return 1;
    case effSetBlockSize: all_engines(s, effSetBlockSize, val, 0); return 1;
    case effMainsChanged: all_engines(s, effMainsChanged, val, 0); return 1;
    case effGetChunk: return get_chunk(s, (void **)p);
    case effSetChunk: if (val > 0 && p) set_chunk(s, (const char *)p, (size_t)val); return 1;
    case 25: {   /* effProcessEvents: MIDI */
        typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4]; char d, v, r1, r2; } Ev;
        typedef struct { int32_t numEvents; intptr_t reserved; Ev *events[2]; } Evs;
        const Evs *ev = (const Evs *)p;
        for (int k = 0; ev && k < ev->numEvents; k++) {
            if (!ev->events[k] || ev->events[k]->type != 1 || (ev->events[k]->midiData[0] & 0xf0) != 0xb0) continue;
            const int cc = ev->events[k]->midiData[1], hi = ev->events[k]->midiData[2] >= 64;
            int i = -1;
            if (cc >= 81 && cc <= 84) i = SLOT(cc - 81, S_BYPASS);
            else if (cc == 85) i = kAmpBypass;
            if (i >= 0) { s->norm[i] = (float)hi; s->automate[i] = 1; }
            if (cc == 64)
                for (int sl = 0; sl < NS; sl++) if (s->kind[sl] == 1) { setParameter(&s->fx, SLOT(sl, S_HOLD), (float)hi); s->automate[SLOT(sl, S_HOLD)] = 1; }
        }
        return 1;
    }
    case effCanDo:
        return p && (!strcmp((const char *)p, "receiveVstEvents") || !strcmp((const char *)p, "receiveVstMidiEvent")) ? 1 : -1;
    default: return 0;
    }
}

/* the full-page lists (tools/picklist.h): each slot's presets (its engine's), the amp's folders, models, IR folders
 * and IRs (NAM's), then each slot's eight knobs (lists 8..39: a choice knob lists its choices, as its engine says) */
static int quad_pick(AEffect *e, int list, int op, int i, char *out, int max) {
    Plugin *s = (Plugin *)e->object;
    if (list >= NS + 4) {
        const int k = (list - NS - 4) / 8, w = (list - NS - 4) % 8, r = s->kind[k] == 0 ? duo_pick(s->duo[k], 3 + w, op, i, out, max)
                      : s->kind[k] == 1 ? amb_pick(s->amb[k], 1 + w, op, i, out, max) : op == PL_HEADING ? 0 : -1;
        if (op == PL_HEADING && out[0]) {   /* "FX 2 Pedal", not the engine's "A Pedal" */
            char t[64];
            snprintf(t, sizeof t, "FX %d %s", k + 1, s->kind[k] == 0 && out[1] == ' ' ? out + 2 : out);
            snprintf(out, (size_t)max, "%s", t);
        }
        return r;
    }
    if (list >= NS) return nam_pick(s->nam, list - NS, op, i, out, max);
    if (op == PL_HEADING) return 0;
    if (s->kind[list] == 0) return duo_pick(s->duo[list], 0, op, i, out, max);
    if (s->kind[list] == 1) return amb_pick(s->amb[list], 0, op, i, out, max);
    return op == PL_CURRENT ? -1 : 0;   /* (Off: no presets) */
}
#define KN(f) "f" #f "_k1", "f" #f "_k2", "f" #f "_k3", "f" #f "_k4", "f" #f "_k5", "f" #f "_k6", "f" #f "_k7", "f" #f "_k8"
#define KT(f) "FX " #f " Knob 1", "FX " #f " Knob 2", "FX " #f " Knob 3", "FX " #f " Knob 4", "FX " #f " Knob 5", "FX " #f " Knob 6", \
              "FX " #f " Knob 7", "FX " #f " Knob 8"
static const char *const LIST_KEYS[] = {"f1_preset", "f2_preset", "f3_preset", "f4_preset", "amp_folder", "amp_model", "amp_ir_folder", "amp_ir",
                                        KN(1), KN(2), KN(3), KN(4)};
static const char *const LIST_TITLES[] = {"FX 1 Presets", "FX 2 Presets", "FX 3 Presets", "FX 4 Presets", "Model Folders", "Models",
                                          "IR Folders", "IRs", KT(1), KT(2), KT(3), KT(4)};
static const PickLists LISTS = {8 + NS * 8, LIST_KEYS, LIST_TITLES, quad_pick};

static AEffect *make(audioMasterCallback master) {
    fill_fx_names();
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    int slot = -1;
    for (int i = 0; i < 64 && slot < 0; i++) if (!all_plugins[i]) slot = i;
    if (slot < 0) { free(s); return NULL; }
    all_plugins[slot] = s;
    s->master = master;
    s->fs = 44100;
    for (int k = 0; k < NS; k++) {
        s->duo[k] = duo_entry(engine_host);
        s->amb[k] = amb_entry(engine_host);
        if (!s->duo[k] || !s->amb[k]) { close_all(s); return NULL; }
        s->duo[k]->setParameter(s->duo[k], DUO_B_ALGO, 1.0f);   /* (each Duo FX engine runs slot A alone: B is Off) */
        s->byp_s[k] = 1;
    }
    s->nam = nam_entry(engine_host);
    if (!s->nam) { close_all(s); return NULL; }
    /* a starting board: drive, chorus, amp, delay, reverb */
    static const char *const START[NS] = {"DrivPdls", "Chorus", "Tape Echo", "Hall"};
    for (int k = 0; k < NS; k++) {
        int fx = FX_OFF;
        for (int f = 0; f < N_FX; f++) if (!strcmp(FX_NAMES[f], START[k])) fx = f;
        s->norm[SLOT(k, S_FX)] = (float)fx / (float)(N_FX - 1);
        s->kind[k] = -1;
        choose_fx(s, k, fx);
    }
    s->norm[kAmpPos] = 2.0f / 4;   /* after FX 2 */
    s->norm[kOutput] = 24.0f / 36;
    for (int i = 0; i < NAM_N; i++) s->norm[kNam0 + i] = s->nam->getParameter(s->nam, i);
    memset((void *)s->automate, 0, sizeof s->automate);
    AEffect *e = &s->fx;
    e->magic = VST_MAGIC;
    e->dispatcher = dispatcher;
    e->process = process;
    e->processReplacing = process;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->numPrograms = 1;
    e->numParams = kNumParams;
    e->numInputs = e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
    e->object = s;
    e->uniqueID = 0x51524947;   /* 'QRIG' */
    e->version = 1000;
    return e;
}
__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) { return pl_wrap(master, make, &LISTS); }

#ifdef QR_PARAMS
/* the page's parameter list (params.json), with the effect names from the engines themselves
 *   (built with the engines, and nam_entry from the real NAM MPC or the stub below) */
static const char *const NAM_KEYS[NAM_N] = {"input", "gate", "bass", "middle", "treble", "output", "model", "model_prev",
    "model_next", "cab", "ir", "ir_prev", "ir_next", "normalize", "size", "folder", "folder_prev", "folder_next",
    "ir_folder", "ir_folder_prev", "ir_folder_next"};
int main(int argc, char **argv) {
    /* NAM's own params.json supplies the amp's entries (argv[1]); ours go around them */
    fill_fx_names();
    printf("{\n  \"name\": \"Quad Rig\",\n  \"params\": [\n");
    printf("    {\"key\": \"amp_pos\", \"name\": \"Amp Position\", \"options\": [");
    for (int k = 0; k < 5; k++) printf("%s\"%s\"", k ? ", " : "", AMP_POS[k]);
    printf("], \"default\": 2},\n");
    printf("    {\"key\": \"amp_bypass\", \"name\": \"Amp Bypass\", \"options\": [\"On\", \"Bypassed\"]},\n");
    printf("    {\"key\": \"output\", \"name\": \"Output\", \"min\": -24, \"max\": 12, \"default\": 0, \"unit\": \"dB\"},\n");
    for (int k = 0; k < NS; k++) {
        printf("    {\"key\": \"f%d_fx\", \"name\": \"FX%d Effect\", \"options\": [", k + 1, k + 1);
        for (int f = 0; f < N_FX; f++) printf("%s\"%s\"", f ? ", " : "", FX_NAMES[f]);
        printf("]},\n");
        printf("    {\"key\": \"f%d_fx__open\", \"name\": \"FX%d Effect List\", \"options\": [\"Closed\", \"Open\"]},\n", k + 1, k + 1);
        printf("    {\"key\": \"f%d_mix\", \"name\": \"FX%d Mix\", \"min\": 0, \"max\": 100, \"unit\": \"%%\"},\n", k + 1, k + 1);
        for (int i = 0; i < 8; i++) printf("    {\"key\": \"f%d_k%d\", \"name\": \"FX%d Knob %d\", \"min\": 0, \"max\": 100},\n", k + 1, i + 1, k + 1, i + 1);
        printf("    {\"key\": \"f%d_preset\", \"name\": \"FX%d Preset\", \"min\": 0, \"max\": 1, \"display\": \"string\", \"type\": \"stepper\"},\n", k + 1, k + 1);
        printf("    {\"key\": \"f%d_preset_prev\", \"name\": \"FX%d Preset <\", \"options\": [\"Off\", \"On\"], \"momentary\": true},\n", k + 1, k + 1);
        printf("    {\"key\": \"f%d_preset_next\", \"name\": \"FX%d Preset >\", \"options\": [\"Off\", \"On\"], \"momentary\": true},\n", k + 1, k + 1);
        printf("    {\"key\": \"f%d_bypass\", \"name\": \"FX%d Bypass\", \"options\": [\"On\", \"Bypassed\"]},\n", k + 1, k + 1);
        printf("    {\"key\": \"f%d_hold\", \"name\": \"FX%d Hold\", \"options\": [\"Off\", \"Held\"]},\n", k + 1, k + 1);
    }
    /* the amp's: NAM MPC's own entries with "amp_" in front of their keys */
    FILE *f = argc > 1 ? fopen(argv[1], "r") : NULL;
    if (!f) { fprintf(stderr, "need NAM MPC's params.json\n"); return 1; }
    static char buf[1 << 16];
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    char *pp = strstr(buf, "\"params\"");
    pp = pp ? strchr(pp, '[') : NULL;
    if (!pp) return 1;
    int count = 0;
    for (char *q = pp + 1; *q; q++) {
        if (*q == '{') {
            char *end = strchr(q, '}');
            if (!end) return 1;
            *end = 0;
            char *key = strstr(q, "\"key\"");
            char *colon = key ? strchr(key, ':') : NULL, *q1 = colon ? strchr(colon, '"') : NULL, *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q2) return 1;
            /* print the object with its key renamed */
            printf("    {");
            fwrite(q + 1, 1, (size_t)(q1 + 1 - (q + 1)), stdout);
            printf("amp_");
            fputs(q1 + 1, stdout);
            printf("}%s\n", count < NAM_N - 1 ? "," : "");
            if (strncmp(q1 + 1, NAM_KEYS[count], strlen(NAM_KEYS[count]))) { fprintf(stderr, "NAM's params.json order changed\n"); return 1; }
            count++;
            q = end;
        }
    }
    pl_params_json(&LISTS, stdout);
    printf("\n  ]\n}\n");
    return count == NAM_N ? 0 : 1;
}
#endif
