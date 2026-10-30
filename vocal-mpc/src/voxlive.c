/* VoxLive 3X: a vocal harmonizer for MPC OS (VST2, stereo), in the spirit of the big vocal-harmony floor units.
 * Four harmony voices from your own voice (TD-PSOLA, formants kept: vocal_core.h), each an interval in a key and
 * scale (or the chord / notes held on MIDI), with its own level, pan and Gender (formant shift); a lead pitch
 * correction (amount, speed), a doubler, Humanize (each voice drifts a little in pitch, time and level, like
 * real singers) and 21 styles (presets: High, Higher, Low, Trio, Choir, ...). Our own code.
 * Modes: Scale (the key and scale you set), MIDI Chord (the voices take the held chord's tones above / below you),
 * MIDI Notes (voice n sings the n-th held note, lowest first). With nothing held, the MIDI modes fall back to Scale.
 * MIDI Program Change n picks style n+1. */
#include "vocal_core.h"

static const char *const MODES[] = {"Scale", "MIDI Chord", "MIDI Notes"};
static const char *const OPEN[] = {"Closed", "Open"};
#define NV 4
enum {
    kStyle, kStylePrev, kStyleNext, kMode, kKey, kScale, kTune, kTuneSpeed, kLead, kHarm, kDouble, kHumanize,
    kV0, /* per voice: interval, its list's open flag, level, pan, gender */
    kOutput = kV0 + 5 * NV, kNumParams
};
#define VP(v, k) (kV0 + 5 * (v) + (k))
static const ParamDef P[kNumParams] = {
    PRESET("style", "Style"), BUTTON("style_prev", "Style <"), BUTTON("style_next", "Style >"),
    OPT("mode", "Mode", MODES, 0), OPT("key", "Key", KEYS, 0), OPT("scale", "Scale", SCALES, 0),
    PCT("tune", "Lead Tune", 0), EXPP("tune_speed", "Tune Speed", 1, 200, 20, "ms"),
    PCT("lead", "Lead", 100), PCT("harmony", "Harmony", 70), PCT("double", "Double", 0), PCT("humanize", "Humanize", 30),
#define VOICE(n) OPT("v" #n "_int", "Voice " #n, INTERVALS, 0), OPT("v" #n "_int__open", "Voice " #n " List", OPEN, 0), \
                 PCT("v" #n "_level", "V" #n " Level", 80), LIN("v" #n "_pan", "V" #n " Pan", -100, 100, 0, ""),       \
                 LIN("v" #n "_gender", "V" #n " Gender", -50, 50, 0, "")
    VOICE(1), VOICE(2), VOICE(3), VOICE(4),
    LIN("output", "Output", -24, 12, 0, "dB"),
};

/* styles: intervals (INTERVALS index), levels, pans, genders, humanize, double */
typedef struct { const char *name; unsigned char iv[NV]; float lvl[NV], pan[NV], gen[NV], hum, dbl; } Style;
enum { I_OFF, I_OCTD, I_6D, I_5D, I_4D, I_3D, I_UNI, I_3U, I_4U, I_5U, I_6U, I_OCTU, I_O3U, I_O5U };
static const Style STYLES[] = {
    {"Natural Double", {0, 0, 0, 0}, {80, 80, 80, 80}, {0, 0, 0, 0}, {0, 0, 0, 0}, 30, 60},
    {"High", {I_3U, 0, 0, 0}, {80, 80, 80, 80}, {20, 0, 0, 0}, {0, 0, 0, 0}, 30, 0},
    {"Higher", {I_5U, 0, 0, 0}, {80, 80, 80, 80}, {-20, 0, 0, 0}, {0, 0, 0, 0}, 30, 0},
    {"Low", {I_3D, 0, 0, 0}, {80, 80, 80, 80}, {20, 0, 0, 0}, {0, 0, 0, 0}, 30, 0},
    {"Lower", {I_4D, 0, 0, 0}, {80, 80, 80, 80}, {-20, 0, 0, 0}, {0, 0, 0, 0}, 30, 0},
    {"High + Higher", {I_3U, I_5U, 0, 0}, {80, 70, 80, 80}, {-40, 40, 0, 0}, {0, 0, 0, 0}, 35, 0},
    {"High + Low", {I_3U, I_4D, 0, 0}, {80, 75, 80, 80}, {-40, 40, 0, 0}, {0, 0, 0, 0}, 35, 0},
    {"Low + Lower", {I_3D, I_5D, 0, 0}, {80, 70, 80, 80}, {-40, 40, 0, 0}, {0, 0, 0, 0}, 35, 0},
    {"Trio", {I_3U, I_5U, I_4D, 0}, {75, 65, 70, 80}, {-50, 50, 0, 0}, {0, 0, 0, 0}, 40, 0},
    {"Quartet", {I_3U, I_5U, I_OCTU, I_4D}, {75, 65, 50, 70}, {-60, 60, -25, 25}, {0, 0, 0, 0}, 40, 0},
    {"Choir", {I_3U, I_5U, I_3D, I_OCTD}, {70, 60, 65, 55}, {-70, 70, -35, 35}, {5, 0, -5, 0}, 75, 30},
    {"Gospel", {I_3U, I_5U, I_6U, I_3D}, {75, 65, 55, 65}, {-60, 60, -20, 20}, {0, 5, 0, -5}, 55, 0},
    {"Barbershop", {I_3U, I_5U, I_3D, I_6D}, {70, 65, 70, 60}, {-45, 45, -15, 15}, {0, 0, -10, -20}, 30, 0},
    {"Fifths", {I_5U, I_5D, 0, 0}, {75, 70, 80, 80}, {-45, 45, 0, 0}, {0, 0, 0, 0}, 25, 0},
    {"Octaves", {I_OCTU, I_OCTD, 0, 0}, {60, 70, 80, 80}, {-35, 35, 0, 0}, {0, 0, 0, 0}, 20, 0},
    {"Octave Up", {I_OCTU, 0, 0, 0}, {65, 80, 80, 80}, {0, 0, 0, 0}, {0, 0, 0, 0}, 15, 0},
    {"Octave Down", {I_OCTD, 0, 0, 0}, {75, 80, 80, 80}, {0, 0, 0, 0}, {0, 0, 0, 0}, 15, 0},
    {"Unison Thick", {I_UNI, I_UNI, I_UNI, I_UNI}, {55, 55, 50, 50}, {-80, 80, -40, 40}, {4, -4, 8, -8}, 80, 0},
    {"Girl Group", {I_3U, I_5U, I_OCTU, 0}, {75, 65, 50, 80}, {-50, 50, 0, 0}, {30, 30, 35, 0}, 45, 0},
    {"Deep Men", {I_3D, I_5D, I_OCTD, 0}, {75, 70, 60, 80}, {-50, 50, 0, 0}, {-30, -30, -35, 0}, 45, 0},
    {"Wide Stack", {I_3U, I_5U, I_O3U, I_OCTD}, {70, 60, 45, 60}, {-80, 80, -50, 50}, {0, 0, 10, -10}, 50, 40},
};
#define NSTYLES ((int)(sizeof STYLES / sizeof *STYLES))

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    int ready, style;
    volatile int update_display;
    volatile uint32_t automate;
    VoiceIn vin;
    Voice voice[NV + 2];            /* four harmonies, then the two doubles */
    Voice lead;                     /* the tuned lead */
    Line late[NV + 2];              /* Humanize / double timing */
    Tracker trk;
    Held held;
    float ratio_s[NV], gain_s[NV], corr_s, hum_p[NV + 2], hum_t[NV + 2], out_s;
    float want_r[NV], want_gn[NV], last_hz, last_conf;   /* the voices' targets, worked out when the tracker moves */
    int last_cfg;
    float corr_target;
    int64_t tick;
} Plugin;

static void reset(Plugin *s) {
    for (int v = 0; v < NV + 2; v++) { voice_reset(&s->voice[v], &s->vin); line_clear(&s->late[v]); }
    voice_reset(&s->lead, &s->vin);
    memset(&s->trk, 0, sizeof s->trk);
    for (int v = 0; v < NV; v++) { s->ratio_s[v] = 1; s->gain_s[v] = 0; }
    s->corr_s = 0;
}
static int setup(Plugin *s, float fs) {
    s->ready = 0;
    s->fs = fs;
    int ok = voicein_init(&s->vin, fs);
    for (int v = 0; v < NV + 2; v++) ok &= line_init(&s->late[v], (int)(fs * 0.06f));
    reset(s);
    s->ready = ok;
    return ok;
}
static void load_style(Plugin *s, int idx) {
    const Style *st = &STYLES[idx];
    for (int v = 0; v < NV; v++) {
        s->norm[VP(v, 0)] = (float)st->iv[v] / (N_INTERVALS - 1);
        s->norm[VP(v, 2)] = pn(&P[VP(v, 2)], st->lvl[v]);
        s->norm[VP(v, 3)] = pn(&P[VP(v, 3)], st->pan[v]);
        s->norm[VP(v, 4)] = pn(&P[VP(v, 4)], st->gen[v]);
        for (int k = 0; k < 5; k++) if (k != 1) s->automate |= 1u << (VP(v, k) - kV0);
    }
    s->norm[kHumanize] = st->hum / 100;
    s->norm[kDouble] = st->dbl / 100;
    s->style = idx;
    s->norm[kStyle] = (float)idx / (float)(NSTYLES - 1);
    s->update_display = 1;
}

static void process(AEffect *e, float **in, float **out, int32_t n) {
    VC_FLUSH_DENORMALS_BEGIN;
    Plugin *s = (Plugin *)e->object;
    if (s->master) {
        const uint32_t a = s->automate;
        if (a) {
            s->automate &= ~a;
            for (int i = 0; i < 32; i++) if (a & (1u << i)) s->master(e, audioMasterAutomate, kV0 + i, 0, NULL, s->norm[kV0 + i]);
            s->master(e, audioMasterAutomate, kHumanize, 0, NULL, s->norm[kHumanize]);
            s->master(e, audioMasterAutomate, kDouble, 0, NULL, s->norm[kDouble]);
            s->master(e, audioMasterAutomate, kStyle, 0, NULL, s->norm[kStyle]);
            s->update_display = 1;
        }
        if (s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    }
    const float *il = in && in[0] ? in[0] : NULL, *ir = in && in[1] ? in[1] : il;
    if (!s->ready) { for (int k = 0; k < n; k++) out[0][k] = out[1][k] = 0; goto done; }
    const float fs = s->fs, *p = s->norm;
    const int mode = (int)pv(&P[kMode], p[kMode]), key = (int)pv(&P[kKey], p[kKey]), scale = (int)pv(&P[kScale], p[kScale]);
    const float tune = p[kTune], speed = expf(-1 / (pv(&P[kTuneSpeed], p[kTuneSpeed]) * 0.001f * fs));
    const float lead_g = p[kLead], harm_g = p[kHarm], dbl = p[kDouble], hum = p[kHumanize];
    const float og = db(pv(&P[kOutput], p[kOutput]));
    float lvl[NV], panl[NV], panr[NV], form[NV];
    int iv[NV];
    for (int v = 0; v < NV; v++) {
        iv[v] = (int)pv(&P[VP(v, 0)], p[VP(v, 0)]);
        lvl[v] = p[VP(v, 2)];
        const float pan = pv(&P[VP(v, 3)], p[VP(v, 3)]) * 0.01f;
        panl[v] = cosf((pan + 1) * PI / 4) * 1.4142f; panr[v] = sinf((pan + 1) * PI / 4) * 1.4142f;
        form[v] = powf(2, pv(&P[VP(v, 4)], p[VP(v, 4)]) / 50 * 0.5f);
    }
    int notes[16], pcs[12];
    const int nnotes = mode == 2 ? held_notes(&s->held, notes, 16) : 0, npc = mode == 1 ? held_pcs(&s->held, pcs) : 0;
    const float glide = expf(-1 / (0.012f * fs)), gain_a = 1 - expf(-1 / (0.012f * fs));
    for (int k = 0; k < n; k++) {
        float l = il ? il[k] : 0, r = ir ? ir[k] : 0;
        if (!finite_bits(l) || l > 8 || l < -8) l = 0;   /* only clean audio in */
        if (!finite_bits(r) || r > 8 || r < -8) r = 0;
        const float x = (l + r) * 0.5f;
        voicein_push(&s->vin, x);
        track(&s->trk, x, fs);
        const int voiced = s->trk.conf > 0.75f && s->trk.hz > 70 && s->trk.hz < 1100;
        const float t0 = voiced ? fs / s->trk.hz : fs / 150, m = voiced ? midi_of(s->trk.hz) : 60;
        const int cfg = mode * 1000 + key * 50 + scale + (iv[0] << 12) + (iv[1] << 16) + (iv[2] << 20) + (iv[3] << 24) + s->held.n * 7;
        if (s->trk.hz != s->last_hz || s->trk.conf != s->last_conf || cfg != s->last_cfg) {
            s->last_hz = s->trk.hz; s->last_conf = s->trk.conf; s->last_cfg = cfg;
            const float q = nearest_in_scale(m, key, scale), base = mode == 1 && npc ? roundf(m) : q;
            s->corr_target = voiced ? q - m : 0;
            for (int v = 0; v < NV; v++) {
                s->want_r[v] = 1; s->want_gn[v] = 0;
                if (iv[v] <= 0 || !voiced) continue;
                if (mode == 2 && nnotes > 0) {
                    if (v < nnotes) { s->want_r[v] = powf(2, ((float)notes[v] - m) / 12); s->want_gn[v] = 1; }
                } else {
                    s->want_r[v] = powf(2, (harmony_note(base, key, scale, iv[v], pcs, npc) - base) / 12);
                    s->want_gn[v] = 1;
                }
            }
        }
        /* the lead: pulled towards the scale note by Lead Tune, at Tune Speed */
        float lead_l, lead_r;
        if (tune > 0.001f) {
            const float want = s->corr_target * tune;
            s->corr_s = want + (s->corr_s - want) * speed;
            const float y = voice_run(&s->lead, &s->vin, t0, exp2f(s->corr_s / 12), 1);
            lead_l = lead_r = y;
        } else { lead_l = l; lead_r = r; }
        float wl = 0, wr = 0;
        for (int v = 0; v < NV; v++) {
            const float ratio = s->want_r[v], want_g = s->want_gn[v];
            if (want_g > 0) s->ratio_s[v] = ratio + (s->ratio_s[v] - ratio) * glide;
            s->gain_s[v] += gain_a * (want_g - s->gain_s[v]);
            if (want_g <= 0 && s->gain_s[v] < 1e-4f && iv[v] <= 0) continue;   /* (an Off voice costs nothing) */
            /* Humanize: a slow random drift in pitch (up to 15 cents), level and timing (up to 25 ms late) */
            if ((s->tick & 255) == 0) { s->hum_t[v] = (rnd() * 2 - 1); }
            s->hum_p[v] += 0.0004f * (s->hum_t[v] - s->hum_p[v]);
            const float cents = hum * 15 * s->hum_p[v];
            const float y = voice_run(&s->voice[v], &s->vin, t0, s->ratio_s[v] * (1 + cents * 0.000577623f), form[v]);
            line_push(&s->late[v], y);
            const float yd = line_read(&s->late[v], hum * (0.004f + 0.006f * (float)v + 0.003f * s->hum_p[v]) * fs);
            const float g = s->gain_s[v] * lvl[v] * (1 + 0.15f * hum * s->hum_p[(v + 1) % NV]);
            wl += yd * g * panl[v];
            wr += yd * g * panr[v];
        }
        if (dbl > 0.001f) {   /* the doubles: you, a few cents off and a little late, left and right */
            for (int d = 0; d < 2; d++) {
                const int v = NV + d;
                if ((s->tick & 511) == 0) s->hum_t[v] = rnd() * 2 - 1;
                s->hum_p[v] += 0.0003f * (s->hum_t[v] - s->hum_p[v]);
                const float y = voice_run(&s->voice[v], &s->vin, t0, 1 + ((d ? -7.0f : 6.0f) + 4 * s->hum_p[v]) * 0.000577623f, 1);
                line_push(&s->late[v], y);
                const float yd = line_read(&s->late[v], (d ? 0.019f : 0.013f) * fs) * dbl * 0.8f;
                if (d) wr += yd; else wl += yd;
            }
        }
        s->tick++;
        s->out_s += 0.002f * (og - s->out_s);
        float yl = (lead_l * lead_g + wl * harm_g * 0.8f) * s->out_s, yr = (lead_r * lead_g + wr * harm_g * 0.8f) * s->out_s;
        if (!finite_bits(yl) || !finite_bits(yr) || fabsf(yl) > 63 || fabsf(yr) > 63) { reset(s); yl = yr = 0; }   /* past +36 dB: start over */
        out[0][k] = clampf(yl, -4, 4);   /* +12 dB ceiling */
        out[1][k] = clampf(yr, -4, 4);
    }
done:
    VC_FLUSH_DENORMALS_END;
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams) return;
    v = clampf(v, 0, 1);
    if (i == kStylePrev || i == kStyleNext) {
        if (v > 0.5f) load_style(s, s->style < 0 ? (i == kStyleNext ? 0 : NSTYLES - 1) : (s->style + (i == kStyleNext ? 1 : -1) + NSTYLES) % NSTYLES);
        s->norm[i] = 0;
        return;
    }
    if (i == kStyle) {   /* a small move (a Q-Link nudge) steps one; an exact value (a project loading) only names it */
        const float pos = v * (float)(NSTYLES - 1), cur = s->style >= 0 ? (float)s->style : s->norm[i] * (float)(NSTYLES - 1);
        if (fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1) { load_style(s, ((int)lrintf(cur) + (pos > cur ? 1 : -1) + NSTYLES) % NSTYLES); return; }
        s->style = (int)lrintf(pos);
        s->norm[i] = v;
        s->update_display = 1;
        return;
    }
    const float nv = snap(&P[i], v, s->norm[i]);
    if (i >= kV0 && i < kOutput && (i - kV0) % 5 == 0 && P[i + 1].kind == K_OPT && s->norm[i + 1] > 0.5f && fabsf(nv - v) < 1e-4f) {
        s->norm[i + 1] = 0;   /* a pick from the list closes it */
        s->automate |= 1u << (i + 1 - kV0);
    }
    s->norm[i] = nv;
    if (P[i].kind == K_OPT) s->update_display = 1;
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: line_free(&s->vin.l); for (int v = 0; v < NV + 2; v++) line_free(&s->late[v]); free(s); return 1;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) copy_str(p, P[idx].name, 32); return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay:
        if (idx >= 0 && idx < kNumParams) {
            char t[32];
            if (idx == kStyle) snprintf(t, sizeof t, "%s", s->style >= 0 && s->style < NSTYLES ? STYLES[s->style].name : "-");
            else if (idx == kStylePrev || idx == kStyleNext) snprintf(t, sizeof t, "%s", idx == kStylePrev ? "<" : ">");
            else pdisplay(&P[idx], s->norm[idx], t);
            copy_str(p, t, 24);
        }
        return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "VoxLive 3X", 32); return 1;
    case effGetVendorString: copy_str(p, VC_VENDOR, 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate: if (opt > 1000 && fabsf(opt - s->fs) > 0.5f) setup(s, opt); return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (val) reset(s); return 1;
    case effProcessEvents: {
        const VstEvents *ev = (const VstEvents *)p;
        for (int k = 0; ev && k < ev->numEvents; k++) {
            if (!ev->events[k] || ev->events[k]->type != 1) continue;
            const unsigned char *m = ev->events[k]->midiData;
            if ((m[0] & 0xf0) == 0xc0) { if (m[1] < NSTYLES) load_style(s, m[1]); }
            else held_midi(&s->held, m);
        }
        return 1;
    }
    case effCanDo:
        return p && (!strcmp((const char *)p, "receiveVstEvents") || !strcmp((const char *)p, "receiveVstMidiEvent")) ? 1 : -1;
    default: return 0;
    }
}

/* the full-page list of the styles (tools/picklist.h) */
static int vox_pick(AEffect *e, int list, int op, int i, char *out, int max) {
    Plugin *s = (Plugin *)e->object;
    (void)list;
    if (op == PL_COUNT) return NSTYLES;
    if (op == PL_CURRENT) return s->style;
    if (i < 0 || i >= NSTYLES) return 0;
    if (op == PL_NAME) snprintf(out, (size_t)max, "%s", STYLES[i].name);
    else if (op == PL_CHOOSE) load_style(s, i);
    return 0;
}
static const char *const LIST_KEYS[] = {"style"};
static const char *const LIST_TITLES[] = {"Styles"};
static const PickLists LISTS = {1, LIST_KEYS, LIST_TITLES, vox_pick};

static AEffect *make(audioMasterCallback master) {
    hann_init();
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    for (int i = 0; i < kNumParams; i++) s->norm[i] = P[i].kind == K_PRESET || P[i].kind == K_BUTTON ? 0 : pn(&P[i], P[i].def);
    s->style = -1;
    load_style(s, 1);   /* High */
    s->automate = 0;
    s->out_s = 1;
    setup(s, 44100);
    AEffect *e = &s->fx;
    e->magic = VST_MAGIC;
    e->dispatcher = dispatcher;
    e->process = process;
    e->processReplacing = process;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->numParams = kNumParams;
    e->numInputs = e->numOutputs = 2;
    e->flags = effFlagsCanReplacing;
    e->object = s;
    e->uniqueID = 0x564c5833;   /* 'VLX3' */
    e->version = 1000;
    return e;
}

__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
#ifdef PICKLIST_OFF
    return make(master);
#else
    return pl_wrap(master, make, &LISTS);
#endif
}

#ifdef VC_PARAMS
int main(void) { write_params_json("VoxLive 3X", P, kNumParams, &LISTS); return 0; }
#endif
