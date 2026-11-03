/* Ergo EUB: the user's HeadRush "Ergo" rigs for their Ergo 5-string electric upright bass (passive piezo) in one MPC
 * insert (VST2, stereo). Our own code, on vocal-mpc's core (parameter table, biquads, FDN reverb), hosting a NAM MPC
 * engine for the amp (as 0dd Rig does: nam_mpc.cpp built with VSTPluginMain renamed nam_entry).
 * The chain, in this order (the handoff's "order matters"):
 *   Input -> [Comp, if Before EQ] -> EQ -> [Comp, if After EQ] -> Hall 1 -> Amp (NAM) -> Hall 2 -> Rig Volume -> Volume
 * EQ: High-pass (12 dB/oct; keep it at 35 Hz or under for the B and E strings), Hum notch (50/60 Hz and up to two
 *   harmonics, depth, Q), Low shelf, eight peaks (freq, gain, Q), High shelf, Low-pass (Off / 12 / 24 dB/oct).
 * Comp: feed-forward, threshold / ratio / attack / release / make-up, 6 dB soft knee, linked stereo; GR is shown.
 * Halls: two of the same kind (the owner likes them stacked), each Mix (wet against dry), Pre-delay, Decay, Tone.
 * Volume: last, log taper (gain = v^2.5), 0% = silence; MIDI CC 7 or 11 (an expression pedal), any channel / MIDI Ch.
 * No gate anywhere (the amp's own gate is NAM's, set per program). Programs: ERGO_PROGRAMS (the handoff's rigs);
 * the amp model and IR are the user's: a program sets NAM's knobs and asks for a capture by words ("ultra lo,svt"),
 * which loads only if one of the user's models has them.
 * State: a chunk, "ERGO1 <n>" + our values, then NAM's own chunk (its model / IR paths). */
#include "vocal_core.h"

AEffect *nam_entry(audioMasterCallback);
int nam_pick(AEffect *, int, int, int, char *, int);
void nam_find(AEffect *, const char *, const char *);

#define NAM_N 22   /* NAM MPC's parameters (nam-mpc/params.json; gen-params checks the order) */
#define NPEAK 8
static const char *const CPOS[] = {"Before EQ", "After EQ"};
static const char *const HUM[] = {"Off", "50 Hz", "60 Hz"};
static const char *const LPF[] = {"Off", "12 dB/oct", "24 dB/oct"};

enum { kProgram, kProgPrev, kProgNext, kInput,
       kCompOn, kCompPos, kThresh, kRatio, kAttack, kRelease, kMakeup, kGr,
       kEqOn, kHpfOn, kHpfHz, kHum, kHumDepth, kHumQ, kHumHarm, kLsHz, kLsGain,
       kPeak0, PK_HZ = 0, PK_GAIN, PK_Q, PKP,
       kHsHz = kPeak0 + NPEAK * PKP, kHsGain, kLpf, kLpfHz,
       kRev0, RV_ON = 0, RV_MIX, RV_PRE, RV_DECAY, RV_TONE, RVP,
       kAmpOn = kRev0 + 2 * RVP, kRigVol, kVolume, kOwn };
#define PEAK(b, w) (kPeak0 + (b) * PKP + (w))
#define REV(r, w) (kRev0 + (r) * RVP + (w))
enum { kNam0 = kOwn, kNumParams = kNam0 + NAM_N };

#define PEAKDEF(n, f) EXPP("p" #n "_hz", "Peak " #n " Freq", 20, 12000, f, "Hz"), LIN("p" #n "_gain", "Peak " #n " Gain", -18, 18, 0, "dB"), \
    EXPP("p" #n "_q", "Peak " #n " Q", 0.3f, 12, 1, "")
#define REVDEF(n) TOGGLE("r" #n "_on", "Hall " #n, 0), PCT("r" #n "_mix", "Hall " #n " Mix", 18), \
    LIN("r" #n "_pre", "Hall " #n " Pre-delay", 0, 200, 68, "ms"), PCT("r" #n "_decay", "Hall " #n " Decay", 54), \
    PCT("r" #n "_tone", "Hall " #n " Tone", 57)
static const ParamDef P[kOwn] = {
    PRESET("program", "Program"), BUTTON("program_prev", "Program <"), BUTTON("program_next", "Program >"),
    LIN("input", "Input", -24, 36, 0, "dB"),
    TOGGLE("comp", "Comp", 1), OPT("comp_pos", "Comp Position", CPOS, 0), LIN("threshold", "Threshold", -60, 0, -25, "dB"),
    EXPP("ratio", "Ratio", 1, 20, 4.4f, ""), EXPP("attack", "Attack", 0.3f, 100, 2.6f, "ms"),
    EXPP("release", "Release", 20, 2000, 151, "ms"), LIN("makeup", "Makeup", -12, 24, 10, "dB"), LIN("gr", "GR", 0, 30, 0, "dB"),
    TOGGLE("eq", "EQ", 1), TOGGLE("hpf", "High-Pass", 0), EXPP("hpf_hz", "HPF Freq", 15, 300, 31, "Hz"),
    OPT("hum", "Hum", HUM, 0), LIN("hum_depth", "Hum Depth", -40, 0, -30, "dB"), EXPP("hum_q", "Hum Q", 1, 20, 9.2f, ""),
    INTP("hum_harm", "Hum Harmonics", 1, 3, 1, ""),
    EXPP("ls_hz", "Low Shelf Freq", 30, 600, 139, "Hz"), LIN("ls_gain", "Low Shelf Gain", -15, 15, 0, "dB"),
    PEAKDEF(1, 119), PEAKDEF(2, 250), PEAKDEF(3, 496), PEAKDEF(4, 1000), PEAKDEF(5, 1800), PEAKDEF(6, 2400), PEAKDEF(7, 4000),
    PEAKDEF(8, 5500),
    EXPP("hs_hz", "High Shelf Freq", 1500, 16000, 9000, "Hz"), LIN("hs_gain", "High Shelf Gain", -18, 18, 0, "dB"),
    OPT("lpf", "Low-Pass", LPF, 0), EXPP("lpf_hz", "LPF Freq", 200, 16000, 794, "Hz"),
    REVDEF(1), REVDEF(2),
    TOGGLE("amp", "Amp", 0), LIN("rig_volume", "Rig Volume", -24, 24, 0, "dB"), PCT("volume", "Volume", 100),
};

/* ---- programs: the handoff's rigs (ERGO_EUB_HANDOFF.md, read off the HeadRush on 2026-11-02) --------------------- */
typedef struct { float hz, gain, q; } Pk;
typedef struct {
    const char *name;
    float input;
    int comp, comp_pos; float thr, ratio, att, rel, makeup;
    int hpf; float hpf_hz;
    int hum; float hum_depth, hum_q;
    float ls_hz, ls_gain;
    Pk pk[NPEAK];
    float hs_hz, hs_gain;
    int lpf; float lpf_hz;
    int rev[2]; float mix, pre, decay, tone;   /* (both halls alike) */
    int amp; float nam_in, nam_gate, nam_bass, nam_mid, nam_treble, nam_out;
    float rig_vol;
    const char *capture;   /* words for the capture (NAM folder + name, any case); NULL: the amp stays */
} Prog;
#define NOPK {0, 0, 1}
#define AUTOQ {119, 3.3f, 1}, {398, -1.4f, 1}, {2400, -8.8f, 1.13f}, {5500, -15, 1}   /* the Auto-Q 8-band's peaks */
static const Prog PROGS[] = {
    /* name             input comp pos thr     ratio att  rel  makeup hpf hz  hum depth  q     ls hz ls dB */
    {"Ergo Simple",      0.5f, 1, 1, -20,   4,   20,  198, 7.6f, 0, 31, 2, -30, 9.2f, 139, 0,
     {NOPK, NOPK, NOPK, NOPK, NOPK, NOPK, NOPK, NOPK}, 9000, 0, 2, 398,
     {0, 0}, 18, 68, 54, 57, 1, 7.2f, -59, 5, 5, 1.9f, 0, 0, "ultra lo,svt,ampeg"},
    {"Ergo Arco",       -9.6f, 1, 1, -16.2f, 1.9f, 42, 412, 6.4f, 0, 31, 2, -30, 9.2f, 139, 0,
     {NOPK, NOPK, NOPK, NOPK, NOPK, NOPK, NOPK, NOPK}, 9000, 0, 2, 1784,
     {0, 0}, 18, 68, 54, 57, 1, 0, -100, 5, 5, 5, 0, 0, "ultra lo,svt,ampeg"},
    {"Ergo to Bass",     0,    1, 0, -25,   4.4f, 2.6f, 151, 10, 1, 31, 0, -30, 9.2f, 242, 2.2f,
     {AUTOQ, {496, -5.8f, 0.7f}, NOPK, NOPK, NOPK}, 9000, -10, 1, 794,
     {0, 0}, 18, 68, 54, 57, 0, 0, -100, 5, 5, 5, 0, 0, NULL},
    {"Ergo to Bass 1EQ", 0,    1, 0, -25,   4.4f, 2.6f, 151, 10, 0, 31, 0, -30, 9.2f, 139, 2.6f,
     {{496, -5.8f, 0.7f}, {1800, 2.2f, 1}, NOPK, NOPK, NOPK, NOPK, NOPK, NOPK}, 9000, 0, 2, 794,
     {0, 0}, 18, 68, 54, 57, 0, 0, -100, 5, 5, 5, 0, 0, NULL},
    {"Ergo Pop Rock",    3.4f, 1, 0, -21.6f, 3,   12,  63,  6,  1, 31, 2, -3.1f, 3.3f, 242, 2.2f,
     {AUTOQ, {496, -5.8f, 0.7f}, {1800, 2.2f, 1}, {119, -3.8f, 2.8f}, NOPK}, 9000, -10, 2, 794,
     {1, 1}, 18, 68, 54, 57, 0, -2.8f, -100, 3.9f, 3.9f, 3.4f, 4.8f, 8.2f, NULL},
    /* the same with the amp on (the owner keeps the Bethel Clean capture as an option) */
    {"Ergo Pop Rock Amp", 3.4f, 1, 0, -21.6f, 3,  12,  63,  6,  1, 31, 2, -3.1f, 3.3f, 242, 2.2f,
     {AUTOQ, {496, -5.8f, 0.7f}, {1800, 2.2f, 1}, {119, -3.8f, 2.8f}, NOPK}, 9000, -10, 2, 794,
     {1, 1}, 18, 68, 54, 57, 1, -2.8f, -100, 3.9f, 3.9f, 3.4f, 4.8f, 8.2f, "bethel,tone hammer,aguilar"},
};
#define NPROGS ((int)(sizeof PROGS / sizeof *PROGS))
#define DEFAULT_PROG 3   /* Ergo to Bass 1EQ: the base (the handoff's "build first") */

/* ---- the plugin --------------------------------------------------------------------------------------------- */
#define MAXPRE 12000   /* (pre-delay, samples: 200 ms at 48 kHz + room) */
typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kOwn];
    float fs;
    int ready, program;
    AEffect *nam;
    volatile int update_display, report_gr;
    volatile unsigned char automate[kNumParams];
    /* EQ */
    Bq hpf[2], hum[3][2], ls[2], pk[NPEAK][2], hs[2], lpf[2][2];
    BqCo hpfc, humc[3], lsc, pkc[NPEAK], hsc, lpfc[2];
    int pk_on[NPEAK], ls_on, hs_on, hum_n;
    float co_key;
    /* comp */
    float env, gr_db, gr_shown;
    int gr_tick;
    /* halls */
    Verb verb[2];
    Line pre[2][2];
    float rv_key[2];
    /* levels, smoothed */
    float in_s, mk_s, rig_s, vol_s, eq_s, amp_s, rv_s[2];
    char *chunk;
} Plugin;

static Plugin *all_plugins[64];
static float val(const Plugin *s, int i) { return pv(&P[i], s->norm[i]); }

static void reset(Plugin *s) {
    memset(s->hpf, 0, sizeof s->hpf); memset(s->hum, 0, sizeof s->hum); memset(s->ls, 0, sizeof s->ls);
    memset(s->pk, 0, sizeof s->pk); memset(s->hs, 0, sizeof s->hs); memset(s->lpf, 0, sizeof s->lpf);
    s->env = 0; s->gr_db = 0;
    for (int r = 0; r < 2; r++) { verb_clear(&s->verb[r]); line_clear(&s->pre[r][0]); line_clear(&s->pre[r][1]); }
}
static void coefficients(Plugin *s) {
    const float fs = s->fs;
    s->hpfc = bq_co(BQ_HP, val(s, kHpfHz), 0.7071f, 0, fs);
    const int hum = (int)val(s, kHum);
    s->hum_n = hum ? (int)val(s, kHumHarm) : 0;
    for (int h = 0; h < s->hum_n; h++) s->humc[h] = bq_co(BQ_PEAK, (hum == 1 ? 50.0f : 60.0f) * (float)(h + 1), val(s, kHumQ), val(s, kHumDepth), fs);
    s->ls_on = fabsf(val(s, kLsGain)) > 0.01f;
    s->lsc = bq_co(BQ_LSHELF, val(s, kLsHz), 0.7071f, val(s, kLsGain), fs);
    for (int b = 0; b < NPEAK; b++) {
        const float g = val(s, PEAK(b, PK_GAIN));
        s->pk_on[b] = fabsf(g) > 0.01f;
        s->pkc[b] = bq_co(BQ_PEAK, val(s, PEAK(b, PK_HZ)), val(s, PEAK(b, PK_Q)), g, fs);
    }
    s->hs_on = fabsf(val(s, kHsGain)) > 0.01f;
    s->hsc = bq_co(BQ_HSHELF, val(s, kHsHz), 0.7071f, val(s, kHsGain), fs);
    /* low-pass: 12 dB = one Butterworth stage; 24 dB = two (Q 0.541 and 1.307: a 4th-order Butterworth) */
    const float lh = val(s, kLpfHz);
    if ((int)val(s, kLpf) == 2) { s->lpfc[0] = bq_co(BQ_LP, lh, 0.5412f, 0, fs); s->lpfc[1] = bq_co(BQ_LP, lh, 1.3066f, 0, fs); }
    else s->lpfc[0] = bq_co(BQ_LP, lh, 0.7071f, 0, fs);
}
static void hall_set(Plugin *s, int r) {
    const float decay = val(s, REV(r, RV_DECAY)) / 100, tone = val(s, REV(r, RV_TONE)) / 100;
    verb_set(&s->verb[r], 1.6f, 0.5f * powf(16, decay), 1500 * powf(8, tone), s->fs);   /* a concert hall: 0.5 .. 8 s */
}
static int setup(Plugin *s, float fs) {
    s->ready = 0;
    s->fs = fs;
    int ok = 1;
    for (int r = 0; r < 2; r++) {
        ok &= verb_init(&s->verb[r], fs);
        for (int c = 0; c < 2; c++) ok &= line_init(&s->pre[r][c], (int)(fs * 0.21f) + 8);
        s->rv_key[r] = -1;
    }
    s->co_key = -1;
    if (ok) reset(s);
    s->ready = ok;
    return ok;
}

/* ---- sound ------------------------------------------------------------------------------------------------------ */
#define BLK 512
static float eq_run(Plugin *s, int c, float y) {
    if (s->norm[kHpfOn] > 0.5f) y = bq(&s->hpf[c], &s->hpfc, y);
    for (int h = 0; h < s->hum_n; h++) y = bq(&s->hum[h][c], &s->humc[h], y);
    if (s->ls_on) y = bq(&s->ls[c], &s->lsc, y);
    for (int b = 0; b < NPEAK; b++) if (s->pk_on[b]) y = bq(&s->pk[b][c], &s->pkc[b], y);
    if (s->hs_on) y = bq(&s->hs[c], &s->hsc, y);
    const int lpf = (int)val(s, kLpf);
    if (lpf) { y = bq(&s->lpf[0][c], &s->lpfc[0], y); if (lpf == 2) y = bq(&s->lpf[1][c], &s->lpfc[1], y); }
    return y;
}
static void comp_run(Plugin *s, float *l, float *r, int m) {
    const float thr = val(s, kThresh), ratio = val(s, kRatio), knee = 6;
    const float aa = 1 - expf(-1 / (val(s, kAttack) * 0.001f * s->fs)), ar = 1 - expf(-1 / (val(s, kRelease) * 0.001f * s->fs));
    const float mk = db(val(s, kMakeup));
    for (int k = 0; k < m; k++) {
        const float x = fmaxf(fabsf(l[k]), fabsf(r[k]));
        s->env += (x > s->env ? aa : ar) * (x - s->env);
        const float lv = 20 * log10f(s->env + 1e-9f), over = lv - thr;
        float gr = 0;   /* (dB of gain reduction, soft knee) */
        if (over >= knee / 2) gr = over * (1 - 1 / ratio);
        else if (over > -knee / 2) { const float t = over + knee / 2; gr = (1 - 1 / ratio) * t * t / (2 * knee); }
        s->gr_db = gr;
        if (gr > s->gr_shown) s->gr_shown = gr;
        s->mk_s += 0.002f * (mk - s->mk_s);
        const float g = db(-gr) * s->mk_s;
        l[k] *= g; r[k] *= g;
    }
}
static void hall_run(Plugin *s, int rv, float *l, float *r, int m) {
    const float want = s->norm[REV(rv, RV_ON)] > 0.5f ? val(s, REV(rv, RV_MIX)) / 100 : 0;
    if (want == 0 && s->rv_s[rv] < 1e-4f) { s->rv_s[rv] = 0; return; }
    const int pre = (int)(val(s, REV(rv, RV_PRE)) * 0.001f * s->fs);
    for (int k = 0; k < m; k++) {
        s->rv_s[rv] += 0.002f * (want - s->rv_s[rv]);
        line_push(&s->pre[rv][0], l[k]); line_push(&s->pre[rv][1], r[k]);
        float wl, wr;
        verb_run(&s->verb[rv], line_tap(&s->pre[rv][0], pre), line_tap(&s->pre[rv][1], pre), &wl, &wr);
        const float mx = s->rv_s[rv];
        l[k] = l[k] * (1 - mx) + wl * mx * 1.2f;   /* (wet against dry, as the HeadRush's Mix) */
        r[k] = r[k] * (1 - mx) + wr * mx * 1.2f;
    }
}
static void process(AEffect *e, float **in, float **out, int32_t n) {
    VC_FLUSH_DENORMALS_BEGIN;
    Plugin *s = (Plugin *)e->object;
    if (s->master) {
        for (int i = 0; i < kNumParams; i++)
            if (s->automate[i]) { s->automate[i] = 0; s->master(e, audioMasterAutomate, i, 0, NULL, i < kOwn ? s->norm[i] : s->nam->getParameter(s->nam, i - kNam0)); s->update_display = 1; }
        if (s->report_gr) { s->report_gr = 0; s->master(e, audioMasterAutomate, kGr, 0, NULL, s->norm[kGr]); }
        if (s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    }
    const float *il = in && in[0] ? in[0] : NULL, *ir = in && in[1] ? in[1] : il;
    if (!s->ready) { for (int k = 0; k < n; k++) out[0][k] = out[1][k] = 0; goto done; }
    {
        float key = 0;
        for (int i = kEqOn; i < kRev0; i++) key += s->norm[i] * (float)(i * 7 + 1);
        if (key != s->co_key) { s->co_key = key; coefficients(s); }
        for (int r = 0; r < 2; r++) {
            const float rk = s->norm[REV(r, RV_DECAY)] * 3 + s->norm[REV(r, RV_TONE)] * 7;
            if (rk != s->rv_key[r]) { s->rv_key[r] = rk; hall_set(s, r); }
        }
    }
    const float ig = db(val(s, kInput)), rg = db(val(s, kRigVol)), vv = val(s, kVolume) / 100, vg = powf(vv, 2.5f);
    const int comp = s->norm[kCompOn] > 0.5f, after = comp && (int)val(s, kCompPos) == 1, before = comp && !after;
    const float eq_on = s->norm[kEqOn] > 0.5f ? 1 : 0, amp_on = s->norm[kAmpOn] > 0.5f ? 1 : 0;
    for (int off = 0; off < n; off += BLK) {
        const int m = n - off < BLK ? n - off : BLK;
        float l[BLK], r[BLK], tl[BLK], tr[BLK];
        for (int k = 0; k < m; k++) {
            float a = il ? il[off + k] : 0, b = ir ? ir[off + k] : a;
            if (!finite_bits(a) || a > 8 || a < -8) a = 0;   /* only clean audio in */
            if (!finite_bits(b) || b > 8 || b < -8) b = 0;
            s->in_s += 0.002f * (ig - s->in_s);
            l[k] = a * s->in_s; r[k] = b * s->in_s;
        }
        if (before) comp_run(s, l, r, m);
        for (int k = 0; k < m; k++) {
            s->eq_s += 0.002f * (eq_on - s->eq_s);
            if (s->eq_s < 1e-4f && eq_on == 0) continue;
            const float yl = eq_run(s, 0, l[k]), yr = eq_run(s, 1, r[k]);
            l[k] += (yl - l[k]) * s->eq_s;
            r[k] += (yr - r[k]) * s->eq_s;
        }
        if (after) comp_run(s, l, r, m);
        if (!comp) s->gr_db = 0;
        hall_run(s, 0, l, r, m);
        if (amp_on > 0 || s->amp_s > 1e-4f) {   /* the amp (NAM), faded in and out */
            float *pi[2] = {l, r}, *po[2] = {tl, tr};
            s->nam->processReplacing(s->nam, pi, po, m);
            for (int k = 0; k < m; k++) {
                s->amp_s += 0.002f * (amp_on - s->amp_s);
                l[k] += (tl[k] - l[k]) * s->amp_s;
                r[k] += (tr[k] - r[k]) * s->amp_s;
            }
        }
        hall_run(s, 1, l, r, m);
        int bad = 0;
        for (int k = 0; k < m; k++) {
            s->rig_s += 0.002f * (rg - s->rig_s);
            s->vol_s += 0.002f * (vg - s->vol_s);
            const float g = s->rig_s * s->vol_s, a = l[k] * g, b = r[k] * g;
            if (!finite_bits(a) || !finite_bits(b) || fabsf(a) > 63 || fabsf(b) > 63) bad = 1;   /* past +36 dB or not a number */
            out[0][off + k] = bad ? 0 : clampf(a, -4, 4);   /* +12 dB ceiling */
            out[1][off + k] = bad ? 0 : clampf(b, -4, 4);
        }
        if (bad) { reset(s); for (int k = 0; k < m; k++) out[0][off + k] = out[1][off + k] = 0; }
    }
    if (++s->gr_tick >= 4) {   /* the GR readout: the most since it last moved */
        s->gr_tick = 0;
        const float g = clampf((comp ? s->gr_shown : 0) / 30, 0, 1);
        if (fabsf(g - s->norm[kGr]) > 0.004f) { s->norm[kGr] = g; s->report_gr = 1; }
        s->gr_shown = 0;
    }
done:
    VC_FLUSH_DENORMALS_END;
}

/* ---- programs ----------------------------------------------------------------------------------------------- */
static void set_own(Plugin *s, int i, float v) { s->norm[i] = pn(&P[i], v); s->automate[i] = 1; }
static void set_nam(Plugin *s, int i, float norm) { s->nam->setParameter(s->nam, i, clampf(norm, 0, 1)); s->automate[kNam0 + i] = 1; }
static void load_program(Plugin *s, int idx) {
    if (idx < 0 || idx >= NPROGS) return;
    const Prog *g = &PROGS[idx];
    set_own(s, kInput, g->input);
    set_own(s, kCompOn, (float)g->comp); set_own(s, kCompPos, (float)g->comp_pos); set_own(s, kThresh, g->thr);
    set_own(s, kRatio, g->ratio); set_own(s, kAttack, g->att); set_own(s, kRelease, g->rel); set_own(s, kMakeup, g->makeup);
    set_own(s, kEqOn, 1); set_own(s, kHpfOn, (float)g->hpf); set_own(s, kHpfHz, g->hpf_hz);
    set_own(s, kHum, (float)g->hum); set_own(s, kHumDepth, g->hum_depth); set_own(s, kHumQ, g->hum_q); set_own(s, kHumHarm, 1);
    set_own(s, kLsHz, g->ls_hz); set_own(s, kLsGain, g->ls_gain);
    static const float SPOT[NPEAK] = {119, 250, 496, 1000, 1800, 2400, 4000, 5500};   /* (an unused peak: flat, at its spot) */
    for (int b = 0; b < NPEAK; b++) {
        const Pk *p = &g->pk[b];
        set_own(s, PEAK(b, PK_HZ), p->hz > 0 ? p->hz : SPOT[b]);
        set_own(s, PEAK(b, PK_GAIN), p->gain);
        set_own(s, PEAK(b, PK_Q), p->q);
    }
    set_own(s, kHsHz, g->hs_hz); set_own(s, kHsGain, g->hs_gain);
    set_own(s, kLpf, (float)g->lpf); set_own(s, kLpfHz, g->lpf_hz);
    for (int r = 0; r < 2; r++) {
        set_own(s, REV(r, RV_ON), (float)g->rev[r]); set_own(s, REV(r, RV_MIX), g->mix); set_own(s, REV(r, RV_PRE), g->pre);
        set_own(s, REV(r, RV_DECAY), g->decay); set_own(s, REV(r, RV_TONE), g->tone);
    }
    set_own(s, kAmpOn, (float)g->amp);
    set_own(s, kRigVol, g->rig_vol);
    /* NAM MPC's knobs (its params 0 Input -24..24 dB, 1 Gate -100..-20 dB, 2-4 Bass/Middle/Treble 0..10, 5 Output -24..24
     * dB, 21 Presence 0..10); the model and IR stay */
    set_nam(s, 0, (g->nam_in + 24) / 48); set_nam(s, 1, (g->nam_gate + 100) / 80); set_nam(s, 2, g->nam_bass / 10);
    set_nam(s, 3, g->nam_mid / 10); set_nam(s, 4, g->nam_treble / 10); set_nam(s, 5, (g->nam_out + 24) / 48); set_nam(s, 21, 0.5f);
    if (g->capture) nam_find(s->nam, g->capture, NULL);   /* (the capture the rig used, if it's in the user's library) */
    s->program = idx;
    s->norm[kProgram] = (float)idx / (float)(NPROGS - 1);
    s->automate[kProgram] = 1;
    s->update_display = 1;
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams || i == kGr) return;   /* (GR is read-only) */
    v = clampf(v, 0, 1);
    if (i >= kNam0) { s->nam->setParameter(s->nam, i - kNam0, v); return; }
    if (i == kProgPrev || i == kProgNext) {   /* momentary buttons */
        if (v > 0.5f) load_program(s, s->program < 0 ? (i == kProgPrev ? NPROGS - 1 : 0) : (s->program + (i == kProgPrev ? -1 : 1) + NPROGS) % NPROGS);
        s->norm[i] = 0;
        return;
    }
    if (i == kProgram) {   /* a pick or a project restoring (exact: the name only), or a Q-Link nudge (loads the next) */
        const float pos = v * (float)(NPROGS - 1), cur = s->program >= 0 ? (float)s->program : s->norm[i] * (float)(NPROGS - 1);
        if (fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1) { load_program(s, ((int)lrintf(cur) + (pos > cur ? 1 : -1) + NPROGS) % NPROGS); return; }
        s->program = (int)lrintf(pos);
        s->norm[i] = v;
        s->update_display = 1;
        return;
    }
    s->norm[i] = snap(&P[i], v, s->norm[i]);
    if (P[i].kind == K_OPT || P[i].kind == K_TOGGLE || P[i].kind == K_INT) s->update_display = 1;
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *s = (Plugin *)e->object;
    if (i >= kNam0 && i < kNumParams) return s->nam->getParameter(s->nam, i - kNam0);
    return i >= 0 && i < kOwn ? s->norm[i] : 0;
}
static void display(Plugin *s, int i, char *t) {
    if (i >= kNam0) { char b[96] = {0}; s->nam->dispatcher(s->nam, effGetParamDisplay, i - kNam0, 0, b, 0); snprintf(t, 24, "%s", b); return; }
    if (i == kProgram) { snprintf(t, 24, "%s", s->program >= 0 && s->program < NPROGS ? PROGS[s->program].name : "-"); return; }
    if (i == kProgPrev) { snprintf(t, 24, "<"); return; }
    if (i == kProgNext) { snprintf(t, 24, ">"); return; }
    if (i == kGr) { snprintf(t, 24, "-%.1f dB", s->norm[i] * 30); return; }
    if (i == kRatio) { snprintf(t, 24, "%.1f:1", val(s, i)); return; }
    if (i == kHumQ || (i >= kPeak0 && i < kHsHz && (i - kPeak0) % PKP == PK_Q)) { snprintf(t, 24, "%.2f", val(s, i)); return; }
    if (i == kHumHarm) { static const char *const H[] = {"Fundamental", "+ 2nd", "+ 2nd & 3rd"}; snprintf(t, 24, "%s", H[(int)val(s, i) - 1]); return; }
    if (i == kVolume) { const float v = val(s, i) / 100; if (v <= 0) snprintf(t, 24, "Off"); else snprintf(t, 24, "%.0f%% (%.0f dB)", v * 100, 50 * log10f(v)); return; }
    pdisplay(&P[i], s->norm[i], t);
}

/* ---- state: our values (text) and NAM's own chunk --------------------------------------------------------------- */
static int get_chunk(Plugin *s, void **out) {
    void *nc = NULL;
    const intptr_t nn = s->nam->dispatcher(s->nam, effGetChunk, 0, 0, &nc, 0);
    const size_t cap = 64 + (size_t)kOwn * 16 + (nn > 0 ? (size_t)nn : 0);
    free(s->chunk);
    s->chunk = malloc(cap);
    if (!s->chunk) return 0;
    size_t o = (size_t)snprintf(s->chunk, cap, "ERGO1 %d\n", kOwn);
    for (int i = 0; i < kOwn; i++) o += (size_t)snprintf(s->chunk + o, cap - o, "%.6f\n", s->norm[i]);
    o += (size_t)snprintf(s->chunk + o, cap - o, "NAM %ld\n", (long)(nn > 0 ? nn : 0));
    if (nn > 0 && nc) { memcpy(s->chunk + o, nc, (size_t)nn); o += (size_t)nn; }
    *out = s->chunk;
    return (int)o;
}
static void set_chunk(Plugin *s, const char *c, size_t n) {
    int np = 0, used = 0;
    if (n < 8 || sscanf(c, "ERGO1 %d\n%n", &np, &used) != 1 || np < 1 || np > kOwn) return;
    size_t o = (size_t)used;
    for (int i = 0; i < np; i++) {
        int u = 0;
        float x;
        if (o >= n || sscanf(c + o, "%f\n%n", &x, &u) != 1) return;
        o += (size_t)u;
        if (i == kProgram) { s->norm[i] = clampf(x, 0, 1); s->program = (int)lrintf(s->norm[i] * (float)(NPROGS - 1)); }
        else if (i != kProgPrev && i != kProgNext && i != kGr) s->norm[i] = clampf(x, 0, 1);
    }
    long nn = 0;
    int u = 0;
    if (o < n && sscanf(c + o, "NAM %ld\n%n", &nn, &u) == 1 && nn > 0 && o + (size_t)u + (size_t)nn <= n)
        s->nam->dispatcher(s->nam, effSetChunk, 0, (intptr_t)nn, (void *)(c + o + (size_t)u), 0);
    s->update_display = 1;
}

/* ---- the host ------------------------------------------------------------------------------------------------- */
static intptr_t engine_host(AEffect *e, int32_t op, int32_t idx, intptr_t val_, void *p, float opt) {   /* NAM's calls */
    (void)val_; (void)p; (void)opt;
    if (op == 1 /* audioMasterVersion */) return 2400;
    for (int i = 0; i < 64; i++) {
        Plugin *s = all_plugins[i];
        if (!s || s->nam != e) continue;
        if (op == audioMasterUpdateDisplay) s->update_display = 1;
        else if (op == audioMasterAutomate && idx >= 0 && idx < NAM_N) s->automate[kNam0 + idx] = 1;
        return 1;
    }
    return 0;
}
static void close_all(Plugin *s) {
    if (s->nam) s->nam->dispatcher(s->nam, effClose, 0, 0, NULL, 0);
    for (int i = 0; i < 64; i++) if (all_plugins[i] == s) all_plugins[i] = NULL;
    for (int r = 0; r < 2; r++) { verb_free(&s->verb[r]); line_free(&s->pre[r][0]); line_free(&s->pre[r][1]); }
    free(s->chunk);
    free(s);
}
static void midi(Plugin *s, const unsigned char *m) {
    const int st = m[0] & 0xf0;
    if (st == 0xb0 && (m[1] == 7 || m[1] == 11)) { s->norm[kVolume] = (float)m[2] / 127; s->automate[kVolume] = 1; }   /* expression / volume */
    else if (st == 0xc0) load_program(s, m[1]);   /* Program Change n: program n + 1 */
}
static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: close_all(s); return 1;
    case effGetParamName:
        if (idx >= kNam0 && idx < kNumParams) { char t[64] = {0}; s->nam->dispatcher(s->nam, effGetParamName, idx - kNam0, 0, t, 0); copy_str(p, t, 32); }
        else if (idx >= 0 && idx < kOwn) copy_str(p, P[idx].name, 32);
        return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay: if (idx >= 0 && idx < kNumParams) { char t[64] = {0}; display(s, idx, t); copy_str(p, t, 24); } return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams && idx != kGr;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "Ergo EUB", 32); return 1;
    case effGetVendorString: copy_str(p, VC_VENDOR, 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate:
        if (opt > 1000 && fabsf(opt - s->fs) > 0.5f) setup(s, opt);
        s->nam->dispatcher(s->nam, effSetSampleRate, 0, 0, NULL, opt);
        return 1;
    case effSetBlockSize: s->nam->dispatcher(s->nam, effSetBlockSize, 0, v, NULL, 0); return 1;
    case effMainsChanged: if (v) reset(s); s->nam->dispatcher(s->nam, effMainsChanged, 0, v, NULL, 0); return 1;
    case effGetChunk: return get_chunk(s, (void **)p);
    case effSetChunk: if (v > 0 && p) set_chunk(s, (const char *)p, (size_t)v); return 1;
    case effProcessEvents: {
        const VstEvents *ev = (const VstEvents *)p;
        for (int k = 0; ev && k < ev->numEvents; k++) if (ev->events[k] && ev->events[k]->type == 1) midi(s, ev->events[k]->midiData);
        return 1;
    }
    case effCanDo: return p && (!strcmp((const char *)p, "receiveVstEvents") || !strcmp((const char *)p, "receiveVstMidiEvent")) ? 1 : -1;
    default: return 0;
    }
}

/* the full-page lists (tools/picklist.h): the programs, then NAM's folders, models, IR folders and IRs */
static int ergo_pick(AEffect *e, int list, int op, int i, char *out, int max) {
    Plugin *s = (Plugin *)e->object;
    if (list > 0) return nam_pick(s->nam, list - 1, op, i, out, max);
    if (op == PL_COUNT) return NPROGS;
    if (op == PL_CURRENT) return s->program;
    if (op == PL_HEADING || i < 0 || i >= NPROGS) return 0;
    if (op == PL_NAME) snprintf(out, (size_t)max, "%s", PROGS[i].name);
    else if (op == PL_CHOOSE) load_program(s, i);
    return 0;
}
static const char *const LIST_KEYS[] = {"program", "amp_folder", "amp_model", "amp_ir_folder", "amp_ir"};
static const char *const LIST_TITLES[] = {"Programs", "Model Folders", "Models", "IR Folders", "IRs"};
static const PickLists LISTS = {5, LIST_KEYS, LIST_TITLES, ergo_pick, 1};   /* (1: MIDI Ch, for the expression pedal) */

static AEffect *make(audioMasterCallback master) {
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    int slot = -1;
    for (int i = 0; i < 64 && slot < 0; i++) if (!all_plugins[i]) slot = i;
    if (slot < 0) { free(s); return NULL; }
    all_plugins[slot] = s;
    s->master = master;
    s->nam = nam_entry(engine_host);
    if (!s->nam) { close_all(s); return NULL; }
    for (int i = 0; i < kOwn; i++) s->norm[i] = pn(&P[i], P[i].def);
    s->program = -1;
    if (!setup(s, 44100)) { close_all(s); return NULL; }
    load_program(s, DEFAULT_PROG);
    memset((void *)s->automate, 0, sizeof s->automate);
    s->in_s = db(val(s, kInput)); s->mk_s = db(val(s, kMakeup)); s->rig_s = db(val(s, kRigVol));
    s->vol_s = powf(val(s, kVolume) / 100, 2.5f); s->eq_s = 1; s->amp_s = s->norm[kAmpOn] > 0.5f ? 1 : 0;
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
    e->uniqueID = 0x4552474f;   /* 'ERGO' */
    e->version = 1000;
    return e;
}
__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) { return pl_wrap(master, make, &LISTS); }

#ifdef ERGO_PARAMS
/* params.json: our table, then NAM MPC's own entries (argv[1]) with "amp_" in front of their keys, then the lists */
static const char *const NAM_KEYS[NAM_N] = {"input", "gate", "bass", "middle", "treble", "output", "model", "model_prev",
    "model_next", "cab", "ir", "ir_prev", "ir_next", "normalize", "size", "folder", "folder_prev", "folder_next",
    "ir_folder", "ir_folder_prev", "ir_folder_next", "presence"};
int main(int argc, char **argv) {
    printf("{\n  \"name\": \"Ergo EUB\",\n  \"params\": [\n");
    for (int i = 0; i < kOwn; i++) {
        const ParamDef *p = &P[i];
        printf("    {\"key\": \"%s\", \"name\": \"%s\"", p->key, p->name);
        if (p->kind == K_OPT) {
            printf(", \"options\": [");
            for (int k = 0; k < p->nopts; k++) printf("%s\"%s\"", k ? ", " : "", p->opts[k]);
            printf("], \"default\": %d", (int)p->def);
        } else if (p->kind == K_TOGGLE) printf(", \"options\": [\"Off\", \"On\"], \"default\": %d", (int)p->def);
        else if (p->kind == K_BUTTON) printf(", \"options\": [\"Off\", \"On\"], \"momentary\": true");
        else if (p->kind == K_PRESET) printf(", \"min\": 0, \"max\": 1, \"display\": \"string\", \"type\": \"stepper\"");
        else printf(", \"min\": %g, \"max\": %g, \"default\": %g%s%s%s", p->lo, p->hi, p->def, p->unit[0] ? ", \"unit\": \"" : "",
                    p->unit, p->unit[0] ? "\"" : "");
        printf("},\n");
    }
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
        if (*q != '{') continue;
        char *end = strchr(q, '}');
        if (!end) return 1;
        *end = 0;
        char *key = strstr(q, "\"key\"");
        char *colon = key ? strchr(key, ':') : NULL, *q1 = colon ? strchr(colon, '"') : NULL;
        if (!q1 || count >= NAM_N) return 1;
        printf("    {");
        fwrite(q + 1, 1, (size_t)(q1 + 1 - (q + 1)), stdout);
        printf("amp_");
        fputs(q1 + 1, stdout);
        printf("}%s", count < NAM_N - 1 ? ",\n" : "");
        if (strncmp(q1 + 1, NAM_KEYS[count], strlen(NAM_KEYS[count])) || q1[1 + strlen(NAM_KEYS[count])] != '"') { fprintf(stderr, "NAM's params.json order changed\n"); return 1; }
        count++;
        q = end;
    }
    pl_params_json(&LISTS, stdout);
    printf("\n  ]\n}\n");
    return count == NAM_N ? 0 : 1;
}
#endif
