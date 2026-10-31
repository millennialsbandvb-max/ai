/* Vocal Rack: a vocal channel strip and effects for MPC OS (VST2, stereo), in the spirit of a vocal processor's
 * chain: Gate -> EQ -> De-Ess -> Comp -> Tune (and the pitch effects) -> FX -> Double -> Delay -> Reverb.
 * Each block switches on and off; 17 presets (Clean Pop, Hard Tune, Rap, Stadium, Megaphone, Robot, ...).
 * Pitch (Tune, and the FX Robot, Octave Down, Chipmunk) is one TD-PSOLA voice (vocal_core.h): formants kept, about
 * 30 ms late; with neither on, the voice isn't delayed at all. Our own code.
 * FX: Megaphone, Telephone, Radio (band-limited and pushed), Distortion, Robot (one note: the Key's, octave 3),
 * Octave Down (deeper formants too), Chipmunk (up an octave, formants up). */
#include "vocal_core.h"

static const char *const FXS[] = {"Off", "Megaphone", "Telephone", "Radio", "Distortion", "Robot", "Octave Down", "Chipmunk"};
enum { FX_OFF, FX_MEGA, FX_PHONE, FX_RADIO, FX_DIST, FX_ROBOT, FX_OCTDN, FX_CHIP };
enum {
    kPreset, kPresetPrev, kPresetNext,
    kGate, kGateThr,
    kEq, kLowCut, kBody, kPresence, kAir,
    kDeEss, kDeEssAmt, kDeEssFreq,
    kComp, kCompThr, kRatio,
    kTune, kKey, kScale, kTuneAmt, kTuneSpeed,
    kFx, kFxAmt,
    kDouble, kDoubleAmt,
    kDelay, kDelayTime, kDelayFb, kDelayMix,
    kReverb, kRevDecay, kRevMix, kRevTone,
    kOutput, kNumParams
};
static const ParamDef P[kNumParams] = {
    PRESET("preset", "Preset"), BUTTON("preset_prev", "Preset <"), BUTTON("preset_next", "Preset >"),
    TOGGLE("gate", "Gate", 0), LIN("gate_thr", "Gate Thresh", -80, -20, -55, "dB"),
    TOGGLE("eq", "EQ", 1), EXPP("low_cut", "Low Cut", 20, 400, 80, "Hz"), LIN("body", "Body", -12, 12, 0, "dB"),
    LIN("presence", "Presence", -12, 12, 2, "dB"), LIN("air", "Air", -12, 12, 2, "dB"),
    TOGGLE("deess", "De-Ess", 0), PCT("deess_amt", "De-Ess Amount", 50), EXPP("deess_freq", "De-Ess Freq", 3000, 10000, 6000, "Hz"),
    TOGGLE("comp", "Comp", 1), LIN("comp_thr", "Comp Thresh", -50, 0, -20, "dB"), LIN("ratio", "Ratio", 1, 20, 3, ":1"),
    TOGGLE("tune", "Tune", 0), OPT("key", "Key", KEYS, 0), OPT("scale", "Scale", SCALES, 0), PCT("tune_amt", "Tune Amount", 70),
    EXPP("tune_speed", "Tune Speed", 1, 200, 25, "ms"),
    OPT("fx", "FX", FXS, 0), PCT("fx_amt", "FX Amount", 100),
    TOGGLE("double", "Double", 0), PCT("double_amt", "Double Amount", 50),
    TOGGLE("delay", "Delay", 0), EXPP("delay_time", "Delay Time", 50, 1500, 375, "ms"), PCT("delay_fb", "Delay Feedback", 25),
    PCT("delay_mix", "Delay Mix", 18),
    TOGGLE("reverb", "Reverb", 1), EXPP("rev_decay", "Reverb Decay", 0.3f, 8, 1.6f, "s"), PCT("rev_mix", "Reverb Mix", 15),
    PCT("rev_tone", "Reverb Tone", 60),
    LIN("output", "Output", -24, 12, 0, "dB"),
};

/* presets: the defaults, then these settings (in each parameter's own units) */
typedef struct { const char *name; struct { int p; float v; } set[16]; } Preset;
#define END {-1, 0}
static const Preset PRESETS[] = {
    {"Clean Pop", {{kGate, 1}, {kTune, 1}, {kTuneAmt, 50}, {kTuneSpeed, 30}, {kPresence, 3}, {kAir, 3}, {kDeEss, 1}, {kRevMix, 18}, END}},
    {"Hard Tune", {{kTune, 1}, {kTuneAmt, 100}, {kTuneSpeed, 1}, {kPresence, 3}, {kAir, 4}, {kDelay, 1}, {kDelayTime, 250}, {kDelayMix, 12}, {kRevMix, 15}, END}},
    {"Rock Lead", {{kCompThr, -24}, {kRatio, 4}, {kPresence, 4}, {kDelay, 1}, {kDelayTime, 380}, {kDelayFb, 25}, {kDelayMix, 15}, {kRevDecay, 2.2f}, {kRevMix, 15}, END}},
    {"Rap Vocal", {{kGate, 1}, {kLowCut, 120}, {kCompThr, -26}, {kRatio, 5}, {kPresence, 3}, {kDeEss, 1}, {kDeEssAmt, 60}, {kDouble, 1}, {kDoubleAmt, 30}, {kRevDecay, 0.8f}, {kRevMix, 10}, END}},
    {"Ballad", {{kTune, 1}, {kTuneAmt, 40}, {kTuneSpeed, 60}, {kRatio, 2.5f}, {kDelay, 1}, {kDelayTime, 450}, {kDelayFb, 30}, {kDelayMix, 15}, {kRevDecay, 3.2f}, {kRevMix, 28}, END}},
    {"Stadium", {{kDouble, 1}, {kDoubleAmt, 40}, {kDelay, 1}, {kDelayTime, 500}, {kDelayFb, 40}, {kDelayMix, 22}, {kRevDecay, 4.5f}, {kRevMix, 30}, END}},
    {"Doubled", {{kDouble, 1}, {kDoubleAmt, 70}, {kRevMix, 12}, END}},
    {"Slapback", {{kDelay, 1}, {kDelayTime, 110}, {kDelayFb, 5}, {kDelayMix, 25}, {kRevDecay, 0.9f}, {kRevMix, 10}, END}},
    {"Whisper Air", {{kAir, 6}, {kPresence, 2}, {kDeEss, 1}, {kDeEssAmt, 70}, {kRevDecay, 2.5f}, {kRevMix, 22}, END}},
    {"Dry Natural", {{kPresence, 0}, {kAir, 0}, {kRatio, 2}, {kReverb, 0}, END}},
    {"Megaphone", {{kFx, FX_MEGA}, {kRevMix, 8}, END}},
    {"Telephone", {{kFx, FX_PHONE}, {kReverb, 0}, END}},
    {"Radio", {{kFx, FX_RADIO}, {kRevMix, 6}, END}},
    {"Dirty", {{kFx, FX_DIST}, {kFxAmt, 60}, {kDelay, 1}, {kDelayTime, 300}, {kDelayMix, 15}, END}},
    {"Robot", {{kFx, FX_ROBOT}, {kRevMix, 12}, END}},
    {"Deep Voice", {{kFx, FX_OCTDN}, {kRevMix, 12}, END}},
    {"Chipmunk", {{kFx, FX_CHIP}, {kRevMix, 12}, END}},
};
#define NPRESETS ((int)(sizeof PRESETS / sizeof *PRESETS))

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    int ready, preset;
    volatile int update_display, automate_all;
    VoiceIn vin;
    Voice pitch, dbl[2];
    Line dbl_late[2], dly[2];
    Verb verb;
    Tracker trk;
    Bq eq[4], ds_bp, ds_hp, fx_bq[3];
    BqCo eqc[4], ds_bpc, ds_hpc, fx_c[3];
    float gate_env, gate_g, ds_env, full_env, comp_env, corr_s, dly_lp[2], dly_s, out_s, pitch_on_s;
    float dbl_p[2], dbl_t[2], noise_lp;
    int64_t tick;
    float co_key;   /* (the coefficient set's fingerprint: recomputed when a knob moves) */
} Plugin;

static void reset(Plugin *s) {
    voice_reset(&s->pitch, &s->vin);
    for (int d = 0; d < 2; d++) { voice_reset(&s->dbl[d], &s->vin); line_clear(&s->dbl_late[d]); line_clear(&s->dly[d]); s->dly_lp[d] = 0; }
    verb_clear(&s->verb);
    memset(&s->trk, 0, sizeof s->trk);
    memset(s->eq, 0, sizeof s->eq);
    memset(&s->ds_bp, 0, sizeof s->ds_bp); memset(&s->ds_hp, 0, sizeof s->ds_hp); memset(s->fx_bq, 0, sizeof s->fx_bq);
    s->gate_env = s->gate_g = s->ds_env = s->full_env = s->comp_env = s->corr_s = 0;
}
static int setup(Plugin *s, float fs) {
    s->ready = 0;
    s->fs = fs;
    int ok = voicein_init(&s->vin, fs) && verb_init(&s->verb, fs);
    for (int d = 0; d < 2; d++) { ok &= line_init(&s->dbl_late[d], (int)(fs * 0.05f)); ok &= line_init(&s->dly[d], (int)(fs * 1.6f)); }
    s->co_key = -1;
    reset(s);
    s->ready = ok;
    return ok;
}
static void load_preset(Plugin *s, int idx) {
    for (int i = kGate; i < kNumParams; i++) if (i != kOutput) s->norm[i] = pn(&P[i], P[i].def);
    for (int k = 0; k < 16 && PRESETS[idx].set[k].p >= 0; k++) s->norm[PRESETS[idx].set[k].p] = pn(&P[PRESETS[idx].set[k].p], PRESETS[idx].set[k].v);
    s->preset = idx;
    s->norm[kPreset] = (float)idx / (float)(NPRESETS - 1);
    s->automate_all = 1;
    s->update_display = 1;
}

static void coefficients(Plugin *s) {
    const float fs = s->fs, *p = s->norm;
    s->eqc[0] = bq_co(BQ_HP, pv(&P[kLowCut], p[kLowCut]), 0.707f, 0, fs);
    s->eqc[1] = bq_co(BQ_PEAK, 250, 0.9f, pv(&P[kBody], p[kBody]), fs);
    s->eqc[2] = bq_co(BQ_PEAK, 3500, 0.9f, pv(&P[kPresence], p[kPresence]), fs);
    s->eqc[3] = bq_co(BQ_HSHELF, 11000, 0.7f, pv(&P[kAir], p[kAir]), fs);
    const float dsf = pv(&P[kDeEssFreq], p[kDeEssFreq]);
    s->ds_bpc = bq_co(BQ_BP, dsf, 1.4f, 0, fs);
    s->ds_hpc = bq_co(BQ_HP, dsf * 0.7f, 0.707f, 0, fs);
    switch ((int)pv(&P[kFx], p[kFx])) {
    case FX_MEGA: s->fx_c[0] = bq_co(BQ_HP, 600, 0.9f, 0, fs); s->fx_c[1] = bq_co(BQ_LP, 3500, 0.9f, 0, fs); s->fx_c[2] = bq_co(BQ_PEAK, 1600, 1.5f, 8, fs); break;
    case FX_PHONE: s->fx_c[0] = bq_co(BQ_HP, 320, 0.8f, 0, fs); s->fx_c[1] = bq_co(BQ_LP, 3300, 0.8f, 0, fs); s->fx_c[2] = bq_co(BQ_PEAK, 1200, 1, 4, fs); break;
    case FX_RADIO: s->fx_c[0] = bq_co(BQ_HP, 500, 0.7f, 0, fs); s->fx_c[1] = bq_co(BQ_LP, 4500, 0.7f, 0, fs); s->fx_c[2] = bq_co(BQ_PEAK, 2000, 1, 3, fs); break;
    default: s->fx_c[0] = bq_co(BQ_LP, 7000, 0.7f, 0, fs); s->fx_c[1] = bq_co(BQ_HP, 90, 0.7f, 0, fs); s->fx_c[2] = bq_co(BQ_PEAK, 1000, 1, 0, fs); break;
    }
    const float dec = pv(&P[kRevDecay], p[kRevDecay]);
    verb_set(&s->verb, clampf(0.6f + dec * 0.25f, 0.6f, 2.0f), dec, expmap(2500, 14000, p[kRevTone]), fs);
}

static void process(AEffect *e, float **in, float **out, int32_t n) {
    VC_FLUSH_DENORMALS_BEGIN;
    Plugin *s = (Plugin *)e->object;
    if (s->master) {
        if (s->automate_all) {
            s->automate_all = 0;
            for (int i = kPreset; i < kNumParams; i++) if (P[i].kind != K_BUTTON) s->master(e, audioMasterAutomate, i, 0, NULL, s->norm[i]);
            s->update_display = 1;
        }
        if (s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    }
    const float *il = in && in[0] ? in[0] : NULL, *ir = in && in[1] ? in[1] : il;
    if (!s->ready) { for (int k = 0; k < n; k++) out[0][k] = out[1][k] = 0; goto done; }
    const float fs = s->fs, *p = s->norm;
    {   /* coefficients only when a knob moved */
        float key = 0;
        for (int i = kLowCut; i < kNumParams; i++) key += p[i] * (float)(i * 7 + 1);
        if (key != s->co_key) { s->co_key = key; coefficients(s); }
    }
    const int gate = p[kGate] > 0.5f, eq = p[kEq] > 0.5f, deess = p[kDeEss] > 0.5f, comp = p[kComp] > 0.5f, tune = p[kTune] > 0.5f;
    const int fx = (int)pv(&P[kFx], p[kFx]), dbl = p[kDouble] > 0.5f, delay = p[kDelay] > 0.5f, reverb = p[kReverb] > 0.5f;
    const int key = (int)pv(&P[kKey], p[kKey]), scale = (int)pv(&P[kScale], p[kScale]);
    const int pitch_fx = fx == FX_ROBOT || fx == FX_OCTDN || fx == FX_CHIP, pitched = tune || pitch_fx;
    const float gate_thr = db(pv(&P[kGateThr], p[kGateThr])), ds_amt = p[kDeEssAmt], comp_thr = pv(&P[kCompThr], p[kCompThr]);
    const float ratio = pv(&P[kRatio], p[kRatio]), makeup = db(-comp_thr * (1 - 1 / ratio) * 0.4f);
    const float tune_amt = p[kTuneAmt], speed = expf(-1 / (pv(&P[kTuneSpeed], p[kTuneSpeed]) * 0.001f * fs)), fx_amt = p[kFxAmt];
    const float dbl_amt = p[kDoubleAmt], dly_t = pv(&P[kDelayTime], p[kDelayTime]) * 0.001f * fs, dly_fb = p[kDelayFb] * 0.92f;
    const float dly_mix = p[kDelayMix], rev_mix = p[kRevMix], og = db(pv(&P[kOutput], p[kOutput]));
    const float a_fast = 1 - expf(-1 / (0.001f * fs)), a_rel = 1 - expf(-1 / (0.12f * fs)), a_ds = 1 - expf(-1 / (0.002f * fs));
    const float a_ds_rel = 1 - expf(-1 / (0.04f * fs)), a_c = 1 - expf(-1 / (0.005f * fs)), a_c_rel = 1 - expf(-1 / (0.1f * fs));
    const float a_gate = 1 - expf(-1 / (0.004f * fs)), dly_lp_a = lp_coef(5000, fs);
    for (int k = 0; k < n; k++) {
        float l = il ? il[k] : 0, r = ir ? ir[k] : 0;
        if (!finite_bits(l) || l > 8 || l < -8) l = 0;   /* only clean audio in */
        if (!finite_bits(r) || r > 8 || r < -8) r = 0;
        float x = (l + r) * 0.5f;
        /* gate: opens at the threshold, closes 6 dB under it (no chatter) */
        if (gate) {
            const float a = fabsf(x);
            s->gate_env += (a > s->gate_env ? a_fast : a_rel) * (a - s->gate_env);
            const float want = s->gate_env > gate_thr ? 1 : s->gate_env < gate_thr * 0.5f ? 0 : s->gate_g > 0.5f ? 1 : 0;
            s->gate_g += a_gate * (want - s->gate_g);
            x *= s->gate_g;
        }
        if (eq) for (int b = 0; b < 4; b++) x = bq(&s->eq[b], &s->eqc[b], x);
        /* de-esser: when the sibilant band gets loud next to the whole, the top end comes down */
        if (deess) {
            const float sib = fabsf(bq(&s->ds_bp, &s->ds_bpc, x)), all = fabsf(x);
            s->ds_env += (sib > s->ds_env ? a_ds : a_ds_rel) * (sib - s->ds_env);
            s->full_env += (all > s->full_env ? a_ds : a_ds_rel) * (all - s->full_env);
            const float k_thr = lerp(0.6f, 0.12f, ds_amt) * s->full_env;
            const float g = s->ds_env > k_thr && s->ds_env > 1e-5f ? clampf(k_thr / s->ds_env, 0.1f, 1) : 1;
            const float top = bq(&s->ds_hp, &s->ds_hpc, x);
            x -= top * (1 - g);
        }
        /* compressor (peak-ish detector, soft over the threshold), with some make-up */
        if (comp) {
            const float a = fabsf(x);
            s->comp_env += (a > s->comp_env ? a_c : a_c_rel) * (a - s->comp_env);
            const float lvl = 20 * log10f(s->comp_env + 1e-6f), over = lvl - comp_thr;
            const float gr = over > 0 ? over * (1 - 1 / ratio) : 0;
            x *= db(-gr) * makeup;
        }
        /* pitch: Tune and the pitch effects, one voice */
        voicein_push(&s->vin, x);
        float y = x;
        if (pitched || dbl) track(&s->trk, x, fs);
        const int voiced = s->trk.conf > 0.75f && s->trk.hz > 70 && s->trk.hz < 1100;
        const float t0 = voiced ? fs / s->trk.hz : fs / 150, m = voiced ? midi_of(s->trk.hz) : 60;
        if (pitched) {
            float semis = 0, form = 1;
            if (tune && voiced) {
                const float want = (nearest_in_scale(m, key, scale) - m) * tune_amt;
                s->corr_s = want + (s->corr_s - want) * speed;
            } else if (tune) s->corr_s *= speed;
            if (tune) semis = s->corr_s;
            if (fx == FX_ROBOT && voiced) semis = (float)(48 + key) - m;
            if (fx == FX_OCTDN) { semis += -12; form = 0.82f; }
            if (fx == FX_CHIP) { semis += 12; form = 1.35f; }
            const float py = voice_run(&s->pitch, &s->vin, t0, exp2f(semis / 12), form);
            /* the pitch effects mix by FX Amount; Tune alone is the whole voice */
            y = pitch_fx ? lerp(voicein_late(&s->vin), py, fx_amt) : py;
        }
        /* character effects */
        if (fx == FX_MEGA || fx == FX_PHONE || fx == FX_RADIO) {
            float w = y;
            for (int b = 0; b < 3; b++) w = bq(&s->fx_bq[b], &s->fx_c[b], w);
            const float drive = fx == FX_MEGA ? 6 : fx == FX_RADIO ? 3 : 2;
            w = tanh_fast(w * drive) / drive * (fx == FX_MEGA ? 2.2f : 1.6f);
            if (fx == FX_RADIO) { s->noise_lp += 0.2f * ((rnd() - 0.5f) - s->noise_lp); w += s->noise_lp * 0.006f; }
            y = lerp(y, w, fx_amt);
        } else if (fx == FX_DIST) {
            const float g = 1 + fx_amt * 25;
            float w = tanh_fast(y * g) / sqrtf(g) * 1.3f;
            w = bq(&s->fx_bq[1], &s->fx_c[1], bq(&s->fx_bq[0], &s->fx_c[0], w));
            y = w;
        }
        float yl = y, yr = y;
        /* doubler: two copies a few cents off, a little late, left and right */
        if (dbl) {
            for (int d = 0; d < 2; d++) {
                if ((s->tick & 511) == 0) s->dbl_t[d] = rnd() * 2 - 1;
                s->dbl_p[d] += 0.0003f * (s->dbl_t[d] - s->dbl_p[d]);
                const float dv = voice_run(&s->dbl[d], &s->vin, t0, 1 + ((d ? -9.0f : 8.0f) + 5 * s->dbl_p[d]) * 0.000577623f, 1);
                line_push(&s->dbl_late[d], dv);
                const float dd = line_read(&s->dbl_late[d], (d ? 0.021f : 0.014f) * fs) * dbl_amt * 0.9f;
                if (d) yr += dd; else yl += dd;
            }
        }
        s->tick++;
        /* delay: the two sides a little apart, feedback crossing over (saturating), darker each time round */
        if (delay) {
            s->dly_s += 0.0005f * (dly_t - s->dly_s);
            const float dl = line_read(&s->dly[0], s->dly_s), dr = line_read(&s->dly[1], s->dly_s * 1.02f);
            s->dly_lp[0] += dly_lp_a * (dl - s->dly_lp[0]);
            s->dly_lp[1] += dly_lp_a * (dr - s->dly_lp[1]);
            line_push(&s->dly[0], (yl + yr) * 0.5f + tanh_fast(s->dly_lp[1] * dly_fb));
            line_push(&s->dly[1], tanh_fast(s->dly_lp[0] * dly_fb));
            yl += dl * dly_mix * 1.4f; yr += dr * dly_mix * 1.4f;
        } else s->dly_s = dly_t;
        if (reverb) {
            float vl, vr;
            verb_run(&s->verb, yl, yr, &vl, &vr);
            yl += vl * rev_mix * 1.2f; yr += vr * rev_mix * 1.2f;
        }
        s->out_s += 0.002f * (og - s->out_s);
        yl *= s->out_s; yr *= s->out_s;
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
    if (i == kPresetPrev || i == kPresetNext) {
        if (v > 0.5f) load_preset(s, s->preset < 0 ? (i == kPresetNext ? 0 : NPRESETS - 1) : (s->preset + (i == kPresetNext ? 1 : -1) + NPRESETS) % NPRESETS);
        s->norm[i] = 0;
        return;
    }
    if (i == kPreset) {   /* a small move (a Q-Link nudge) steps one; an exact value (a project loading) only names it */
        const float pos = v * (float)(NPRESETS - 1), cur = s->preset >= 0 ? (float)s->preset : s->norm[i] * (float)(NPRESETS - 1);
        if (fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1) { load_preset(s, ((int)lrintf(cur) + (pos > cur ? 1 : -1) + NPRESETS) % NPRESETS); return; }
        s->preset = (int)lrintf(pos);
        s->norm[i] = v;
        s->update_display = 1;
        return;
    }
    s->norm[i] = snap(&P[i], v, s->norm[i]);
    if (P[i].kind == K_OPT || P[i].kind == K_TOGGLE) s->update_display = 1;
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        line_free(&s->vin.l);
        for (int d = 0; d < 2; d++) { line_free(&s->dbl_late[d]); line_free(&s->dly[d]); }
        verb_free(&s->verb);
        free(s);
        return 1;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) copy_str(p, P[idx].name, 32); return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay:
        if (idx >= 0 && idx < kNumParams) {
            char t[32];
            if (idx == kPreset) snprintf(t, sizeof t, "%s", s->preset >= 0 && s->preset < NPRESETS ? PRESETS[s->preset].name : "-");
            else if (idx == kPresetPrev || idx == kPresetNext) snprintf(t, sizeof t, "%s", idx == kPresetPrev ? "<" : ">");
            else if (idx == kRatio) snprintf(t, sizeof t, "%.1f:1", pv(&P[idx], s->norm[idx]));
            else pdisplay(&P[idx], s->norm[idx], t);
            copy_str(p, t, 24);
        }
        return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "Vocal Rack", 32); return 1;
    case effGetVendorString: copy_str(p, VC_VENDOR, 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate: if (opt > 1000 && fabsf(opt - s->fs) > 0.5f) setup(s, opt); return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (val) reset(s); return 1;
    case effProcessEvents: {   /* MIDI Program Change n: preset n+1 */
        const VstEvents *ev = (const VstEvents *)p;
        for (int k = 0; ev && k < ev->numEvents; k++)
            if (ev->events[k] && ev->events[k]->type == 1 && (ev->events[k]->midiData[0] & 0xf0) == 0xc0 && ev->events[k]->midiData[1] < NPRESETS)
                load_preset(s, ev->events[k]->midiData[1]);
        return 1;
    }
    case effCanDo:
        return p && (!strcmp((const char *)p, "receiveVstEvents") || !strcmp((const char *)p, "receiveVstMidiEvent")) ? 1 : -1;
    default: return 0;
    }
}

/* the full-page list of the presets (tools/picklist.h) */
static int rack_pick(AEffect *e, int list, int op, int i, char *out, int max) {
    Plugin *s = (Plugin *)e->object;
    (void)list;
    if (op == PL_COUNT) return NPRESETS;
    if (op == PL_CURRENT) return s->preset;
    if (i < 0 || i >= NPRESETS) return 0;
    if (op == PL_NAME) snprintf(out, (size_t)max, "%s", PRESETS[i].name);
    else if (op == PL_CHOOSE) load_preset(s, i);
    return 0;
}
static const char *const LIST_KEYS[] = {"preset"};
static const char *const LIST_TITLES[] = {"Presets"};
static const PickLists LISTS = {1, LIST_KEYS, LIST_TITLES, rack_pick};

static AEffect *make(audioMasterCallback master) {
    hann_init();
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    for (int i = 0; i < kNumParams; i++) s->norm[i] = P[i].kind == K_PRESET || P[i].kind == K_BUTTON ? 0 : pn(&P[i], P[i].def);
    s->preset = -1;
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
    e->uniqueID = 0x56524b31;   /* 'VRK1' */
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
int main(void) { write_params_json("Vocal Rack", P, kNumParams, &LISTS); return 0; }
#endif
