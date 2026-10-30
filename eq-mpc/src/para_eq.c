/* X-Para EQ: a 4-band parametric EQ for MPC OS (VST2, stereo), laid out like a digital console's channel EQ (the
 * X32 style): a Low Cut, then four bands, each with its own Type (LCut, LShv, PEQ, VEQ, HShv, HCut), Freq, Gain
 * (+/-15 dB) and Q (0.3 .. 10). Our own code, on vocal-mpc's core (parameter table, biquads).
 * PEQ is the RBJ peaking filter; VEQ ("vintage") widens as its gain comes down, like an old proportional-Q EQ; the
 * shelves use Q as their slope; LCut / HCut in a band are 12 dB/octave with Q as the resonance. Low Cut: 12
 * dB/octave Butterworth, 20 .. 400 Hz. EQ in/out, Output. */
#include "vocal_core.h"

#define NBAND 4
static const char *const TYPES[] = {"LCut", "LShv", "PEQ", "VEQ", "HShv", "HCut"};
enum { T_LCUT, T_LSHV, T_PEQ, T_VEQ, T_HSHV, T_HCUT };
enum { kLowCut, kLowCutHz, kEq, kBand0, B_TYPE = 0, B_FREQ, B_GAIN, B_Q, BP };
#define BANDP(b, w) (kBand0 + (b) * BP + (w))
enum { kOutput = kBand0 + NBAND * BP, kNumParams };
#define BANDDEF(n, t, f) OPT("b" #n "_type", #n " Type", TYPES, t), EXPP("b" #n "_freq", #n " Freq", 20, 20000, f, "Hz"), \
    LIN("b" #n "_gain", #n " Gain", -15, 15, 0, "dB"), EXPP("b" #n "_q", #n " Q", 0.3f, 10, 2, "")
static const ParamDef P[kNumParams] = {
    TOGGLE("low_cut", "Low Cut", 0), EXPP("low_cut_hz", "Low Cut Freq", 20, 400, 80, "Hz"), TOGGLE("eq", "EQ", 1),
    BANDDEF(1, T_LSHV, 100), BANDDEF(2, T_PEQ, 500), BANDDEF(3, T_PEQ, 2000), BANDDEF(4, T_HSHV, 10000),
    LIN("output", "Output", -24, 12, 0, "dB"),
};

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    int ready;
    volatile int update_display;
    Bq band[2][NBAND], lc[2];
    BqCo bc[NBAND], lcc;
    int active[NBAND];
    float out_s, eq_s;
    float co_key;
} Plugin;

static void reset(Plugin *s) { memset(s->band, 0, sizeof s->band); memset(s->lc, 0, sizeof s->lc); }
static void coefficients(Plugin *s) {
    const float *p = s->norm;
    for (int b = 0; b < NBAND; b++) {
        const int t = (int)pv(&P[BANDP(b, B_TYPE)], p[BANDP(b, B_TYPE)]);
        const float hz = pv(&P[BANDP(b, B_FREQ)], p[BANDP(b, B_FREQ)]), g = pv(&P[BANDP(b, B_GAIN)], p[BANDP(b, B_GAIN)]);
        const float q = pv(&P[BANDP(b, B_Q)], p[BANDP(b, B_Q)]);
        switch (t) {
        case T_LCUT: s->bc[b] = bq_co(BQ_HP, hz, q * 0.354f, 0, s->fs); break;   /* (Q 2 -> 0.707: Butterworth) */
        case T_HCUT: s->bc[b] = bq_co(BQ_LP, hz, q * 0.354f, 0, s->fs); break;
        case T_LSHV: s->bc[b] = bq_co(BQ_LSHELF, hz, q * 0.354f, g, s->fs); break;
        case T_HSHV: s->bc[b] = bq_co(BQ_HSHELF, hz, q * 0.354f, g, s->fs); break;
        case T_VEQ: s->bc[b] = bq_co(BQ_PEAK, hz, q * (0.2f + 0.8f * fabsf(g) / 15), g, s->fs); break;   /* broad when gentle */
        default: s->bc[b] = bq_co(BQ_PEAK, hz, q, g, s->fs); break;
        }
        s->active[b] = t == T_LCUT || t == T_HCUT || fabsf(g) > 0.01f;   /* (a 0 dB peak or shelf: skipped) */
    }
    s->lcc = bq_co(BQ_HP, pv(&P[kLowCutHz], p[kLowCutHz]), 0.7071f, 0, s->fs);
}
static int setup(Plugin *s, float fs) { s->fs = fs; s->co_key = -1; reset(s); s->ready = 1; return 1; }

static void process(AEffect *e, float **in, float **out, int32_t n) {
    VC_FLUSH_DENORMALS_BEGIN;
    Plugin *s = (Plugin *)e->object;
    if (s->master && s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    const float *il = in && in[0] ? in[0] : NULL, *ir = in && in[1] ? in[1] : il;
    if (!s->ready) { for (int k = 0; k < n; k++) out[0][k] = out[1][k] = 0; goto done; }
    {
        float key = 0;
        for (int i = 0; i < kNumParams; i++) key += s->norm[i] * (float)(i * 7 + 1);
        if (key != s->co_key) { s->co_key = key; coefficients(s); }
    }
    const int lc = s->norm[kLowCut] > 0.5f;
    const float og = db(pv(&P[kOutput], s->norm[kOutput])), eq_on = s->norm[kEq] > 0.5f ? 1 : 0;
    for (int k = 0; k < n; k++) {
        float x[2] = {il ? il[k] : 0, ir ? ir[k] : 0};
        s->eq_s += 0.002f * (eq_on - s->eq_s);
        s->out_s += 0.002f * (og - s->out_s);
        for (int c = 0; c < 2; c++) {
            float v = x[c];
            if (!finite_bits(v) || v > 8 || v < -8) v = 0;   /* only clean audio in */
            float y = lc ? bq(&s->lc[c], &s->lcc, v) : v;
            for (int b = 0; b < NBAND; b++) if (s->active[b]) y = bq(&s->band[c][b], &s->bc[b], y);
            y = (v + (y - v) * s->eq_s) * s->out_s;
            if (!finite_bits(y) || fabsf(y) > 63) { reset(s); y = 0; }   /* past +36 dB: start over */
            out[c][k] = clampf(y, -4, 4);   /* +12 dB ceiling */
        }
    }
done:
    VC_FLUSH_DENORMALS_END;
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams) return;
    s->norm[i] = snap(&P[i], clampf(v, 0, 1), s->norm[i]);
    if (P[i].kind == K_OPT || P[i].kind == K_TOGGLE) s->update_display = 1;
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }

static void display(const Plugin *s, int i, char *t) {
    if (i >= kBand0 && i < kOutput && (i - kBand0) % BP == B_Q) { snprintf(t, 24, "%.1f", pv(&P[i], s->norm[i])); return; }
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
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "X-Para EQ", 32); return 1;
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
    s->out_s = 1;
    s->eq_s = 1;
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
    e->uniqueID = 0x58504551;   /* 'XPEQ' */
    e->version = 1000;
    return e;
}
__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) { return make(master); }

#ifdef VC_PARAMS
int main(void) { write_params_json("X-Para EQ", P, kNumParams, NULL); return 0; }
#endif
