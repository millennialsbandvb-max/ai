/* GQ31 Graphic: a 31-band graphic EQ for MPC OS (VST2, stereo), in the style of the classic rack graphics (one
 * slider per ISO 1/3-octave band, 20 Hz .. 20 kHz). Our own code, on vocal-mpc's core (parameter table, biquads).
 * Each band is a constant-Q peaking filter (Q 4.3, a third of an octave wide at any boost or cut), so neighbouring
 * bands add up like an analog graphic's do. Range: the sliders span +/-12 dB, or +/-6 dB for finer moves. High-pass
 * (20 Hz = off) and low-pass (20 kHz = off), 18 dB/octave Butterworth. Level, EQ in/out, Flat. Bands at 0 dB cost
 * nothing. */
#include "vocal_core.h"

#define NB 31
static const float BAND_HZ[NB] = {20, 25, 31.5f, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000,
                                  1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};
static const char *const RANGES[] = {"+/-12 dB", "+/-6 dB"};
#define BAND(k, n) LIN(k, n, -12, 12, 0, "dB")
enum { kBand0, kRange = kBand0 + NB, kHpf, kLpf, kLevel, kEq, kFlat, kNumParams };
static const ParamDef P[kNumParams] = {
    BAND("g20", "20"), BAND("g25", "25"), BAND("g31", "31.5"), BAND("g40", "40"), BAND("g50", "50"), BAND("g63", "63"),
    BAND("g80", "80"), BAND("g100", "100"), BAND("g125", "125"), BAND("g160", "160"), BAND("g200", "200"),
    BAND("g250", "250"), BAND("g315", "315"), BAND("g400", "400"), BAND("g500", "500"), BAND("g630", "630"),
    BAND("g800", "800"), BAND("g1k", "1k"), BAND("g1k25", "1.25k"), BAND("g1k6", "1.6k"), BAND("g2k", "2k"),
    BAND("g2k5", "2.5k"), BAND("g3k15", "3.15k"), BAND("g4k", "4k"), BAND("g5k", "5k"), BAND("g6k3", "6.3k"),
    BAND("g8k", "8k"), BAND("g10k", "10k"), BAND("g12k5", "12.5k"), BAND("g16k", "16k"), BAND("g20k", "20k"),
    OPT("range", "Range", RANGES, 0), EXPP("hpf", "High-Pass", 20, 500, 20, "Hz"), EXPP("lpf", "Low-Pass", 1000, 20000, 20000, "Hz"),
    LIN("level", "Level", -24, 12, 0, "dB"), TOGGLE("eq", "EQ", 1), BUTTON("flat", "Flat"),
};

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    int ready;
    volatile int update_display, automate_all;
    Bq band[2][NB], hp[2], lp[2];
    BqCo bc[NB], hpc, lpc;
    float gain_db[NB];
    float hp1[2], lp1[2], hp1a, lp1a;   /* the first-order halves of the 18 dB/octave filters */
    float out_s, eq_s;
    float co_key;
} Plugin;

static float band_db(const Plugin *s, int b) {   /* a band's gain, after the Range */
    return pv(&P[kBand0 + b], s->norm[kBand0 + b]) * (option_of(s->norm[kRange], 2) ? 0.5f : 1);
}
static int hpf_on(const Plugin *s) { return s->norm[kHpf] > 0.002f; }
static int lpf_on(const Plugin *s) { return s->norm[kLpf] < 0.998f; }

static void reset(Plugin *s) {
    memset(s->band, 0, sizeof s->band); memset(s->hp, 0, sizeof s->hp); memset(s->lp, 0, sizeof s->lp);
    s->hp1[0] = s->hp1[1] = s->lp1[0] = s->lp1[1] = 0;
}
static void coefficients(Plugin *s) {
    for (int b = 0; b < NB; b++) {
        s->gain_db[b] = band_db(s, b);
        s->bc[b] = bq_co(BQ_PEAK, BAND_HZ[b], 4.32f, s->gain_db[b], s->fs);
    }
    const float hz = pv(&P[kHpf], s->norm[kHpf]), lz = pv(&P[kLpf], s->norm[kLpf]);
    s->hpc = bq_co(BQ_HP, hz, 1.0f, 0, s->fs);   /* (Q 1 + a first-order pole: third-order Butterworth) */
    s->lpc = bq_co(BQ_LP, lz, 1.0f, 0, s->fs);
    s->hp1a = 1 - expf(-2 * PI * hz / s->fs);
    s->lp1a = 1 - expf(-2 * PI * clampf(lz, 10, s->fs * 0.45f) / s->fs);
}
static int setup(Plugin *s, float fs) {
    s->fs = fs;
    s->co_key = -1;
    reset(s);
    s->ready = 1;
    return 1;
}

static void process(AEffect *e, float **in, float **out, int32_t n) {
    VC_FLUSH_DENORMALS_BEGIN;
    Plugin *s = (Plugin *)e->object;
    if (s->master) {
        if (s->automate_all) {
            s->automate_all = 0;
            for (int i = 0; i < kNumParams; i++) s->master(e, audioMasterAutomate, i, 0, NULL, s->norm[i]);
            s->update_display = 1;
        }
        if (s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    }
    const float *il = in && in[0] ? in[0] : NULL, *ir = in && in[1] ? in[1] : il;
    if (!s->ready) { for (int k = 0; k < n; k++) out[0][k] = out[1][k] = 0; goto done; }
    {   /* coefficients only when a slider moved */
        float key = 0;
        for (int i = 0; i < kEq; i++) key += s->norm[i] * (float)(i * 7 + 1);
        if (key != s->co_key) { s->co_key = key; coefficients(s); }
    }
    int act[NB], na = 0;
    for (int b = 0; b < NB; b++) if (fabsf(s->gain_db[b]) > 0.01f) act[na++] = b;
    const int hpf = hpf_on(s), lpf = lpf_on(s);
    const float og = db(pv(&P[kLevel], s->norm[kLevel])), eq_on = s->norm[kEq] > 0.5f ? 1 : 0;
    for (int k = 0; k < n; k++) {
        float x[2] = {il ? il[k] : 0, ir ? ir[k] : 0};
        s->eq_s += 0.002f * (eq_on - s->eq_s);   /* (EQ in/out crossfades) */
        s->out_s += 0.002f * (og - s->out_s);
        for (int c = 0; c < 2; c++) {
            float v = x[c];
            if (!finite_bits(v) || v > 8 || v < -8) v = 0;   /* only clean audio in */
            float y = v;
            if (hpf) { y = bq(&s->hp[c], &s->hpc, y); s->hp1[c] += s->hp1a * (y - s->hp1[c]); y -= s->hp1[c]; }
            for (int a = 0; a < na; a++) y = bq(&s->band[c][act[a]], &s->bc[act[a]], y);
            if (lpf) { y = bq(&s->lp[c], &s->lpc, y); s->lp1[c] += s->lp1a * (y - s->lp1[c]); y = s->lp1[c]; }
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
    v = clampf(v, 0, 1);
    if (i == kFlat) {   /* every band to 0 dB */
        if (v > 0.5f) { for (int b = 0; b < NB; b++) s->norm[kBand0 + b] = 0.5f; s->automate_all = 1; }
        s->norm[i] = 0;
        return;
    }
    s->norm[i] = snap(&P[i], v, s->norm[i]);
    if (i == kRange || i == kEq) s->update_display = 1;
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }

static void display(const Plugin *s, int i, char *t) {
    if (i < kRange) { const float g = band_db(s, i - kBand0); snprintf(t, 24, fabsf(g) < 0.05f ? "0" : "%+.1f", g); return; }
    if (i == kHpf && !hpf_on(s)) { snprintf(t, 24, "Off"); return; }
    if (i == kLpf && !lpf_on(s)) { snprintf(t, 24, "Off"); return; }
    pdisplay(&P[i], s->norm[i], t);
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    (void)val;
    switch (op) {
    case effOpen: return 1;
    case effClose: free(s); return 1;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) copy_str(p, P[idx].name, 32); return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay: if (idx >= 0 && idx < kNumParams) { char t[32] = {0}; display(s, idx, t); copy_str(p, t, 24); } return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "GQ31 Graphic", 32); return 1;
    case effGetVendorString: copy_str(p, VC_VENDOR, 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate: if (opt > 1000 && fabsf(opt - s->fs) > 0.5f) { setup(s, opt); s->co_key = -1; } return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (val) reset(s); return 1;
    default: return 0;
    }
}

static AEffect *make(audioMasterCallback master) {
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    for (int i = 0; i < kNumParams; i++) s->norm[i] = P[i].kind == K_BUTTON ? 0 : pn(&P[i], P[i].def);
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
    e->uniqueID = 0x47513331;   /* 'GQ31' */
    e->version = 1000;
    return e;
}
__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) { return make(master); }

#ifdef VC_PARAMS
int main(void) { write_params_json("GQ31 Graphic", P, kNumParams, NULL); return 0; }
#endif
