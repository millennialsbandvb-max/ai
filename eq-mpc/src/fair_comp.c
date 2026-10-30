/* Fairchyld 670: a vari-mu tube compressor for MPC OS (VST2, stereo), after the classic 660/670 (and laid out like a
 * digital console's "fair" compressor: Input, Threshold, Time, Bias, Output, a gain-reduction VU). Our own code, on
 * vocal-mpc's core.
 * Feedback detector (the gain cell is controlled from its own output, as in the original), a soft knee that gets
 * harder and steeper with Bias, and the six Time constants (attack ms / release s):
 *   1: 0.2 / 0.3   2: 0.2 / 0.8   3: 0.4 / 2   4: 0.4 / 5   5: 0.4 / 2 then 10 (program dependent)
 *   6: 0.2 / 0.3 then 10 (program dependent: short peaks let go fast, held compression slowly).
 * Mode: Linked (one detector for both sides), Dual (each side its own), Lat/Vert (mid and side, as the 670's
 * lateral/vertical mode). Tube colour: a gentle, mostly even-order bend that grows with level. Mix for parallel
 * compression. GR is a read-only parameter for the page's meter. */
#include "vocal_core.h"

static const char *const MODES[] = {"Linked", "Dual", "Lat/Vert"};
enum { kInput, kThreshold, kTime, kBias, kOutput, kMix, kMode, kComp, kGr, kNumParams };
static const ParamDef P[kNumParams] = {
    LIN("input", "Input", -20, 20, 0, "dB"), LIN("threshold", "Threshold", 0, 10, 5, ""), INTP("time", "Time", 1, 6, 2, ""),
    LIN("bias", "Bias", 0, 10, 5, ""), LIN("output", "Output", -20, 20, 0, "dB"), PCT("mix", "Mix", 100),
    OPT("mode", "Mode", MODES, 0), TOGGLE("comp", "Comp", 1), LIN("gr", "GR", 0, 20, 0, "dB"),
};
static const float ATT_MS[6] = {0.2f, 0.2f, 0.4f, 0.4f, 0.4f, 0.2f}, REL_S[6] = {0.3f, 0.8f, 2, 5, 2, 0.3f};
static const float SLOW_S[6] = {0, 0, 0, 0, 10, 10};   /* program dependent: the slow part's release */

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    int ready;
    volatile int update_display, report_gr;
    float fb[2];          /* each side's last output (the detector listens to it) */
    float env[2], fast[2], slow[2];   /* detector envelope (dB) and the gain-reduction followers (dB) */
    float dc_x[2], dc_y[2];
    float in_s, out_s, mix_s, comp_s;
    float gr_shown;
    int meter_count;
} Plugin;

static void reset(Plugin *s) {
    for (int c = 0; c < 2; c++) { s->fb[c] = 0; s->env[c] = -120; s->fast[c] = s->slow[c] = 0; s->dc_x[c] = s->dc_y[c] = 0; }
}
static int setup(Plugin *s, float fs) { s->fs = fs; reset(s); s->ready = 1; return 1; }

/* static curve: dB over the threshold -> dB of gain reduction (soft knee; Bias: knee 14 -> 4 dB, ratio 2 -> 7) */
static float curve(float over, float bias) {
    const float knee = lerp(14, 4, bias), slope = 1 - 1 / lerp(2, 7, bias);
    if (over <= -knee / 2) return 0;
    if (over < knee / 2) { const float u = over + knee / 2; return slope * u * u / (2 * knee); }
    return slope * over;
}

static void process(AEffect *e, float **in, float **out, int32_t n) {
    VC_FLUSH_DENORMALS_BEGIN;
    Plugin *s = (Plugin *)e->object;
    if (s->master) {
        if (s->report_gr) { s->report_gr = 0; s->master(e, audioMasterAutomate, kGr, 0, NULL, s->norm[kGr]); }
        if (s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    }
    const float *il = in && in[0] ? in[0] : NULL, *ir = in && in[1] ? in[1] : il;
    if (!s->ready) { for (int k = 0; k < n; k++) out[0][k] = out[1][k] = 0; goto done; }
    const float fs = s->fs, *p = s->norm;
    const int t = (int)pv(&P[kTime], p[kTime]) - 1, mode = (int)pv(&P[kMode], p[kMode]);
    const float thr = -4 * pv(&P[kThreshold], p[kThreshold]) - 2;   /* 0..10 -> -2 .. -42 dBFS */
    const float bias = p[kBias];
    const float a_att = 1 - expf(-1 / (ATT_MS[t] * 0.001f * fs)), a_rel = 1 - expf(-1 / (REL_S[t] * fs));
    const float a_slow_att = 1 - expf(-1 / (1.5f * fs)), a_slow_rel = SLOW_S[t] > 0 ? 1 - expf(-1 / (SLOW_S[t] * fs)) : 0;
    const float a_env = 1 - expf(-1 / (0.0005f * fs)), a_env_rel = 1 - expf(-1 / (0.03f * fs));
    const float gin = db(pv(&P[kInput], p[kInput])), gout = db(pv(&P[kOutput], p[kOutput])), mix = p[kMix];
    const float comp_on = p[kComp] > 0.5f ? 1 : 0, a_dc = 1 - expf(-2 * PI * 8 / fs);
    for (int k = 0; k < n; k++) {
        float x[2] = {il ? il[k] : 0, ir ? ir[k] : 0};
        for (int c = 0; c < 2; c++) if (!finite_bits(x[c]) || x[c] > 8 || x[c] < -8) x[c] = 0;   /* only clean audio in */
        s->in_s += 0.002f * (gin - s->in_s); s->out_s += 0.002f * (gout - s->out_s);
        s->mix_s += 0.002f * (mix - s->mix_s); s->comp_s += 0.002f * (comp_on - s->comp_s);
        x[0] *= s->in_s; x[1] *= s->in_s;
        float v[2] = {x[0], x[1]};
        if (mode == 2) { v[0] = (x[0] + x[1]) * 0.7071f; v[1] = (x[0] - x[1]) * 0.7071f; }   /* lateral / vertical */
        /* the detector hears the gain cell's own output (feedback) */
        float lvl[2];
        for (int c = 0; c < 2; c++) {
            const float a = fabsf(s->fb[c]), ldb = 20 * log10f(a + 1e-6f);
            s->env[c] += (ldb > s->env[c] ? a_env : a_env_rel) * (ldb - s->env[c]);
            lvl[c] = s->env[c];
        }
        if (mode == 0) lvl[0] = lvl[1] = fmaxf(lvl[0], lvl[1]);
        float y[2];
        for (int c = 0; c < 2; c++) {
            const float target = curve(lvl[c] - thr, bias) * 1.6f;   /* (feedback: the loop needs more push) */
            s->fast[c] += (target > s->fast[c] ? a_att : a_rel) * (target - s->fast[c]);
            if (a_slow_rel > 0) s->slow[c] += (target > s->slow[c] ? a_slow_att : a_slow_rel) * (target - s->slow[c]);
            else s->slow[c] = 0;
            float gr = fmaxf(s->fast[c], s->slow[c] * 0.9f);
            gr = clampf(gr, 0, 30) * s->comp_s;
            float w = v[c] * db(-gr);
            /* tube colour: a little even-order bend, more as it's driven, then a soft limit */
            w = tanh_fast(w * 0.8f) / 0.8f + 0.04f * w * fabsf(w);
            s->fb[c] = w;
            y[c] = w;
            s->gr_shown = fmaxf(s->gr_shown, gr);   /* (the meter shows the most since it last moved) */
        }
        if (mode == 2) { const float m = y[0], d = y[1]; y[0] = (m + d) * 0.7071f; y[1] = (m - d) * 0.7071f; }
        for (int c = 0; c < 2; c++) {
            s->dc_y[c] = y[c] - s->dc_x[c] + (1 - a_dc) * s->dc_y[c]; s->dc_x[c] = y[c];   /* (the bend's DC out) */
            float o = (x[c] + (s->dc_y[c] - x[c]) * s->mix_s) * s->out_s;
            if (!finite_bits(o) || fabsf(o) > 63) { reset(s); o = 0; }   /* past +36 dB: start over */
            out[c][k] = clampf(o, -4, 4);   /* +12 dB ceiling */
        }
    }
    /* the meter, ~25 times a second */
    s->meter_count += n;
    if (s->meter_count >= (int)(fs / 25)) {
        s->meter_count = 0;
        const float g = clampf(s->gr_shown / 20, 0, 1);
        if (fabsf(g - s->norm[kGr]) > 0.004f) { s->norm[kGr] = g; s->report_gr = 1; }
        s->gr_shown = 0;
    }
done:
    VC_FLUSH_DENORMALS_END;
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams || i == kGr) return;   /* (GR is the meter's: read-only) */
    s->norm[i] = snap(&P[i], clampf(v, 0, 1), s->norm[i]);
    if (P[i].kind == K_OPT || P[i].kind == K_TOGGLE || P[i].kind == K_INT) s->update_display = 1;
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }

static void display(const Plugin *s, int i, char *t) {
    if (i == kThreshold || i == kBias) { snprintf(t, 24, "%.1f", pv(&P[i], s->norm[i])); return; }
    if (i == kTime) { snprintf(t, 24, "%d", (int)pv(&P[i], s->norm[i])); return; }
    if (i == kGr) { snprintf(t, 24, "-%.1f dB", s->norm[i] * 20); return; }
    pdisplay(&P[i], s->norm[i], t);
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: free(s); return 1;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) copy_str(p, P[idx].name, 32); return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay: if (idx >= 0 && idx < kNumParams) { char t[32] = {0}; display(s, idx, t); copy_str(p, t, 24); } return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams && idx != kGr;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "Fairchyld 670", 32); return 1;
    case effGetVendorString: copy_str(p, VC_VENDOR, 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate: if (opt > 1000 && fabsf(opt - s->fs) > 0.5f) setup(s, opt); return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (val) reset(s); return 1;
    default: return 0;
    }
}

static AEffect *make(audioMasterCallback master) {
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    for (int i = 0; i < kNumParams; i++) s->norm[i] = pn(&P[i], P[i].def);
    s->in_s = s->out_s = s->mix_s = s->comp_s = 1;
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
    e->uniqueID = 0x46363730;   /* 'F670' */
    e->version = 1000;
    return e;
}
__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) { return make(master); }

#ifdef VC_PARAMS
int main(void) { write_params_json("Fairchyld 670", P, kNumParams, NULL); return 0; }
#endif
