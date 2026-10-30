/* HedRush Harmony: a two-voice intelligent harmonizer for MPC OS (VST2, stereo), in the spirit of the harmonizer
 * block on the HeadRush-style floor processors: a key, a scale, two voices each an interval in that scale with its
 * own level and pan, a dry level and a Tone. Voices are TD-PSOLA (vocal_core.h), so a voice stays a voice; it
 * works on guitar too. Our own code. */
#include "vocal_core.h"

enum { kKey, kScale, kV1, kV1Level, kV1Pan, kV2, kV2Level, kV2Pan, kDry, kTone, kGlide, kOutput, kNumParams };
static const ParamDef P[kNumParams] = {
    OPT("key", "Key", KEYS, 0), OPT("scale", "Scale", SCALES, 0),
    OPT("v1", "Voice 1", INTERVALS, 7), PCT("v1_level", "V1 Level", 80), LIN("v1_pan", "V1 Pan", -100, 100, -40, ""),
    OPT("v2", "Voice 2", INTERVALS, 9), PCT("v2_level", "V2 Level", 0), LIN("v2_pan", "V2 Pan", -100, 100, 40, ""),
    PCT("dry", "Dry", 100), PCT("tone", "Tone", 70), EXPP("glide", "Glide", 1, 200, 15, "ms"),
    LIN("output", "Output", -24, 12, 0, "dB"),
};

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    int ready;
    volatile int update_display;
    VoiceIn vin;
    Voice voice[2];
    Tracker trk;
    OnePole tone[2][2];
    float ratio_s[2], gain_s[2], out_s;
} Plugin;

static void reset(Plugin *s) {
    for (int v = 0; v < 2; v++) { voice_reset(&s->voice[v], &s->vin); s->ratio_s[v] = 1; s->gain_s[v] = 0; }
    memset(&s->trk, 0, sizeof s->trk);
    memset(s->tone, 0, sizeof s->tone);
}
static int setup(Plugin *s, float fs) {
    s->ready = 0;
    s->fs = fs;
    const int ok = voicein_init(&s->vin, fs);
    reset(s);
    s->ready = ok;
    return ok;
}

static void process(AEffect *e, float **in, float **out, int32_t n) {
    VC_FLUSH_DENORMALS_BEGIN;
    Plugin *s = (Plugin *)e->object;
    if (s->update_display && s->master) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    const float *il = in && in[0] ? in[0] : NULL, *ir = in && in[1] ? in[1] : il;
    if (!s->ready) { for (int k = 0; k < n; k++) out[0][k] = out[1][k] = 0; goto done; }
    const float fs = s->fs, *p = s->norm;
    const int key = (int)pv(&P[kKey], p[kKey]), scale = (int)pv(&P[kScale], p[kScale]);
    const int iv[2] = {(int)pv(&P[kV1], p[kV1]), (int)pv(&P[kV2], p[kV2])};
    const float lvl[2] = {p[kV1Level], p[kV2Level]}, dry = p[kDry];
    float pl[2], pr[2];
    for (int v = 0; v < 2; v++) {
        const float pan = pv(&P[v ? kV2Pan : kV1Pan], p[v ? kV2Pan : kV1Pan]) * 0.01f;
        pl[v] = cosf((pan + 1) * PI / 4) * 1.4142f; pr[v] = sinf((pan + 1) * PI / 4) * 1.4142f;
    }
    const float tone_a = lp_coef(expmap(1500, 18000, p[kTone]), fs);
    const float glide = expf(-1 / (pv(&P[kGlide], p[kGlide]) * 0.001f * fs)), gain_a = 1 - expf(-1 / (0.012f * fs));
    const float og = db(pv(&P[kOutput], p[kOutput]));
    for (int k = 0; k < n; k++) {
        float l = il ? il[k] : 0, r = ir ? ir[k] : 0;
        if (!finite_bits(l) || l > 8 || l < -8) l = 0;   /* only clean audio in */
        if (!finite_bits(r) || r > 8 || r < -8) r = 0;
        const float x = (l + r) * 0.5f;
        voicein_push(&s->vin, x);
        track(&s->trk, x, fs);
        const int voiced = s->trk.conf > 0.75f && s->trk.hz > 70 && s->trk.hz < 1100;
        const float t0 = voiced ? fs / s->trk.hz : fs / 150, m = voiced ? midi_of(s->trk.hz) : 60;
        const float q = nearest_in_scale(m, key, scale);
        float wl = 0, wr = 0;
        for (int v = 0; v < 2; v++) {
            const int on = iv[v] > 0 && voiced && lvl[v] > 0.001f;
            if (on) {
                const float ratio = exp2f((harmony_note(q, key, scale, iv[v], NULL, 0) - q) / 12);
                s->ratio_s[v] = ratio + (s->ratio_s[v] - ratio) * glide;
            }
            s->gain_s[v] += gain_a * ((float)on - s->gain_s[v]);
            if (iv[v] <= 0 && s->gain_s[v] < 1e-4f) continue;   /* (an Off voice costs nothing) */
            float y = voice_run(&s->voice[v], &s->vin, t0, s->ratio_s[v], 1) * s->gain_s[v] * lvl[v];
            y = lp(&s->tone[v][1], lp(&s->tone[v][0], y, tone_a), tone_a);
            wl += y * pl[v];
            wr += y * pr[v];
        }
        s->out_s += 0.002f * (og - s->out_s);
        float yl = (l * dry + wl) * s->out_s, yr = (r * dry + wr) * s->out_s;
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
    s->norm[i] = snap(&P[i], clampf(v, 0, 1), s->norm[i]);
    if (P[i].kind == K_OPT) s->update_display = 1;
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: line_free(&s->vin.l); free(s); return 1;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) copy_str(p, P[idx].name, 32); return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay: if (idx >= 0 && idx < kNumParams) { char t[32]; pdisplay(&P[idx], s->norm[idx], t); copy_str(p, t, 24); } return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "HedRush Harmony", 32); return 1;
    case effGetVendorString: copy_str(p, VC_VENDOR, 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate: if (opt > 1000 && fabsf(opt - s->fs) > 0.5f) setup(s, opt); return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (val) reset(s); return 1;
    case effCanDo: return -1;
    default: return 0;
    }
}

__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    hann_init();
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    for (int i = 0; i < kNumParams; i++) s->norm[i] = pn(&P[i], P[i].def);
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
    e->uniqueID = 0x48524843;   /* 'HRHC' */
    e->version = 1000;
    return e;
}

#ifdef VC_PARAMS
int main(void) { write_params_json("HedRush Harmony", P, kNumParams, NULL); return 0; }
#endif
