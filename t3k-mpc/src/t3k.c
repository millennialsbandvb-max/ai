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
#define MAXT 250    /* tones kept (pages of 25 load as the cards scroll) */
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
       kModelNext, kDownload, kStatus, kStatus2, kInfo1, kInfo2, kInfo3, kPowered, kBrandPrev, kBrandNext, kStylePrev,
       kStyleNext,
       /* the list-first page (Nov 2026, the user's ask): tone rows to tap, pages, big tap tiles (a "list" of one) */
       kPagePrev, kPageNext, kPage, kSel, kDl, kSignTile, kSignOutTile, kHelp1, kHelp2, kRow0,
       /* the HeadRush-style screen (Nov 2026, the user's ask, after its TONE3000 library): one page, four views
        * (Library = 6 cards with gear pictures, Tone, Search, Account), a picture kind per card, nav tiles */
       kView = kRow0 + 12, kHdr, kCardKind0, kSelKind = kCardKind0 + 6, kBack, kNav1, kNav2, kScrUp, kScrDn,
       /* the HeadRush editor's TONE3000 browser, copied from the user's screenshots (Nov 2026): tabs, search, sort,
        * filter drawer, 10 cards (title, uploader, downloads / favorites / models), the tone pop-up with its model rows */
       kTab0, kQuery = kTab0 + 3, kQueryOpen, kSortBy, kSortByOpen, kGear0, kFilt = kGear0 + 5, kFReset, kFDone, kQClear,
       kCount, kUp0, kSd0 = kUp0 + 10, kSf0 = kSd0 + 10, kSm0 = kSf0 + 10, kCk7 = kSm0 + 10, kMClose = kCk7 + 4, kGearTxt,
       kDate, kDesc0, kMakes = kDesc0 + 3, kTags0, kModHdr = kTags0 + 2, kMRow0, kMDl0 = kMRow0 + 6, kMAll = kMDl0 + 6,
       kMScrUp, kMScrDn, kMPage, kSide0, kBot0 = kSide0 + 2, kBlank = kBot0 + 4,
       kNumParams };
#define ROWS 10   /* cards a page: row_1..row_10 (row_11, row_12 are left from the list page, blank) */
#define MROWS 6   /* model rows a page in the tone pop-up */
static const char *const VIEWS[] = {"Library", "Tone", "Search", "Account"};
enum { V_LIB, V_TONE, V_SEARCH, V_ACCOUNT };
/* a card's picture (option 0: none, so a hidden view draws nothing) */
static const char *const KINDS[] = {"None", "Amp", "Full Rig", "Pedal", "IR", "Outboard", "Empty"};
static const char *const TABS[] = {"Favorites", "Browse", "My Tones"};
static const char *const QUERIES[] = {"Search", "plexi", "marshall", "fender", "vox", "mesa", "5150", "peavey", "orange",
                                      "friedman", "bogner", "soldano", "diezel", "dumble", "matchless", "two rock", "hiwatt",
                                      "supro", "ampeg", "darkglass", "tube screamer", "klon", "big muff", "boss", "rat",
                                      "clean", "edge of breakup", "crunch", "high gain", "metal", "blues", "bass", "fuzz",
                                      "boost", "jcm800", "ac30", "twin reverb", "deluxe", "jazz"};
static const char *const SORTBYS[] = {"Trending", "Newest", "Oldest", "Most Downloads"};
static const char *const SORTBY_KEY[] = {"trending", "newest", "oldest", "downloads-all-time"};
static const char *const OPENS[] = {"Closed", "Open"};
static const char *const GEAR_NAMES[] = {"Amp Head", "Full Rig / Combo", "Pedal", "Outboard", "IR"};
static const char *const GEAR_KEYS[] = {"amp", "amp-cab", "pedal", "outboard", ""};
#define NQUERIES ((int)(sizeof QUERIES / sizeof *QUERIES))
static const ParamDef P[kNumParams] = {
    OPT("source", "Source", SOURCES, 0), OPT("sort", "Sort", SORTS, 0), OPT("brand", "Brand", BRANDS, 0),
    OPT("style", "Style", STYLES, 0), BUTTON("signin", "Sign In"), BUTTON("signout", "Sign Out"), BUTTON("find", "Find"), PRESET("tone", "Tone"), BUTTON("tone_prev", "Tone <"), BUTTON("tone_next", "Tone >"),
    PRESET("model", "Model"), BUTTON("model_prev", "Model <"), BUTTON("model_next", "Model >"),
    BUTTON("download", "Download"), PRESET("status", "Status"), PRESET("status2", "Status 2"),
    PRESET("info1", "Creator"), PRESET("info2", "Gear"), PRESET("info3", "License"), PRESET("powered", "Powered"),
    BUTTON("brand_prev", "Brand <"), BUTTON("brand_next", "Brand >"), BUTTON("style_prev", "Style <"), BUTTON("style_next", "Style >"),
    BUTTON("page_prev", "Page <"), BUTTON("page_next", "Page >"), PRESET("page", "Page"), PRESET("sel", "Selected"),
    PRESET("dl_1", "Download Tile"), PRESET("acct_1", "Sign In Tile"), PRESET("acct_2", "Sign Out Tile"),
    PRESET("help1", "Help 1"), PRESET("help2", "Help 2"),
    PRESET("row_1", "Row 1"), PRESET("row_2", "Row 2"), PRESET("row_3", "Row 3"), PRESET("row_4", "Row 4"),
    PRESET("row_5", "Row 5"), PRESET("row_6", "Row 6"), PRESET("row_7", "Row 7"), PRESET("row_8", "Row 8"),
    PRESET("row_9", "Row 9"), PRESET("row_10", "Row 10"), PRESET("row_11", "Row 11"), PRESET("row_12", "Row 12"),
    OPT("view", "View", VIEWS, 0), PRESET("hdr", "Header"),
    OPT("ck_1", "Card 1 Kind", KINDS, 0), OPT("ck_2", "Card 2 Kind", KINDS, 0), OPT("ck_3", "Card 3 Kind", KINDS, 0),
    OPT("ck_4", "Card 4 Kind", KINDS, 0), OPT("ck_5", "Card 5 Kind", KINDS, 0), OPT("ck_6", "Card 6 Kind", KINDS, 0),
    OPT("selk", "Tone Kind", KINDS, 0), PRESET("back_1", "Back Tile"), PRESET("nav_1", "Search Tile"),
    PRESET("nav_2", "Account Tile"), PRESET("scr_1", "Up Tile"), PRESET("scr_2", "Down Tile"),
    PRESET("tb_1", "Tab Favorites"), PRESET("tb_2", "Tab Browse"), PRESET("tb_3", "Tab My Tones"),
    OPT("query", "Search", QUERIES, 0), OPT("query__open", "Search List", OPENS, 0),
    OPT("sortby", "Sort By", SORTBYS, 0), OPT("sortby__open", "Sort List", OPENS, 0),
    PRESET("gear_1", "Gear Amp Head"), PRESET("gear_2", "Gear Full Rig"), PRESET("gear_3", "Gear Pedal"),
    PRESET("gear_4", "Gear Outboard"), PRESET("gear_5", "Gear IR"), PRESET("filt_1", "Filter Icon"),
    PRESET("fbtn_1", "Filter Reset"), PRESET("fbtn_2", "Filter Done"), PRESET("qx_1", "Search Clear"),
    PRESET("count", "Results"),
#define CARD10(k, n) PRESET(k "_1", n " 1"), PRESET(k "_2", n " 2"), PRESET(k "_3", n " 3"), PRESET(k "_4", n " 4"), \
    PRESET(k "_5", n " 5"), PRESET(k "_6", n " 6"), PRESET(k "_7", n " 7"), PRESET(k "_8", n " 8"), PRESET(k "_9", n " 9"), \
    PRESET(k "_10", n " 10")
    CARD10("up", "Uploader"), CARD10("sd", "Downloads"), CARD10("sf", "Favorites"), CARD10("sm", "Models"),
    OPT("ck_7", "Card 7 Kind", KINDS, 0), OPT("ck_8", "Card 8 Kind", KINDS, 0), OPT("ck_9", "Card 9 Kind", KINDS, 0),
    OPT("ck_10", "Card 10 Kind", KINDS, 0),
    PRESET("mx_1", "Close Tile"), PRESET("gtxt", "Gear Type"), PRESET("date", "Date"),
    PRESET("desc1", "Description 1"), PRESET("desc2", "Description 2"), PRESET("desc3", "Description 3"),
    PRESET("makes", "Makes"), PRESET("tags1", "Tags 1"), PRESET("tags2", "Tags 2"), PRESET("mhdr", "Models Header"),
    PRESET("mr_1", "Model Row 1"), PRESET("mr_2", "Model Row 2"), PRESET("mr_3", "Model Row 3"),
    PRESET("mr_4", "Model Row 4"), PRESET("mr_5", "Model Row 5"), PRESET("mr_6", "Model Row 6"),
    PRESET("md_1", "Model Download 1"), PRESET("md_2", "Model Download 2"), PRESET("md_3", "Model Download 3"),
    PRESET("md_4", "Model Download 4"), PRESET("md_5", "Model Download 5"), PRESET("md_6", "Model Download 6"),
    PRESET("mall_1", "Download All Tile"), PRESET("mscr_1", "Models Up"), PRESET("mscr_2", "Models Down"),
    PRESET("mpg", "Models Page"), PRESET("side_1", "Cloud Tile"), PRESET("side_2", "Account Side Tile"),
    PRESET("bot_1", "Featured Tile"), PRESET("bot_2", "Browse Tile"), PRESET("bot_3", "My Content Tile"),
    PRESET("bot_4", "TONE3000 Tile"), PRESET("blank", "Blank"),
};
#define NBRANDS ((int)(sizeof BRANDS / sizeof *BRANDS))
#define NSTYLES ((int)(sizeof STYLES / sizeof *STYLES))

typedef struct {
    long id; char title[96], creator[48], gear[24], sizes[48], license[16], format[8], date[24], desc[400], makes[96], tags[200];
    long downloads, a2, irs, favs, models;
} Tone;
typedef struct { long id; char name[96], url[300], arch[8]; } Model;
enum { J_NONE, J_SIGNIN, J_FIND, J_MODELS, J_DOWNLOAD, J_MORE };

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    volatile int automate[kNumParams], update_display;
    volatile int repaint[kNumParams];   /* tiles whose text changed: reported as a tiny change, then their value */
    int page;                           /* the tone rows' page (ROWS a page) */
    int view;                           /* V_LIB.. */
    int tab, query, sortby, gmask;      /* the search: Favorites / Browse / My Tones, a search word, sort, gear boxes */
    int apage, apages, want_page, mpage;
    long total;
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
/* the tap tiles (tone rows, Download, Sign In / Out): a selected row rests at 1, everything else at 0; new text is
 * reported as a tiny change and then the value, so MPC repaints it */
static int is_tile(int i) {
    return (i >= kRow0 && i < kRow0 + 12) || i == kDl || i == kSignTile || i == kSignOutTile || (i >= kBack && i <= kScrDn) ||
           (i >= kTab0 && i < kTab0 + 3) || (i >= kGear0 && i <= kQClear) || i == kMClose || (i >= kMDl0 && i <= kMScrDn) ||
           (i >= kSide0 && i < kBot0 + 4);
}
static int ck_param(int c) { return c < 6 ? kCardKind0 + c : kCk7 + (c - 6); }
static int kind_of(const Tone *t) {   /* the picture for a tone (TONE3000's gear) */
    if (!strcmp(t->format, "ir") || !strcmp(t->gear, "ir")) return 4;
    if (!strcmp(t->gear, "full-rig")) return 2;
    if (!strcmp(t->gear, "pedal")) return 3;
    if (!strcmp(t->gear, "outboard")) return 5;
    return 1;
}
static void set_opt(Plugin *s, int i, int o, int n) {
    const float v = n > 1 ? (float)o / (float)(n - 1) : 0;
    if (v != s->norm[i]) { s->norm[i] = v; s->automate[i] = 1; }
}
static void tiles_changed(Plugin *s);
static void go_view(Plugin *s, int v) { s->view = v; tiles_changed(s); }
static void tiles_changed(Plugin *s) {
    pthread_mutex_lock(&s->mu);
    const int sel = s->tone - s->page * ROWS;
    pthread_mutex_unlock(&s->mu);
    for (int r = 0; r < 12; r++) s->norm[kRow0 + r] = r == sel && r < ROWS ? 1.0f : 0.0f;
    /* the pictures: only in their own view (option 0 draws nothing) */
    pthread_mutex_lock(&s->mu);
    for (int c = 0; c < ROWS; c++) {
        const int k = s->page * ROWS + c;
        set_opt(s, ck_param(c), s->view != V_LIB ? 0 : k < s->ntones ? kind_of(&s->tones[k]) : 0, 7);
    }
    for (int t = 0; t < 3; t++) s->norm[kTab0 + t] = t == s->tab ? 1.0f : 0.0f;
    for (int g = 0; g < 5; g++) s->norm[kGear0 + g] = (s->gmask >> g) & 1 ? 1.0f : 0.0f;
    s->norm[kSide0] = 1.0f;   /* (Cloud: lit) */
    s->norm[kBot0 + 3] = 1.0f;   /* (TONE3000: lit) */
    for (int i = kUp0; i < kCk7; i++) s->automate[i] = 1;
    for (int i = kGearTxt; i <= kMRow0 + MROWS - 1; i++) s->automate[i] = 1;
    s->automate[kMPage] = s->automate[kCount] = 1;
    set_opt(s, kSelKind, s->view == V_TONE ? (s->tone >= 0 && s->tone < s->ntones ? kind_of(&s->tones[s->tone]) : 6) : 0, 7);
    pthread_mutex_unlock(&s->mu);
    set_opt(s, kView, s->view, 4);
    s->automate[kHdr] = 1;
    for (int i = 0; i < kNumParams; i++) if (is_tile(i)) s->repaint[i] = 2;
    s->automate[kPage] = s->automate[kSel] = s->automate[kInfo1] = s->automate[kInfo2] = s->automate[kInfo3] = 1;
    s->automate[kTone] = s->automate[kModel] = 1;
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
            say(s, who, "Loading tones...");
            pthread_mutex_lock(&s->mu);
            if (!s->job) s->job = J_FIND;
            pthread_mutex_unlock(&s->mu);
            s->view = V_LIB;   /* (back to the library, which fills) */
            tiles_changed(s);
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
    t->favs = jp_num(d, jp_get(d, o, "favorites_count"));
    t->models = jp_num(d, jp_get(d, o, "models_count"));
    jp_str(d, jp_get(d, o, "description"), t->desc, sizeof t->desc);
    char c[40];
    jp_str(d, jp_get(d, o, "created_at"), c, sizeof c);   /* 2026-11-11T... -> 11 November 2026 */
    static const char *const MON[] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                      "September", "October", "November", "December"};
    const int yy = atoi(c), mm = strlen(c) >= 7 ? atoi(c + 5) : 0, dd = strlen(c) >= 10 ? atoi(c + 8) : 0;
    if (yy > 1900 && mm >= 1 && mm <= 12) snprintf(t->date, sizeof t->date, "%d %s %d", dd, MON[mm - 1], yy);
    for (int pass = 0; pass < 2; pass++) {   /* makes / tags: [{id, name}] (or plain strings) joined */
        const int a = jp_get(d, o, pass ? "tags" : "makes");
        char *out = pass ? t->tags : t->makes;
        const size_t max = pass ? sizeof t->tags : sizeof t->makes;
        for (int k = 0, n = jp_len(d, a); k < n; k++) {
            char x[64];
            const int e = jp_at(d, a, k);
            jp_str(d, d->n[e].type == JT_OBJ ? jp_get(d, e, "name") : e, x, sizeof x);
            if (x[0] && strlen(out) + strlen(x) + 3 < max) { if (out[0]) strcat(out, ", "); strcat(out, x); }
        }
    }
    t->a2 = jp_num(d, jp_get(d, o, "a2_models_count"));
    t->irs = jp_num(d, jp_get(d, o, "irs_count"));
}
/* T3K_FAKE_TONES=<n> (tests): n made-up tones of 3 models each, no network; downloads still need the network */
static void fake_find(Plugin *s) {
    const int n = atoi(getenv("T3K_FAKE_TONES")) > MAXT ? MAXT : atoi(getenv("T3K_FAKE_TONES"));
    pthread_mutex_lock(&s->mu);
    for (int i = 0; i < n; i++) {
        Tone *t = &s->tones[i];
        memset(t, 0, sizeof *t);
        t->id = 1000 + i;
        const int src = option_of(s->norm[kSource], 7);
        if (src) snprintf(t->title, sizeof t->title, "Fake %s %d", SOURCES[src], i + 1);
        else snprintf(t->title, sizeof t->title, "Fake Tone %d", i + 1);
        snprintf(t->creator, sizeof t->creator, "maker%d", i % 4);
        snprintf(t->format, sizeof t->format, "nam");
        snprintf(t->gear, sizeof t->gear, "%s", (const char *[]){"amp", "amp-cab", "pedal", "outboard"}[i % 4]);
        snprintf(t->date, sizeof t->date, "11 November 2026");
        snprintf(t->desc, sizeof t->desc, "This pack contains direct profiles captured from a freshly serviced amp using a controlled, studio-grade reamping workflow, number %d.", i + 1);
        snprintf(t->makes, sizeof t->makes, "Marshall 6100LM, Radial X-Amp");
        snprintf(t->tags, sizeof t->tags, "clean, crunch, edge of breakup, heavy metal, high-gain, jazz, lead, marshall, rock");
        t->a2 = 8; t->models = 8; t->downloads = 1000 * (i + 1); t->favs = 10 * i;
    }
    s->ntones = n; s->tone = n ? 0 : -1; s->nmodels = 0; s->model = 0; s->page = 0; s->total = n; s->apage = s->apages = 1;
    pthread_mutex_unlock(&s->mu);
    tiles_changed(s);
    char a[64];
    snprintf(a, sizeof a, "%d tones found", n);
    say(s, a, "Tap a tone, then DOWNLOAD");
}
static void fake_models(Plugin *s, const Tone *t) {
    pthread_mutex_lock(&s->mu);
    if (s->tone >= 0 && s->tone < s->ntones && s->tones[s->tone].id == t->id) {
        for (int i = 0; i < 8; i++) {
            memset(&s->models[i], 0, sizeof s->models[i]);
            snprintf(s->models[i].name, sizeof s->models[i].name, "%s model %d", t->title, i + 1);
        }
        s->nmodels = 8; s->model = 0; s->mpage = 0;
    }
    pthread_mutex_unlock(&s->mu);
    tiles_changed(s);
    say(s, "8 A2 models: tap DOWNLOAD", "");
}
/* a search (append: the next page of the same one, as the cards scroll) */
static void do_search(Plugin *s, int append) {
    if (!s->access[0]) { say(s, "Not signed in", "Tap the person icon, then SIGN IN"); tiles_changed(s); return; }
    if (getenv("T3K_FAKE_TONES")) { if (!append) fake_find(s); return; }
    pthread_mutex_lock(&s->mu);
    const int tab = s->tab, query = s->query, sortby = s->sortby, gm = s->gmask, page = append ? s->apage + 1 : 1;
    pthread_mutex_unlock(&s->mu);
    char qe[200] = "", gears[80] = "";
    if (query > 0) url_escape(QUERIES[query], qe, sizeof qe);
    for (int g = 0; g < 4; g++)
        if (gm & (1 << g)) snprintf(gears + strlen(gears), sizeof gears - strlen(gears), "%s%s", gears[0] ? "_" : "", GEAR_KEYS[g]);
    const int ir = (gm & 16) != 0;
    if (!append) say(s, "Searching...", "");
    char path[600];
    if (tab != 1)   /* the user's favorites / downloads */
        snprintf(path, sizeof path, "/tones/%s?page=%d&page_size=25%s%s%s%s", tab == 0 ? "favorited" : "downloaded", page,
                 qe[0] ? "&query=" : "", qe, gears[0] && !strchr(gears, '_') ? "&gear=" : "", gears[0] && !strchr(gears, '_') ? gears : "");
    else
        snprintf(path, sizeof path, "/tones/search?page=%d&page_size=25&sort=%s&format=%s%s%s%s%s%s", page, SORTBY_KEY[sortby],
                 ir ? "ir" : "nam", ir ? "" : "&architecture=2", gears[0] && !ir ? "&gears=" : "", ir ? "" : gears,
                 qe[0] ? "&query=" : "", qe);
    Buf b = {0};
    const long code = api_get(s, path, &b);
    Tone *found = calloc(25, sizeof *found);
    int n = 0;
    long total = 0, pages = 0;
    if (code == 200 && found) {
        JDoc d;
        const int r = jp_parse(&d, b.b, (int)b.n), data = jp_get(&d, r, "data");
        for (int i = 0, k = jp_len(&d, data); i < k && n < 25; i++) {
            tone_from(&d, jp_at(&d, data, i), &found[n]);
            if (found[n].id) n++;
        }
        total = jp_num(&d, jp_get(&d, r, "total"));
        pages = jp_num(&d, jp_get(&d, r, "total_pages"));
        if (r >= 0) jp_free(&d);
    }
    free(b.b);
    if (!found) return;
    pthread_mutex_lock(&s->mu);
    if (!append) { s->ntones = 0; s->tone = -1; s->nmodels = 0; s->model = 0; s->page = 0; }
    if (code == 200) {
        const int room = MAXT - s->ntones, k = n < room ? n : room;
        memcpy(s->tones + s->ntones, found, sizeof *found * (size_t)k);
        s->ntones += k;
        s->apage = page;
        s->apages = (int)pages;
        s->total = total > s->ntones ? total : s->ntones;
        if (append && s->want_page * ROWS < s->ntones) s->page = s->want_page;
    }
    const int nt = s->ntones;
    pthread_mutex_unlock(&s->mu);
    free(found);
    tiles_changed(s);
    if (code == 401) say(s, "Signed out: tap the person icon", "");
    else if (code == 429) say(s, "TONE3000 is busy", "Wait a minute, then try again");
    else if (code != 200) say(s, code ? "TONE3000 didn't answer" : "No internet connection", "");
    else if (!nt) say(s, "No results found", "Try different keywords or remove your search filters.");
    else { char a[64]; snprintf(a, sizeof a, "%d tones found", nt); say(s, a, ""); }
}
static void do_find(Plugin *s) { do_search(s, 0); }
static void do_models(Plugin *s) {
    pthread_mutex_lock(&s->mu);
    const Tone t = s->tone >= 0 && s->tone < s->ntones ? s->tones[s->tone] : (Tone){0};
    pthread_mutex_unlock(&s->mu);
    if (!t.id) return;
    const int ir = !strcmp(t.format, "ir");
    if (getenv("T3K_FAKE_TONES")) { fake_models(s, &t); return; }
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
        s->mpage = 0;
    }
    pthread_mutex_unlock(&s->mu);
    free(m);
    tiles_changed(s);
    char a[64];
    if (code != 200) snprintf(a, sizeof a, "Couldn't list this tone's models");
    else if (!n) snprintf(a, sizeof a, "No A2 models in this tone");
    else snprintf(a, sizeof a, "%d %s: tap DOWNLOAD", n, ir ? "IRs" : "A2 models");
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
    if (getenv("T3K_FAKE_TONES") && !s->access[0]) snprintf(s->access, sizeof s->access, "fake");   /* (tests) */
    if (s->access[0]) {
        char who[80];
        snprintf(who, sizeof who, "Signed in%s%s", s->user[0] ? " as " : "", s->user);
        say(s, who, "Loading tones...");
        pthread_mutex_lock(&s->mu);
        if (!s->job) s->job = J_FIND;   /* (the list fills by itself) */
        pthread_mutex_unlock(&s->mu);
    } else say(s, "Not signed in", "Tap the person icon, then SIGN IN");
    tiles_changed(s);
    for (;;) {
        pthread_mutex_lock(&s->mu);
        while (!s->job && !s->quit) pthread_cond_wait(&s->cv, &s->mu);
        const int j = s->job;
        s->job = J_NONE;
        s->busy = j;
        pthread_mutex_unlock(&s->mu);
        if (s->quit) break;
        if (j == J_DOWNLOAD || j == J_FIND) tiles_changed(s);
        if (j == J_SIGNIN) do_signin(s);
        else if (j == J_FIND) do_find(s);
        else if (j == J_MODELS) do_models(s);
        else if (j == J_DOWNLOAD) do_download(s);
        else if (j == J_MORE) do_search(s, 1);
        s->busy = 0;
        tiles_changed(s);
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
            if (s->repaint[i] == 2) {   /* (a tiny change first, so MPC repaints the text) */
                s->repaint[i] = 1;
                s->master(e, audioMasterAutomate, i, 0, NULL, s->norm[i] > 0.5f ? 0.999f : 0.001f);
            } else if (s->repaint[i] == 1 || s->automate[i]) {
                s->repaint[i] = s->automate[i] = 0;
                s->master(e, audioMasterAutomate, i, 0, NULL, s->norm[i]);
                s->update_display = 1;
            }
        if (s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    }
    for (int c = 0; c < 2; c++) {   /* the audio passes straight through (in place, or not) */
        const float *x = in && in[c] ? in[c] : NULL;
        for (int k = 0; k < n; k++) { float v = x ? x[k] : 0; if (!finite_bits(v) || v > 8 || v < -8) v = 0; out[c][k] = v; }
    }
}
static void pick_tone(Plugin *s, int t) {
    pthread_mutex_lock(&s->mu);
    const int ok = t >= 0 && t < s->ntones, same = ok && t == s->tone && s->nmodels;
    if (ok && !same) { s->tone = t; s->nmodels = 0; s->model = 0; }
    if (ok) s->page = t / ROWS;
    const int n = s->ntones;
    pthread_mutex_unlock(&s->mu);
    if (!ok) return;
    s->norm[kTone] = n > 1 ? (float)t / (float)(n - 1) : 0;
    tiles_changed(s);
    if (!same) post(s, J_MODELS);
}
static void pick_model(Plugin *s, int m) {
    pthread_mutex_lock(&s->mu);
    const int n = s->nmodels + 1;
    if (m >= 0 && m < n) s->model = m;
    pthread_mutex_unlock(&s->mu);
    s->norm[kModel] = n > 1 ? (float)m / (float)(n - 1) : 0;
    tiles_changed(s);
}
static void set_page(Plugin *s, int dir) {   /* scroll a page of cards: down past what's loaded fetches the next 25 */
    pthread_mutex_lock(&s->mu);
    const int np = (s->ntones + ROWS - 1) / ROWS, more = s->apage < s->apages && s->ntones < MAXT;
    int fetch = 0;
    if (dir < 0) { if (s->page > 0) s->page--; else if (!more && np > 1) s->page = np - 1; }
    else if (s->page + 1 < np) s->page++;
    else if (more) { s->want_page = s->page + 1; fetch = 1; }
    else if (np > 1) s->page = 0;
    if (!fetch && more && (s->page + 2) * ROWS > s->ntones) { s->want_page = -1; fetch = 1; }   /* (have the next ready) */
    pthread_mutex_unlock(&s->mu);
    if (fetch && !s->busy) post(s, J_MORE);
    tiles_changed(s);
}
static void sign_out(Plugin *s) {
    pthread_mutex_lock(&s->mu);
    s->access[0] = s->refresh[0] = s->user[0] = 0;
    s->expires = 0;
    s->ntones = s->nmodels = 0;
    s->tone = -1;
    s->page = 0;
    pthread_mutex_unlock(&s->mu);
    unlink(token_path());
    say(s, "Signed out", "Press SIGN IN to connect TONE3000");
    tiles_changed(s);
}
static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams) return;
    v = clampf(v, 0, 1);
    const int press = v > 0.5f;
    switch (i) {
    case kSignIn: if (press) post(s, J_SIGNIN); s->norm[i] = 0; return;
    case kSignOut: if (press) sign_out(s); s->norm[i] = 0; return;
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
    case kBrandPrev: case kBrandNext: case kStylePrev: case kStyleNext: {   /* the Brand / Style steppers' arrows */
        const int b = i == kBrandPrev || i == kBrandNext, k = b ? kBrand : kStyle, n = b ? NBRANDS : NSTYLES;
        if (press) {
            const int cur = option_of(s->norm[k], n), to = (cur + ((i == kBrandPrev || i == kStylePrev) ? -1 : 1) + n) % n;
            s->norm[k] = (float)to / (float)(n - 1);
            s->automate[k] = 1;
            if (s->access[0]) post(s, J_FIND);
        }
        s->norm[i] = 0;
        return;
    }
    case kPagePrev: case kPageNext: if (press) set_page(s, i == kPagePrev ? -1 : 1); s->norm[i] = 0; return;
    case kPage: {   /* a Q-Link nudge turns the page; an exact value changes nothing */
        if (fabsf(v - s->norm[i]) > 0.0001f && fabsf(v - roundf(v)) > 0.001f) set_page(s, v > s->norm[i] ? 1 : -1);
        s->automate[i] = 1;
        return;
    }
    case kStatus: case kStatus2: case kInfo1: case kInfo2: case kInfo3: case kPowered: case kSel: case kHelp1:
    case kHelp2: case kHdr: return;   /* (read-only) */
    case kView: case kSelKind: case kCardKind0: case kCardKind0 + 1: case kCardKind0 + 2: case kCardKind0 + 3:
    case kCardKind0 + 4: case kCardKind0 + 5: case kCk7: case kCk7 + 1: case kCk7 + 2: case kCk7 + 3:
        s->automate[i] = 1; return;   /* (ours: a restore changes nothing) */
    case kQueryOpen: case kSortByOpen: s->norm[i] = v > 0.5f ? 1.0f : 0.0f; return;   /* (the pop-up lists' open flags) */
    case kQuery: case kSortBy: {   /* a pick in a pop-up list: search again, close the list */
        const int n = i == kQuery ? NQUERIES : 4, o = option_of(v, n);
        s->norm[i] = (float)o / (float)(n - 1);
        if (i == kQuery) s->query = o; else s->sortby = o;
        s->norm[i + 1] = 0; s->automate[i + 1] = 1;
        if (s->view != V_LIB) go_view(s, V_LIB);
        if (s->access[0]) post(s, J_FIND);
        return;
    }
    case kCount: case kGearTxt: case kDate: case kDesc0: case kDesc0 + 1: case kDesc0 + 2: case kMakes: case kTags0:
    case kTags0 + 1: case kModHdr: case kMPage: case kBlank: return;
    case kMRow0: case kMRow0 + 1: case kMRow0 + 2: case kMRow0 + 3: case kMRow0 + 4: case kMRow0 + 5: return;
    default:
        if (is_tile(i)) {   /* a tap toggles the tile from what it showed: any change is a press */
            const int tap = (v > 0.5f) != (s->norm[i] > 0.5f);
            s->automate[i] = 1;
            if (!tap) return;
            if (i >= kRow0 && i < kRow0 + ROWS) {   /* a card: the tone's page */
                pthread_mutex_lock(&s->mu);
                const int t = s->page * ROWS + (i - kRow0), ok = t < s->ntones;
                pthread_mutex_unlock(&s->mu);
                if (ok) { pick_tone(s, t); s->view = V_TONE; }
            } else if (i >= kRow0 && i < kRow0 + 12) { /* (row_7..12: not on the page) */ }
            else if (i == kDl) { if (!s->busy) post(s, J_DOWNLOAD); }
            else if (i == kBack) go_view(s, V_LIB);
            else if (i == kNav1) go_view(s, s->view == V_SEARCH ? V_LIB : V_SEARCH);
            else if (i == kNav2) go_view(s, s->view == V_ACCOUNT ? V_LIB : V_ACCOUNT);
            else if (i == kScrUp || i == kScrDn) set_page(s, i == kScrUp ? -1 : 1);
            else if (i >= kTab0 && i < kTab0 + 3) { s->tab = i - kTab0; if (s->access[0]) post(s, J_FIND); }
            else if (i >= kGear0 && i < kGear0 + 5) {   /* a gear box (IR on its own: another format) */
                const int g = i - kGear0;
                s->gmask ^= 1 << g;
                if (s->gmask & (1 << g)) s->gmask = g == 4 ? 16 : s->gmask & 15;
                if (s->access[0]) post(s, J_FIND);
            }
            else if (i == kFilt) go_view(s, s->view == V_SEARCH ? V_LIB : V_SEARCH);
            else if (i == kFReset) { s->gmask = 0; if (s->access[0]) post(s, J_FIND); }
            else if (i == kFDone || i == kMClose || i == kSide0) go_view(s, V_LIB);
            else if (i == kSide0 + 1) go_view(s, V_ACCOUNT);
            else if (i == kQClear) { if (s->query) { s->query = 0; set_opt(s, kQuery, 0, NQUERIES); if (s->access[0]) post(s, J_FIND); } }
            else if (i >= kMDl0 && i < kMDl0 + MROWS) {   /* a model row's download */
                pthread_mutex_lock(&s->mu);
                const int m = s->mpage * MROWS + (i - kMDl0), ok = m < s->nmodels && !s->busy;
                if (ok) s->model = m + 1;
                pthread_mutex_unlock(&s->mu);
                if (ok) post(s, J_DOWNLOAD);
            }
            else if (i == kMAll) { if (!s->busy && s->nmodels) { s->model = 0; post(s, J_DOWNLOAD); } }
            else if (i == kMScrUp || i == kMScrDn) {
                const int np = (s->nmodels + MROWS - 1) / MROWS;
                if (np > 1) s->mpage = (s->mpage + (i == kMScrUp ? -1 : 1) + np) % np;
            }
            else if (i >= kBot0 && i < kBot0 + 4) {   /* the bottom tabs: Featured (trending, no filters), Browse, My Content */
                const int b = i - kBot0;
                if (b == 0) { s->tab = 1; s->sortby = 0; s->gmask = 0; s->query = 0; set_opt(s, kSortBy, 0, 4); set_opt(s, kQuery, 0, NQUERIES); }
                else if (b == 1) s->tab = 1;
                else if (b == 2) s->tab = 2;
                s->view = V_LIB;
                if (b < 3 && s->access[0]) post(s, J_FIND);
            }
            else if (i == kSignTile) post(s, J_SIGNIN);
            else sign_out(s);
            tiles_changed(s);
            return;
        }
        {
            const float was = s->norm[i];
            s->norm[i] = snap(&P[i], v, s->norm[i]);
            s->update_display = 1;
            if ((i == kSource || i == kSort || i == kBrand || i == kStyle) && s->norm[i] != was && s->access[0])
                post(s, J_FIND);   /* (no Find press: the list follows the choices) */
            if (i == kSource && s->view != V_LIB) go_view(s, V_LIB);
        }
    }
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }
static void fmt_count(long n, char *t, size_t max) {
    if (n >= 1000000) snprintf(t, max, "%.1fM", (double)n / 1e6);
    else if (n >= 1000) snprintf(t, max, "%.1fk", (double)n / 1e3);
    else snprintf(t, max, "%ld", n);
}
/* line `line` of text word-wrapped at `w` characters (the last shown line ends "..." when there's more) */
static void wrap_line(const char *txt, int w, int lines, int line, char *t, int max) {
    const char *p = txt;
    for (int l = 0;; l++) {
        while (*p == ' ') p++;
        int n = (int)strlen(p), cut = n;
        if (n > w) { cut = w; while (cut > 0 && p[cut] != ' ') cut--; if (!cut) cut = w; }
        if (l == line) {
            const int more = l == lines - 1 && n > cut;
            snprintf(t, (size_t)max, "%.*s%s", more ? (cut > w - 3 ? w - 3 : cut) : cut, p, more ? "..." : "");
            if (!t[0]) snprintf(t, (size_t)max, " ");
            return;
        }
        p += cut;
        if (!*p) { snprintf(t, (size_t)max, " "); return; }
    }
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
    case kPage: {
        const int np = (s->ntones + ROWS - 1) / ROWS;
        if (np) snprintf(t, (size_t)max, "%d / %d", s->page + 1, np);
        else snprintf(t, (size_t)max, " ");
        break;
    }
    case kHdr:
        if (s->view == V_LIB) snprintf(t, (size_t)max, "%s", SOURCES[option_of(s->norm[kSource], 7)]);
        else snprintf(t, (size_t)max, "%s", s->view == V_TONE ? "Tone" : s->view == V_SEARCH ? "Search" : "Account");
        break;
    case kBack: snprintf(t, (size_t)max, " "); break;
    case kTab0: case kTab0 + 1: case kTab0 + 2: snprintf(t, (size_t)max, "%s", TABS[i - kTab0]); break;
    case kQuery: snprintf(t, (size_t)max, "%s", QUERIES[s->query]); break;
    case kSortBy: snprintf(t, (size_t)max, "%s", SORTBYS[s->sortby]); break;
    case kGear0: case kGear0 + 1: case kGear0 + 2: case kGear0 + 3: case kGear0 + 4: snprintf(t, (size_t)max, "%s", GEAR_NAMES[i - kGear0]); break;
    case kFReset: snprintf(t, (size_t)max, "Reset"); break;
    case kFDone: snprintf(t, (size_t)max, "Done"); break;
    case kFilt: case kQClear: case kMClose: case kMAll: case kMScrUp: case kMScrDn: case kSide0: case kSide0 + 1: case kBlank:
        snprintf(t, (size_t)max, " "); break;
    case kBot0: case kBot0 + 1: case kBot0 + 2: case kBot0 + 3:
        snprintf(t, (size_t)max, "%s", (const char *[]){"Featured", "Browse", "My Content", "TONE3000"}[i - kBot0]); break;
    case kCount:
        if (!s->access[0]) snprintf(t, (size_t)max, "Not signed in");
        else if (s->busy == J_FIND) snprintf(t, (size_t)max, "Searching...");
        else snprintf(t, (size_t)max, "%ld results", s->total);
        break;
    case kGearTxt: {
        const char *g = tn ? tn->gear : "";
        snprintf(t, (size_t)max, "%s", !tn ? " " : !strcmp(tn->format, "ir") ? "IR" : !strcmp(g, "amp") ? "Amp Head" :
                 !strcmp(g, "amp-cab") || !strcmp(g, "full-rig") ? "Full Rig / Combo" : !strcmp(g, "pedal") ? "Pedal" :
                 !strcmp(g, "outboard") ? "Outboard" : !strcmp(g, "cab") ? "Cab" : g);
        break;
    }
    case kDate: snprintf(t, (size_t)max, "%s", tn && tn->date[0] ? tn->date : " "); break;
    case kDesc0: case kDesc0 + 1: case kDesc0 + 2: case kTags0: case kTags0 + 1: {   /* wrapped lines */
        const int desc = i < kMakes, line = desc ? i - kDesc0 : i - kTags0, nl = desc ? 3 : 2, w = desc ? 58 : 46;
        wrap_line(tn ? (desc ? tn->desc : tn->tags) : "", w, nl, line, t, max);
        break;
    }
    case kMakes: snprintf(t, (size_t)max, "%s", tn && tn->makes[0] ? tn->makes : "-"); break;
    case kModHdr:
        if (!tn) snprintf(t, (size_t)max, " ");
        else if (!s->nmodels) snprintf(t, (size_t)max, s->busy == J_MODELS ? "Loading models..." : "No A2 models");
        else snprintf(t, (size_t)max, "%s (%d)", !strcmp(tn->format, "ir") ? "IR" : "NAM A2", s->nmodels);
        break;
    case kMPage: {
        const int np = (s->nmodels + MROWS - 1) / MROWS;
        if (np > 1) snprintf(t, (size_t)max, "%d / %d", s->mpage + 1, np); else snprintf(t, (size_t)max, " ");
        break;
    }
    case kNav1: case kNav2: case kScrUp: case kScrDn: snprintf(t, (size_t)max, " "); break;
    case kSel: snprintf(t, (size_t)max, "%s", tn ? tn->title : "No tone picked"); break;
    case kDl:
        if (s->busy == J_DOWNLOAD) snprintf(t, (size_t)max, "DOWNLOADING...");
        else if (!tn || !s->nmodels) snprintf(t, (size_t)max, tn ? "WAIT: LISTING MODELS" : "DOWNLOAD");
        else if (s->model == 0) snprintf(t, (size_t)max, "DOWNLOAD ALL %d", s->nmodels);
        else snprintf(t, (size_t)max, "DOWNLOAD THIS MODEL");
        break;
    case kSignTile: snprintf(t, (size_t)max, s->access[0] ? "SIGN IN AGAIN" : "SIGN IN"); break;
    case kSignOutTile: snprintf(t, (size_t)max, "SIGN OUT"); break;
    case kHelp1: snprintf(t, (size_t)max, "Sign in once: the MPC remembers it"); break;
    case kHelp2: snprintf(t, (size_t)max, "Files go to NAM/Models or NAM/IRs"); break;
    case kTonePrev: case kModelPrev: case kBrandPrev: case kStylePrev: snprintf(t, (size_t)max, "<"); break;
    case kToneNext: case kModelNext: case kBrandNext: case kStyleNext: snprintf(t, (size_t)max, ">"); break;
    case kSignIn: snprintf(t, (size_t)max, "Sign In"); break;
    case kSignOut: snprintf(t, (size_t)max, "Sign Out"); break;
    case kFind: snprintf(t, (size_t)max, "Find"); break;
    case kDownload: snprintf(t, (size_t)max, "Download"); break;
    default:
        if (i >= kUp0 && i < kCk7) {   /* a card's uploader / downloads / favorites / models */
            const int f = (i - kUp0) / 10, k = s->page * ROWS + (i - kUp0) % 10;
            if (k >= s->ntones) snprintf(t, (size_t)max, " ");
            else if (f == 0) snprintf(t, (size_t)max, "%s", s->tones[k].creator);
            else fmt_count(f == 1 ? s->tones[k].downloads : f == 2 ? s->tones[k].favs : s->tones[k].models ? s->tones[k].models
                           : s->tones[k].a2 + s->tones[k].irs, t, (size_t)max);
        } else if (i >= kMRow0 && i < kMRow0 + MROWS) {
            const int m = s->mpage * MROWS + (i - kMRow0);
            snprintf(t, (size_t)max, "%s", m < s->nmodels ? s->models[m].name : " ");
        } else if (i >= kRow0 && i < kRow0 + 12) {
            const int k = s->page * ROWS + (i - kRow0);
            if (i >= kRow0 + ROWS) snprintf(t, (size_t)max, " ");
            else if (k < s->ntones) snprintf(t, (size_t)max, "%s", s->tones[k].title);
            else if (k == 0 && !s->access[0]) snprintf(t, (size_t)max, "Not signed in: tap the person icon");
            else if (k == 0 && s->busy == J_FIND) snprintf(t, (size_t)max, "Looking on TONE3000...");
            else snprintf(t, (size_t)max, " ");
        } else pdisplay(&P[i], s->norm[i], t);
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
    case effGetParamDisplay:   /* (JUCE reads up to 256: the pop-up's lines are longer than 40) */
        if (idx >= 0 && idx < kNumParams) { char t[96]; display(s, idx, t, 72); copy_str(p, t, 72); }
        return 1;
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
    if (list >= 2) {   /* Brands, Styles */
        const int k = list == 2 ? kBrand : kStyle, n = list == 2 ? NBRANDS : NSTYLES;
        if (op == PL_COUNT) return n;
        if (op == PL_CURRENT) return option_of(s->norm[k], n);
        if (op == PL_HEADING || i < 0 || i >= n) return 0;
        if (op == PL_NAME) snprintf(out, (size_t)max, "%s", list == 2 ? BRANDS[i] : STYLES[i]);
        else if (op == PL_CHOOSE) { s->norm[k] = (float)i / (float)(n - 1); s->automate[k] = 1; if (s->access[0]) post(s, J_FIND); }
        return 0;
    }
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
static const char *const LIST_KEYS[] = {"tone", "model", "brand", "style"};
static const char *const LIST_TITLES[] = {"Tones", "Models", "Brands", "Styles"};
static const PickLists LISTS = {4, LIST_KEYS, LIST_TITLES, t3k_pick, 0};

static AEffect *make(audioMasterCallback master) {
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    s->tone = -1;
    s->tab = 1;
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
