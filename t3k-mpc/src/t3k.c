/* TONE3000 browser for MPC OS (VST2 effect, the audio passes straight through): browse TONE3000's NAM captures and
 * IRs on the MPC's own screen and download them onto the card, HeadRush-Cloud style. Our own code; TONE3000's
 * public API (www.tone3000.com/api, version 1), as an independent developer integration: "Powered by TONE3000", the
 * creator shown with each tone, the user's own sign-in, models downloaded only when the user asks.
 *   Sign in: Device Authorization (RFC 8628): the page shows a code to enter at tone3000.com/activate; the tokens
 *     are kept in /data/other-things/t3k-token (0600) and refreshed when they run out.
 *   Browse: Source (All / Amps / Full Rigs / Pedals / IRs / Favorites / Downloaded), Sort, Brand and Style (words
 *     for the search) -> Find: up to 50 tones (NAM A2 only, or IRs) in the Tone stepper and its full-page list.
 *   A tone: its creator, gear, sizes, downloads; its models (A2) in the Model stepper (first entry: all of them).
 *   Download: into <card>/NAM/Models/<tone>/ (.nam) or <card>/NAM/IRs/<tone>/ (.wav), the first mounted drive that
 *     has a NAM folder (else /sdcard/NAM); then T3K_STAMP is touched so NAM MPC / 0dd Rig / Ergo EUB rescan.
 * Networking: the MPC's own libcurl, opened at run time, on a worker thread (never the audio or UI thread). */
#define _GNU_SOURCE
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include "vocal_core.h"
#include "t3k_json.h"

#define T3K_CLIENT_ID "t3k_pub_qJ7M_267j8PLELjFhYezlKM-AXmaj71e"   /* the publishable key (made for devices) */
#define T3K_API "https://www.tone3000.com/api/v1"
#define T3K_STAMP "/tmp/t3k-downloaded"
#define MAXT 50     /* tones in a result */
#define MAXM 120    /* models of a tone */

static const char *const SOURCES[] = {"All", "Amps", "Full Rigs", "Pedals", "IRs", "Favorites", "Downloaded"};
static const char *const SORTS[] = {"Trending", "Most Downloaded", "Newest", "Best Match"};
static const char *const BRANDS[] = {"Any Brand", "Fender", "Marshall", "Vox", "Mesa", "Peavey", "EVH", "Orange", "Friedman",
                                     "Bogner", "Soldano", "Diezel", "Dumble", "Matchless", "Two Rock", "Hiwatt", "Supro",
                                     "Ampeg", "Darkglass", "Tube Screamer", "Klon", "Big Muff", "Boss", "Rat"};
static const char *const STYLES[] = {"Any Style", "Clean", "Edge of Breakup", "Crunch", "High Gain", "Metal", "Blues",
                                     "Bass", "Fuzz", "Boost"};
/* (parameter 0 is a choice, not a button: the MPC nudges parameter 0 to 1.0 and back on every prepare, which would
 * look like a press, and picklist.h holds such a nudge back) */
enum { kSource, kSort, kBrand, kStyle, kSignIn, kSignOut, kFind, kTone, kTonePrev, kToneNext, kModel, kModelPrev,
       kModelNext, kDownload, kStatus, kStatus2, kInfo1, kInfo2, kInfo3, kPowered, kNumParams };
static const ParamDef P[kNumParams] = {
    OPT("source", "Source", SOURCES, 0), OPT("sort", "Sort", SORTS, 0), OPT("brand", "Brand", BRANDS, 0),
    OPT("style", "Style", STYLES, 0), BUTTON("signin", "Sign In"), BUTTON("signout", "Sign Out"), BUTTON("find", "Find"), PRESET("tone", "Tone"), BUTTON("tone_prev", "Tone <"), BUTTON("tone_next", "Tone >"),
    PRESET("model", "Model"), BUTTON("model_prev", "Model <"), BUTTON("model_next", "Model >"),
    BUTTON("download", "Download"), PRESET("status", "Status"), PRESET("status2", "Status 2"),
    PRESET("info1", "Creator"), PRESET("info2", "Gear"), PRESET("info3", "License"), PRESET("powered", "Powered"),
};

typedef struct { long id; char title[96], creator[48], gear[24], sizes[48], license[16], format[8]; long downloads, a2, irs; } Tone;
typedef struct { long id; char name[96], url[300], arch[8]; } Model;
enum { J_NONE, J_SIGNIN, J_FIND, J_MODELS, J_DOWNLOAD };

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    volatile int automate[kNumParams], update_display;
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int quit, job, busy;
    /* what the page shows (under mu) */
    char status[64], status2[64];
    Tone tones[MAXT];
    int ntones, tone;
    Model models[MAXM];
    int nmodels, model;   /* model 0 = all of them; 1.. = models[model - 1] */
    char user[48];
    /* the sign-in (the worker's) */
    char access[1024], refresh[1024];
    long expires;
} Plugin;

/* ---- the MPC's libcurl, opened when first needed ------------------------------------------------------------- */
static struct {
    int ok;
    void *(*easy_init)(void);
    int (*easy_setopt)(void *, int, ...);
    int (*easy_perform)(void *);
    int (*easy_getinfo)(void *, int, ...);
    void (*easy_cleanup)(void *);
    void *(*slist_append)(void *, const char *);
    void (*slist_free_all)(void *);
    char *(*easy_escape)(void *, const char *, int);
    void (*free_)(void *);
} C;
static pthread_mutex_t curl_mu = PTHREAD_MUTEX_INITIALIZER;
static int curl_load(void) {
    pthread_mutex_lock(&curl_mu);
    if (!C.ok) {
        void *h = dlopen("libcurl.so.4", RTLD_NOW);
        if (h) {
            *(void **)&C.easy_init = dlsym(h, "curl_easy_init");
            *(void **)&C.easy_setopt = dlsym(h, "curl_easy_setopt");
            *(void **)&C.easy_perform = dlsym(h, "curl_easy_perform");
            *(void **)&C.easy_getinfo = dlsym(h, "curl_easy_getinfo");
            *(void **)&C.easy_cleanup = dlsym(h, "curl_easy_cleanup");
            *(void **)&C.slist_append = dlsym(h, "curl_slist_append");
            *(void **)&C.slist_free_all = dlsym(h, "curl_slist_free_all");
            *(void **)&C.easy_escape = dlsym(h, "curl_easy_escape");
            *(void **)&C.free_ = dlsym(h, "curl_free");
            C.ok = C.easy_init && C.easy_setopt && C.easy_perform && C.easy_getinfo && C.easy_cleanup && C.slist_append &&
                   C.slist_free_all && C.easy_escape && C.free_;
        }
    }
    pthread_mutex_unlock(&curl_mu);
    return C.ok;
}
typedef struct { char *b; size_t n, cap; FILE *f; } Buf;
static size_t on_data(char *p, size_t sz, size_t nm, void *u) {
    Buf *b = u;
    const size_t n = sz * nm;
    if (b->f) return fwrite(p, 1, n, b->f);
    if (b->n + n + 1 > b->cap) {
        if (b->n + n > (8u << 20)) return 0;   /* (no answer of theirs is this big) */
        size_t cap = b->cap ? b->cap * 2 : 16384;
        while (cap < b->n + n + 1) cap *= 2;
        char *x = realloc(b->b, cap);
        if (!x) return 0;
        b->b = x; b->cap = cap;
    }
    memcpy(b->b + b->n, p, n);
    b->n += n;
    b->b[b->n] = 0;
    return n;
}
/* one request: GET (form NULL) or a form POST; bearer: a token or NULL; to a buffer or a file. Returns the HTTP
 * status (0: no connection). */
static long http(const char *url, const char *form, const char *bearer, Buf *out) {
    if (!curl_load()) return 0;
    enum { URL = 10002, WRITEFUNCTION = 20011, WRITEDATA = 10001, HTTPHEADER = 10023, POSTFIELDS = 10015,
           FOLLOWLOCATION = 52, CONNECTTIMEOUT = 78, TIMEOUT = 13, USERAGENT = 10018, NOSIGNAL = 99,
           LOW_SPEED_LIMIT = 19, LOW_SPEED_TIME = 20, RESPONSE_CODE = 0x200002 };
    void *h = C.easy_init();
    if (!h) return 0;
    void *hdr = C.slist_append(NULL, "Accept: application/json");
    char auth[1100];
    if (bearer && bearer[0]) { snprintf(auth, sizeof auth, "Authorization: Bearer %s", bearer); hdr = C.slist_append(hdr, auth); }
    C.easy_setopt(h, URL, url);
    C.easy_setopt(h, WRITEFUNCTION, on_data);
    C.easy_setopt(h, WRITEDATA, out);
    C.easy_setopt(h, HTTPHEADER, hdr);
    if (form) C.easy_setopt(h, POSTFIELDS, form);
    C.easy_setopt(h, FOLLOWLOCATION, 1L);
    C.easy_setopt(h, NOSIGNAL, 1L);
    C.easy_setopt(h, CONNECTTIMEOUT, 20L);
    C.easy_setopt(h, TIMEOUT, out->f ? 600L : 60L);
    C.easy_setopt(h, LOW_SPEED_LIMIT, 500L);
    C.easy_setopt(h, LOW_SPEED_TIME, 60L);
    C.easy_setopt(h, USERAGENT, "OtherThings-MPC-TONE3000/1.0");
    long code = 0;
    if (C.easy_perform(h) == 0) C.easy_getinfo(h, RESPONSE_CODE, &code);
    C.easy_cleanup(h);
    C.slist_free_all(hdr);
    return code;
}
static void url_escape(const char *in, char *out, size_t max) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < max; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p)) out[o++] = (char)*p;
        else o += (size_t)snprintf(out + o, max - o, "%%%02X", *p);
    }
    out[o] = 0;
}

/* ---- page text ------------------------------------------------------------------------------------------------- */
static void say(Plugin *s, const char *a, const char *b) {
    pthread_mutex_lock(&s->mu);
    snprintf(s->status, sizeof s->status, "%s", a ? a : "");
    snprintf(s->status2, sizeof s->status2, "%s", b ? b : "");
    pthread_mutex_unlock(&s->mu);
    s->automate[kStatus] = s->automate[kStatus2] = 1;
    s->update_display = 1;
}

/* ---- the sign-in: tokens kept on the MPC's internal storage ----------------------------------------------------- */
static const char *token_path(void) {
    static char p[256];
    if (!p[0] && getenv("T3K_TOKEN_FILE")) snprintf(p, sizeof p, "%s", getenv("T3K_TOKEN_FILE"));   /* (tests) */
    if (!p[0]) {
        mkdir("/data/other-things", 0700);
        snprintf(p, sizeof p, "%s", access("/data/other-things", W_OK) ? "/tmp/t3k-token" : "/data/other-things/t3k-token");
    }
    return p;
}
static void token_save(Plugin *s) {
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.new", token_path());
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fchmod(fileno(f), 0600);
    fprintf(f, "%s\n%s\n%ld\n%s\n", s->access, s->refresh, s->expires, s->user);
    fclose(f);
    rename(tmp, token_path());
}
static void token_load(Plugin *s) {
    FILE *f = fopen(token_path(), "r");
    if (!f) return;
    char l[4][1100] = {{0}};
    for (int i = 0; i < 4 && fgets(l[i], sizeof l[i], f); i++) l[i][strcspn(l[i], "\r\n")] = 0;
    fclose(f);
    snprintf(s->access, sizeof s->access, "%s", l[0]);
    snprintf(s->refresh, sizeof s->refresh, "%s", l[1]);
    s->expires = atol(l[2]);
    snprintf(s->user, sizeof s->user, "%s", l[3]);
}
/* a token answer into the sign-in; 1 = taken */
static int take_tokens(Plugin *s, const Buf *b) {
    JDoc d;
    const int r = jp_parse(&d, b->b ? b->b : "", (int)b->n);
    int ok = 0;
    if (r >= 0 && jp_get(&d, r, "access_token") >= 0) {
        jp_str(&d, jp_get(&d, r, "access_token"), s->access, sizeof s->access);
        char rt[1024];
        jp_str(&d, jp_get(&d, r, "refresh_token"), rt, sizeof rt);
        if (rt[0]) snprintf(s->refresh, sizeof s->refresh, "%s", rt);
        const long in = jp_num(&d, jp_get(&d, r, "expires_in"));
        s->expires = (long)time(NULL) + (in > 60 ? in - 60 : 3000);
        ok = s->access[0] != 0;
    }
    jp_free(&d);
    return ok;
}
static int refresh_tokens(Plugin *s) {
    if (!s->refresh[0]) return 0;
    char form[1400], e[1100];
    url_escape(s->refresh, e, sizeof e);
    snprintf(form, sizeof form, "grant_type=refresh_token&refresh_token=%s&client_id=%s", e, T3K_CLIENT_ID);
    Buf b = {0};
    const long c = http(T3K_API "/oauth/token", form, NULL, &b);
    const int ok = c == 200 && take_tokens(s, &b);
    free(b.b);
    if (ok) token_save(s);
    return ok;
}
/* an API GET with the sign-in (refreshed when it has run out or is turned down); the answer in *b */
static long api_get(Plugin *s, const char *path, Buf *b) {
    char url[700];
    snprintf(url, sizeof url, "%s%s", T3K_API, path);
    if (s->access[0] && s->expires && time(NULL) > s->expires) refresh_tokens(s);
    long c = http(url, NULL, s->access, b);
    if (c == 401 && refresh_tokens(s)) { free(b->b); memset(b, 0, sizeof *b); c = http(url, NULL, s->access, b); }
    return c;
}
static void fetch_user(Plugin *s) {
    Buf b = {0};
    if (api_get(s, "/user", &b) == 200) {
        JDoc d;
        const int r = jp_parse(&d, b.b, (int)b.n);
        char u[48] = "";
        jp_str(&d, jp_get(&d, r, "display_name"), u, sizeof u);
        if (!u[0]) jp_str(&d, jp_get(&d, r, "username"), u, sizeof u);
        jp_free(&d);
        pthread_mutex_lock(&s->mu);
        snprintf(s->user, sizeof s->user, "%s", u);
        pthread_mutex_unlock(&s->mu);
        token_save(s);
    }
    free(b.b);
}
static int job_waiting(Plugin *s) { pthread_mutex_lock(&s->mu); const int j = s->job; pthread_mutex_unlock(&s->mu); return j; }
static void do_signin(Plugin *s) {
    say(s, "Asking TONE3000 for a code...", "");
    Buf b = {0};
    const long c = http(T3K_API "/oauth/device_authorization", "client_id=" T3K_CLIENT_ID "&scope=read", NULL, &b);
    JDoc d;
    const int r = c == 200 ? jp_parse(&d, b.b, (int)b.n) : -1;
    if (r < 0) { if (c == 200) jp_free(&d); free(b.b); say(s, c ? "TONE3000 said no (sign-in)" : "No internet connection", ""); return; }
    char dev[256], code[32], form[512];
    jp_str(&d, jp_get(&d, r, "device_code"), dev, sizeof dev);
    jp_str(&d, jp_get(&d, r, "user_code"), code, sizeof code);
    long interval = jp_num(&d, jp_get(&d, r, "interval")), life = jp_num(&d, jp_get(&d, r, "expires_in"));
    jp_free(&d);
    free(b.b);
    if (interval < 2) interval = 5;
    if (life < 60) life = 600;
    char l2[64];
    snprintf(l2, sizeof l2, "and enter the code %s", code);
    say(s, "Go to tone3000.com/activate", l2);
    snprintf(form, sizeof form, "grant_type=urn:ietf:params:oauth:grant-type:device_code&device_code=%s&client_id=%s", dev, T3K_CLIENT_ID);
    const time_t end = time(NULL) + life;
    while (time(NULL) < end) {
        for (long w = 0; w < interval * 10; w++) { if (s->quit || job_waiting(s)) { say(s, "Sign-in stopped", ""); return; } usleep(100000); }
        Buf t = {0};
        const long tc = http(T3K_API "/oauth/token", form, NULL, &t);
        if (tc == 200 && take_tokens(s, &t)) {
            free(t.b);
            token_save(s);
            fetch_user(s);
            char who[80];
            snprintf(who, sizeof who, "Signed in%s%s", s->user[0] ? " as " : "", s->user);
            say(s, who, "Pick a Source and press Find");
            return;
        }
        if (t.b && strstr(t.b, "slow_down")) interval += 5;
        const int fatal = t.b && (strstr(t.b, "access_denied") || strstr(t.b, "expired_token") || strstr(t.b, "invalid_"));
        free(t.b);
        if (fatal || (tc != 400 && tc != 428 && tc != 200)) { say(s, "Sign-in didn't go through", "Press Sign In to try again"); return; }
    }
    say(s, "The code ran out", "Press Sign In for a new one");
}

/* ---- finding tones ------------------------------------------------------------------------------------------- */
static void tone_from(const JDoc *d, int o, Tone *t) {
    memset(t, 0, sizeof *t);
    t->id = jp_num(d, jp_get(d, o, "id"));
    jp_str(d, jp_get(d, o, "title"), t->title, sizeof t->title);
    const int u = jp_get(d, o, "user");
    jp_str(d, jp_get(d, u, "display_name"), t->creator, sizeof t->creator);
    if (!t->creator[0]) jp_str(d, jp_get(d, u, "username"), t->creator, sizeof t->creator);
    jp_str(d, jp_get(d, o, "gear"), t->gear, sizeof t->gear);
    jp_str(d, jp_get(d, o, "license"), t->license, sizeof t->license);
    jp_str(d, jp_get(d, o, "format"), t->format, sizeof t->format);
    const int sz = jp_get(d, o, "sizes");
    for (int k = 0, n = jp_len(d, sz); k < n; k++) {
        char x[16];
        jp_str(d, jp_at(d, sz, k), x, sizeof x);
        if (strlen(t->sizes) + strlen(x) + 2 < sizeof t->sizes) { if (t->sizes[0]) strcat(t->sizes, ", "); strcat(t->sizes, x); }
    }
    t->downloads = jp_num(d, jp_get(d, o, "downloads_count"));
    t->a2 = jp_num(d, jp_get(d, o, "a2_models_count"));
    t->irs = jp_num(d, jp_get(d, o, "irs_count"));
}
static void do_find(Plugin *s) {
    if (!s->access[0]) { say(s, "Sign in first", "Press Sign In"); return; }
    const int src = option_of(s->norm[kSource], 7), sort = option_of(s->norm[kSort], 4);
    const int brand = option_of(s->norm[kBrand], (int)(sizeof BRANDS / sizeof *BRANDS));
    const int style = option_of(s->norm[kStyle], (int)(sizeof STYLES / sizeof *STYLES));
    char q[96] = "", qe[200];
    if (brand) snprintf(q, sizeof q, "%s", BRANDS[brand]);
    if (style) snprintf(q + strlen(q), sizeof q - strlen(q), "%s%s", q[0] ? " " : "", STYLES[style]);
    url_escape(q, qe, sizeof qe);
    static const char *const SORT_KEY[] = {"trending", "downloads-all-time", "newest", "best-match"};
    say(s, "Looking on TONE3000...", "");
    Tone *found = calloc(MAXT, sizeof *found);
    int n = 0;
    long code = 0;
    for (int page = 1; page <= 2 && n < MAXT && found; page++) {
        char path[600];
        if (src == 5 || src == 6)   /* the user's favorites / downloads */
            snprintf(path, sizeof path, "/tones/%s?page=%d&page_size=25%s%s", src == 5 ? "favorited" : "downloaded", page,
                     q[0] ? "&query=" : "", qe);
        else {
            static const char *const GEARS[] = {"", "amp", "amp-cab", "pedal", ""};
            snprintf(path, sizeof path, "/tones/search?page=%d&page_size=25&sort=%s&format=%s%s%s%s%s%s", page,
                     SORT_KEY[sort == 3 && !q[0] ? 0 : sort], src == 4 ? "ir" : "nam", src == 4 ? "" : "&architecture=2",
                     GEARS[src][0] ? "&gears=" : "", GEARS[src], q[0] ? "&query=" : "", qe);
        }
        Buf b = {0};
        code = api_get(s, path, &b);
        if (code != 200) { free(b.b); break; }
        JDoc d;
        const int r = jp_parse(&d, b.b, (int)b.n), data = jp_get(&d, r, "data");
        const int k = jp_len(&d, data);
        for (int i = 0; i < k && n < MAXT; i++) {
            tone_from(&d, jp_at(&d, data, i), &found[n]);
            if (found[n].id && (src == 4 || src >= 5 || found[n].a2 > 0 || found[n].irs > 0)) n++;
        }
        const long pages = jp_num(&d, jp_get(&d, r, "total_pages"));
        jp_free(&d);
        free(b.b);
        if (k < 25 || page >= pages) break;
    }
    if (!found) return;
    pthread_mutex_lock(&s->mu);
    memcpy(s->tones, found, sizeof *found * (size_t)n);
    s->ntones = n;
    s->tone = n ? 0 : -1;
    s->nmodels = 0;
    s->model = 0;
    pthread_mutex_unlock(&s->mu);
    free(found);
    s->automate[kTone] = s->automate[kModel] = s->automate[kInfo1] = s->automate[kInfo2] = s->automate[kInfo3] = 1;
    if (code == 401) say(s, "Signed out: press Sign In", "");
    else if (code == 429) say(s, "TONE3000 is busy (too many asks)", "Wait a minute, then Find again");
    else if (code != 200 && !n) say(s, code ? "TONE3000 didn't answer the search" : "No internet connection", "");
    else {
        char a[64];
        snprintf(a, sizeof a, "%d tones found", n);
        say(s, a, n ? "Pick one: Tone < > or tap its name" : "Try another Brand or Style");
    }
    if (n) { pthread_mutex_lock(&s->mu); s->job = s->job ? s->job : J_MODELS; pthread_mutex_unlock(&s->mu); }
}
static void do_models(Plugin *s) {
    pthread_mutex_lock(&s->mu);
    const Tone t = s->tone >= 0 && s->tone < s->ntones ? s->tones[s->tone] : (Tone){0};
    pthread_mutex_unlock(&s->mu);
    if (!t.id) return;
    const int ir = !strcmp(t.format, "ir");
    char path[160];
    snprintf(path, sizeof path, "/models?tone_id=%ld&page_size=%d%s", t.id, MAXM, ir ? "" : "&architecture=2");
    Buf b = {0};
    const long code = api_get(s, path, &b);
    Model *m = calloc(MAXM, sizeof *m);
    int n = 0;
    if (code == 200 && m) {
        JDoc d;
        const int r = jp_parse(&d, b.b, (int)b.n), data = jp_get(&d, r, "data");
        for (int i = 0, k = jp_len(&d, data); i < k && n < MAXM; i++) {
            const int o = jp_at(&d, data, i);
            Model *x = &m[n];
            x->id = jp_num(&d, jp_get(&d, o, "id"));
            jp_str(&d, jp_get(&d, o, "name"), x->name, sizeof x->name);
            jp_str(&d, jp_get(&d, o, "model_url"), x->url, sizeof x->url);
            jp_str(&d, jp_get(&d, o, "architecture_version"), x->arch, sizeof x->arch);
            if (x->url[0] && (ir || !x->arch[0] || !strcmp(x->arch, "2"))) n++;   /* A2 only (the MPC's CPU) */
        }
        jp_free(&d);
    }
    free(b.b);
    pthread_mutex_lock(&s->mu);
    if (s->tone >= 0 && s->tone < s->ntones && s->tones[s->tone].id == t.id && m) {
        memcpy(s->models, m, sizeof *m * (size_t)n);
        s->nmodels = n;
        s->model = 0;
    }
    pthread_mutex_unlock(&s->mu);
    free(m);
    s->automate[kModel] = 1;
    char a[64];
    if (code != 200) snprintf(a, sizeof a, "Couldn't list this tone's models");
    else if (!n) snprintf(a, sizeof a, "No A2 models in this tone");
    else snprintf(a, sizeof a, "%d %s: pick one or All, then Download", n, ir ? "IRs" : "A2 models");
    say(s, a, "");
}

/* ---- downloading --------------------------------------------------------------------------------------------- */
static void clean_name(const char *in, char *out, size_t max) {   /* a file / folder name from a title */
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < max; p++) {
        const char c = *p;
        out[o++] = (strchr("/\\:*?\"<>|", c) || (unsigned char)c < 32) ? '-' : c;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;   /* (exFAT dislikes trailing dots and spaces) */
    out[o] = 0;
    if (!out[0] || out[0] == '.') snprintf(out, max, "TONE3000");
}
static int dest_base(char *out, size_t max) {   /* the first drive with a NAM folder, else /sdcard/NAM */
    FILE *m = fopen("/proc/mounts", "r");
    char dev[256], mp[256];
    out[0] = 0;
    if (getenv("T3K_DEST")) { snprintf(out, max, "%s", getenv("T3K_DEST")); if (m) fclose(m); return 1; }   /* (tests) */
    while (m && fscanf(m, "%255s %255s %*s %*s %*d %*d", dev, mp) == 2) {
        if (strncmp(mp, "/media/", 7)) continue;
        char unesc[256];   /* (mounts writes a space as \040) */
        size_t o = 0;
        for (const char *p = mp; *p && o + 1 < sizeof unesc; p++) {
            if (p[0] == '\\' && p[1] == '0' && p[2] == '4' && p[3] == '0') { unesc[o++] = ' '; p += 3; } else unesc[o++] = *p;
        }
        unesc[o] = 0;
        char nam[300];
        struct stat st;
        snprintf(nam, sizeof nam, "%s/NAM", unesc);
        if (!stat(nam, &st) && S_ISDIR(st.st_mode)) { snprintf(out, max, "%s", nam); break; }
    }
    if (m) fclose(m);
    if (!out[0]) { snprintf(out, max, "/sdcard/NAM"); mkdir(out, 0755); }
    return 1;
}
static int download_one(Plugin *s, const Model *x, const char *dir, int ir) {
    char name[100], path[600], tmp[620];
    clean_name(x->name, name, sizeof name);
    const char *ext = ir ? ".wav" : ".nam";
    const size_t nl = strlen(name);
    snprintf(path, sizeof path, "%s/%s%s", dir, name, nl > 4 && !strcasecmp(name + nl - 4, ext) ? "" : ext);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    Buf b = {0};
    b.f = f;
    if (s->expires && time(NULL) > s->expires) refresh_tokens(s);
    long c = http(x->url, NULL, s->access, &b);
    if (c == 401 && refresh_tokens(s)) { rewind(f); if (ftruncate(fileno(f), 0)) {} c = http(x->url, NULL, s->access, &b); }
    const long size = ftell(f);
    const int ok = !fclose(f) && c == 200 && size > 64;
    if (ok) rename(tmp, path); else unlink(tmp);
    return ok;
}
static void do_download(Plugin *s) {
    pthread_mutex_lock(&s->mu);
    const Tone t = s->tone >= 0 && s->tone < s->ntones ? s->tones[s->tone] : (Tone){0};
    const int pick = s->model, n = s->nmodels;
    Model *m = n ? malloc(sizeof *m * (size_t)n) : NULL;
    if (m) memcpy(m, s->models, sizeof *m * (size_t)n);
    pthread_mutex_unlock(&s->mu);
    if (!t.id || !m) { free(m); say(s, "Pick a tone with models first", ""); return; }
    const int ir = !strcmp(t.format, "ir");
    char base[300], folder[100], dir[700];
    dest_base(base, sizeof base);
    clean_name(t.title, folder, sizeof folder);
    snprintf(dir, sizeof dir, "%s/%s", base, ir ? "IRs" : "Models");
    mkdir(dir, 0755);
    snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "/%s", folder);
    mkdir(dir, 0755);
    int got = 0, want = 0;
    for (int i = 0; i < n; i++) {
        if (pick > 0 && i != pick - 1) continue;
        want++;
        char a[64];
        snprintf(a, sizeof a, "Downloading %d of %d...", want, pick > 0 ? 1 : n);
        say(s, a, m[i].name);
        got += download_one(s, &m[i], dir, ir);
        if (s->quit) break;
    }
    free(m);
    FILE *st = fopen(T3K_STAMP, "w");   /* (NAM MPC rescans when it sees this change) */
    if (st) { fprintf(st, "%ld\n", (long)time(NULL)); fclose(st); }
    utime(T3K_STAMP, NULL);
    char a[64], b2[96];
    snprintf(a, sizeof a, got == want ? "Downloaded %d to the card" : "Downloaded %d of %d", got, want);
    snprintf(b2, sizeof b2, "%s/%.40s", ir ? "NAM/IRs" : "NAM/Models", folder);
    say(s, a, b2);
}

static void *worker(void *arg) {
    Plugin *s = arg;
    token_load(s);
    if (s->access[0]) {
        char who[80];
        snprintf(who, sizeof who, "Signed in%s%s", s->user[0] ? " as " : "", s->user);
        say(s, who, "Pick a Source and press Find");
    } else say(s, "Press Sign In to connect TONE3000", "");
    for (;;) {
        pthread_mutex_lock(&s->mu);
        while (!s->job && !s->quit) pthread_cond_wait(&s->cv, &s->mu);
        const int j = s->job;
        s->job = J_NONE;
        s->busy = j;
        pthread_mutex_unlock(&s->mu);
        if (s->quit) break;
        if (j == J_SIGNIN) do_signin(s);
        else if (j == J_FIND) do_find(s);
        else if (j == J_MODELS) do_models(s);
        else if (j == J_DOWNLOAD) do_download(s);
        s->busy = 0;
    }
    return NULL;
}
static void post(Plugin *s, int j) {
    pthread_mutex_lock(&s->mu);
    s->job = j;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

/* ---- the plugin --------------------------------------------------------------------------------------------- */
static void process(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *s = (Plugin *)e->object;
    if (s->master) {
        for (int i = 0; i < kNumParams; i++)
            if (s->automate[i]) { s->automate[i] = 0; s->master(e, audioMasterAutomate, i, 0, NULL, s->norm[i]); s->update_display = 1; }
        if (s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    }
    for (int c = 0; c < 2; c++) {   /* the audio passes straight through (in place, or not) */
        const float *x = in && in[c] ? in[c] : NULL;
        for (int k = 0; k < n; k++) { float v = x ? x[k] : 0; if (!finite_bits(v) || v > 8 || v < -8) v = 0; out[c][k] = v; }
    }
}
static void pick_tone(Plugin *s, int t) {
    pthread_mutex_lock(&s->mu);
    const int ok = t >= 0 && t < s->ntones;
    if (ok) { s->tone = t; s->nmodels = 0; s->model = 0; }
    const int n = s->ntones;
    pthread_mutex_unlock(&s->mu);
    if (!ok) return;
    s->norm[kTone] = n > 1 ? (float)t / (float)(n - 1) : 0;
    s->automate[kTone] = s->automate[kModel] = s->automate[kInfo1] = s->automate[kInfo2] = s->automate[kInfo3] = 1;
    post(s, J_MODELS);
}
static void pick_model(Plugin *s, int m) {
    pthread_mutex_lock(&s->mu);
    const int n = s->nmodels + 1;
    if (m >= 0 && m < n) s->model = m;
    pthread_mutex_unlock(&s->mu);
    s->norm[kModel] = n > 1 ? (float)m / (float)(n - 1) : 0;
    s->automate[kModel] = 1;
}
static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams) return;
    v = clampf(v, 0, 1);
    const int press = v > 0.5f;
    switch (i) {
    case kSignIn: if (press) post(s, J_SIGNIN); s->norm[i] = 0; return;
    case kSignOut:
        if (press) {
            pthread_mutex_lock(&s->mu);
            s->access[0] = s->refresh[0] = s->user[0] = 0;
            s->expires = 0;
            pthread_mutex_unlock(&s->mu);
            unlink(token_path());
            say(s, "Signed out", "Press Sign In to connect TONE3000");
        }
        s->norm[i] = 0;
        return;
    case kFind: if (press) post(s, J_FIND); s->norm[i] = 0; return;
    case kDownload: if (press) post(s, J_DOWNLOAD); s->norm[i] = 0; return;
    case kTonePrev: case kToneNext:
        if (press && s->ntones) pick_tone(s, (s->tone + (i == kTonePrev ? -1 : 1) + s->ntones) % s->ntones);
        s->norm[i] = 0;
        return;
    case kModelPrev: case kModelNext: {
        const int n = s->nmodels + 1;
        if (press && s->nmodels) pick_model(s, (s->model + (i == kModelPrev ? -1 : 1) + n) % n);
        s->norm[i] = 0;
        return;
    }
    case kTone: case kModel: {   /* a Q-Link nudge steps; an exact value (a project restoring) changes nothing */
        const int n = i == kTone ? s->ntones : s->nmodels + 1, cur = i == kTone ? s->tone : s->model;
        if (n < 2) return;
        const float pos = v * (float)(n - 1);
        if (fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - (float)cur) < 1) {
            const int to = (cur + (pos > (float)cur ? 1 : -1) + n) % n;
            if (i == kTone) pick_tone(s, to); else pick_model(s, to);
        }
        return;
    }
    case kStatus: case kStatus2: case kInfo1: case kInfo2: case kInfo3: case kPowered: return;   /* (read-only) */
    default: s->norm[i] = snap(&P[i], v, s->norm[i]); s->update_display = 1;
    }
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }
static void fmt_count(long n, char *t, size_t max) {
    if (n >= 1000000) snprintf(t, max, "%.1fM", (double)n / 1e6);
    else if (n >= 1000) snprintf(t, max, "%.1fk", (double)n / 1e3);
    else snprintf(t, max, "%ld", n);
}
static void display(Plugin *s, int i, char *t, int max) {
    t[0] = 0;
    pthread_mutex_lock(&s->mu);
    const Tone *tn = s->tone >= 0 && s->tone < s->ntones ? &s->tones[s->tone] : NULL;
    switch (i) {
    case kStatus: snprintf(t, (size_t)max, "%s", s->status); break;
    case kStatus2: snprintf(t, (size_t)max, "%s", s->status2[0] ? s->status2 : " "); break;
    case kTone: snprintf(t, (size_t)max, "%s", tn ? tn->title : "-"); break;
    case kModel:
        if (!tn || !s->nmodels) snprintf(t, (size_t)max, "-");
        else if (s->model == 0) snprintf(t, (size_t)max, "All %d models", s->nmodels);
        else snprintf(t, (size_t)max, "%s", s->models[s->model - 1].name);
        break;
    case kInfo1: snprintf(t, (size_t)max, "%s%s", tn ? "by " : " ", tn ? tn->creator : ""); break;
    case kInfo2: {
        char dl[16] = "";
        if (tn) fmt_count(tn->downloads, dl, sizeof dl);
        if (tn) snprintf(t, (size_t)max, "%s  %s  %s dl", tn->gear[0] ? tn->gear : tn->format, tn->sizes, dl);
        else snprintf(t, (size_t)max, " ");
        break;
    }
    case kInfo3: snprintf(t, (size_t)max, "%s%s", tn ? "License: " : " ", tn ? tn->license : ""); break;
    case kPowered: snprintf(t, (size_t)max, "Powered by TONE3000"); break;
    case kTonePrev: case kModelPrev: snprintf(t, (size_t)max, "<"); break;
    case kToneNext: case kModelNext: snprintf(t, (size_t)max, ">"); break;
    case kSignIn: snprintf(t, (size_t)max, "Sign In"); break;
    case kSignOut: snprintf(t, (size_t)max, "Sign Out"); break;
    case kFind: snprintf(t, (size_t)max, "Find"); break;
    case kDownload: snprintf(t, (size_t)max, "Download"); break;
    default: pdisplay(&P[i], s->norm[i], t);
    }
    pthread_mutex_unlock(&s->mu);
}
static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    (void)v; (void)opt;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        pthread_mutex_lock(&s->mu);
        s->quit = 1;
        pthread_cond_signal(&s->cv);
        pthread_mutex_unlock(&s->mu);
        pthread_join(s->th, NULL);   /* (a download in flight finishes its file first; a minute at worst) */
        free(s);
        return 1;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) copy_str(p, P[idx].name, 32); return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay: if (idx >= 0 && idx < kNumParams) { char t[64]; display(s, idx, t, 40); copy_str(p, t, 40); } return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams && idx != kStatus && idx != kStatus2 && idx < kInfo1;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "TONE3000", 32); return 1;
    case effGetVendorString: copy_str(p, VC_VENDOR, 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    default: return 0;
    }
}

/* the full-page lists: the tones found, and the chosen tone's models */
static int t3k_pick(AEffect *e, int list, int op, int i, char *out, int max) {
    Plugin *s = (Plugin *)e->object;
    if (op == PL_CHOOSE) { if (list == 0) pick_tone(s, i); else pick_model(s, i); return 0; }
    pthread_mutex_lock(&s->mu);
    int r = 0;
    if (list == 0) {
        if (op == PL_COUNT) r = s->ntones;
        else if (op == PL_CURRENT) r = s->tone;
        else if (op == PL_NAME && i >= 0 && i < s->ntones) snprintf(out, (size_t)max, "%s", s->tones[i].title);
    } else {
        if (op == PL_COUNT) r = s->nmodels ? s->nmodels + 1 : 0;
        else if (op == PL_CURRENT) r = s->model;
        else if (op == PL_NAME && i == 0) snprintf(out, (size_t)max, "All %d models", s->nmodels);
        else if (op == PL_NAME && i > 0 && i <= s->nmodels) snprintf(out, (size_t)max, "%s", s->models[i - 1].name);
    }
    pthread_mutex_unlock(&s->mu);
    return r;
}
static const char *const LIST_KEYS[] = {"tone", "model"};
static const char *const LIST_TITLES[] = {"Tones", "Models"};
static const PickLists LISTS = {2, LIST_KEYS, LIST_TITLES, t3k_pick, 0};

static AEffect *make(audioMasterCallback master) {
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    s->tone = -1;
    for (int i = 0; i < kNumParams; i++) s->norm[i] = pn(&P[i], P[i].def);
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    if (pthread_create(&s->th, NULL, worker, s)) { free(s); return NULL; }
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
    e->flags = effFlagsCanReplacing;
    e->object = s;
    e->uniqueID = 0x54334b42;   /* 'T3KB' */
    e->version = 1000;
    return e;
}
__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) { return pl_wrap(master, make, &LISTS); }

#ifdef VC_PARAMS
int main(void) { write_params_json("TONE3000", P, kNumParams, &LISTS); return 0; }
#endif
