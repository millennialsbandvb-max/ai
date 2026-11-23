/* t3k_test: checks the TONE3000 plugin offline-ish.   t3k_test <TONE3000.so>   exit 0 = PASSED
 * Always: the parameters, the audio passing straight through (in place too), the JSON reader on sample answers.
 * With the internet: Sign In gets a code from TONE3000 (the page shows tone3000.com/activate and the code).
 * With T3K_TOKEN_FILE holding a signed-in token (lines: access, refresh, expiry, user): Find (Amps, Marshall) lists
 * tones, a tone's A2 models are listed, and one model downloads into T3K_DEST (default a temporary folder). */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../nam-mpc/src/vst2.h"

#define N 128
static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)
static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) { (void)e; (void)i; (void)v; (void)p; (void)o; return op == 1 ? 2400 : 0; }
static AEffect *e;
static int param(const char *want) {
    for (int i = 0; i < e->numParams; i++) { char t[64] = {0}; e->dispatcher(e, effGetParamName, i, 0, t, 0); if (!strcmp(t, want)) return i; }
    printf("  (no parameter %s)\n", want); failures++; return 0;
}
static void disp(const char *name, char *t) { char b[256] = {0}; e->dispatcher(e, effGetParamDisplay, param(name), 0, b, 0); snprintf(t, 64, "%s", b); }
static void press(const char *name) { const int i = param(name); e->setParameter(e, i, 1); e->setParameter(e, i, 0); }
static void run(int blocks) { static float a[N], b[N]; float *io[2] = {a, b}; for (int k = 0; k < blocks; k++) e->processReplacing(e, io, io, N); }
/* wait (running blocks) until Status shows one of the words, up to secs */
static int wait_for(const char *w1, const char *w2, int secs, char *t) {
    for (int i = 0; i < secs * 10; i++) {
        run(1); disp("Status", t);
        if (strstr(t, w1) || (w2 && strstr(t, w2))) return 1;
        usleep(100000);
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: t3k_test <TONE3000.so>\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    AEffect *(*entry)(audioMasterCallback) = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, 48000);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    char t[64], name[64] = {0};
    e->dispatcher(e, effGetEffectName, 0, 0, name, 0);
    printf("%s, %d parameters\n", name, e->numParams);
    disp("Powered", t);
    CHECK(!strcmp(t, "Powered by TONE3000"), "the TONE3000 credit is missing (%s)", t);
    /* audio straight through, in place */
    {
        float a[N], b[N], *io[2] = {a, b};
        for (int k = 0; k < N; k++) { a[k] = 0.3f * sinf((float)k * 0.1f); b[k] = -a[k]; }
        float c0 = a[17], c1 = b[33];
        e->processReplacing(e, io, io, N);
        CHECK(a[17] == c0 && b[33] == c1, "the audio doesn't pass straight through");
    }
    usleep(300000);
    run(2);
    disp("Status", t);
    printf("  status at start: %s\n", t);
    if (getenv("T3K_FAKE_TONES")) {   /* the HeadRush-editor-style browser offline: T3K_FAKE_TONES=30 (made-up tones) */
        const int view = param("View"), ck1 = param("Card 1 Kind"), ck10 = param("Card 10 Kind"), selk = param("Tone Kind");
        CHECK(wait_for("tones found", NULL, 5, t), "the cards didn't fill by itself (%s)", t);
        run(4);
        char a[64], b[64], c[64];
        disp("Row 1", a); disp("Row 10", b); disp("Row 11", c);
        printf("  cards: %s .. %s\n", a, b);
        CHECK(!strcmp(a, "Fake Tone 1") && !strcmp(b, "Fake Tone 10") && !strcmp(c, " "), "the cards don't show the first 10");
        disp("Uploader 2", a); disp("Downloads 2", b); disp("Favorites 2", c); printf("  card 2: %s, %s dl, %s fav\n", a, b, c);
        CHECK(!strcmp(a, "maker1") && !strcmp(b, "2.0k") && !strcmp(c, "10"), "card 2's uploader / counts");
        disp("Results", a); printf("  %s\n", a);
        CHECK(!strcmp(a, "30 results"), "the result count (%s)", a);
        CHECK(e->getParameter(e, ck1) > 0.1f && e->getParameter(e, ck10) > 0.1f && e->getParameter(e, selk) < 0.01f, "card pictures");
        CHECK(e->getParameter(e, param("Tab Browse")) > 0.5f && e->getParameter(e, param("Tab Favorites")) < 0.5f, "Browse tab lit");
        e->setParameter(e, param("Row 5"), 1.0f);   /* tap card 5: the tone pop-up */
        for (int k = 0; k < 50; k++) { run(1); disp("Models Header", a); if (strstr(a, "(")) break; usleep(100000); }
        disp("Selected", b); disp("View", c); printf("  tapped card 5: %s (%s), %s\n", b, c, a);
        CHECK(!strcmp(b, "Fake Tone 5") && !strcmp(c, "Tone") && !strcmp(a, "NAM A2 (8)"), "card 5 didn't open its pop-up");
        CHECK(e->getParameter(e, ck1) < 0.01f && e->getParameter(e, selk) > 0.1f, "pop-up: card pictures off, its picture on");
        disp("Gear Type", a); disp("Date", b); disp("Description 1", c); printf("  %s, %s\n  %s\n", a, b, c);
        CHECK(!strcmp(a, "Amp Head") && !strcmp(b, "11 November 2026") && strlen(c) > 20 && strlen(c) <= 58, "the pop-up's header / description");
        disp("Description 2", a); disp("Tags 1", b); printf("  %s\n  tags: %s\n", a, b);
        disp("Model Row 1", a); disp("Model Row 6", b); disp("Models Page", c); printf("  models: %s .. %s (%s)\n", a, b, c);
        CHECK(strstr(a, "model 1") && strstr(b, "model 6") && !strcmp(c, "1 / 2"), "model rows");
        e->setParameter(e, param("Models Down"), 1.0f); run(2); disp("Model Row 1", a); disp("Model Row 3", b);
        CHECK(strstr(a, "model 7") && !strcmp(b, " "), "model rows page 2 (%s, %s)", a, b);
        e->setParameter(e, param("Close Tile"), 1.0f); run(2); disp("View", a);
        CHECK(!strcmp(a, "Library") && e->getParameter(e, ck1) > 0.1f, "X didn't close the pop-up");
        e->setParameter(e, param("Down Tile"), 1.0f); run(2); disp("Row 1", a); printf("  down: %s\n", a);
        CHECK(!strcmp(a, "Fake Tone 11"), "scrolling down");
        e->setParameter(e, param("Down Tile"), 1.0f); run(2); e->setParameter(e, param("Down Tile"), 1.0f); run(2);
        disp("Row 1", a); CHECK(!strcmp(a, "Fake Tone 1"), "scrolling past the end comes back to the top (%s)", a);
        e->setParameter(e, param("Filter Icon"), 1.0f); run(2); disp("View", a);
        CHECK(!strcmp(a, "Search"), "the filter icon didn't open the drawer (%s)", a);
        e->setParameter(e, param("Gear Pedal"), 1.0f); run(2);
        CHECK(e->getParameter(e, param("Gear Pedal")) > 0.5f, "the Pedal box didn't tick");
        e->setParameter(e, param("Gear IR"), 1.0f); run(2);
        CHECK(e->getParameter(e, param("Gear IR")) > 0.5f && e->getParameter(e, param("Gear Pedal")) < 0.5f, "IR clears the other boxes");
        e->setParameter(e, param("Filter Reset"), 1.0f); run(2);
        CHECK(e->getParameter(e, param("Gear IR")) < 0.5f, "Reset");
        e->setParameter(e, param("Filter Done"), 1.0f); run(2); disp("View", a);
        CHECK(!strcmp(a, "Library"), "Done");
        e->setParameter(e, param("Search List"), 1.0f); e->setParameter(e, param("Search"), 1.0f / 38); run(4);
        disp("Search", a); printf("  search: %s\n", a);
        CHECK(!strcmp(a, "plexi") && e->getParameter(e, param("Search List")) < 0.5f, "a pick in the search list");
        e->setParameter(e, param("Tab My Tones"), 1.0f); run(2);
        CHECK(e->getParameter(e, param("Tab My Tones")) > 0.5f && e->getParameter(e, param("Tab Browse")) < 0.5f, "My Tones tab");
        e->setParameter(e, param("Account Side Tile"), 1.0f); run(2); disp("View", a);
        CHECK(!strcmp(a, "Account"), "the account icon");
        CHECK(view >= 0, "");
    } else if (!getenv("T3K_TOKEN_FILE") || access(getenv("T3K_TOKEN_FILE"), R_OK)) {
        press("Sign In");
        const int ok = wait_for("activate", "internet", 20, t);
        char t2[64];
        disp("Status 2", t2);
        printf("  sign in: %s / %s\n", t, t2);
        CHECK(ok && strstr(t, "activate") && strstr(t2, "code"), "Sign In didn't show a code");
    } else {
        int src = param("Source"), brand = param("Brand");
        e->setParameter(e, src, 1.0f / 6);         /* Amps */
        e->setParameter(e, brand, 2.0f / 23);      /* Marshall */
        char x[64];
        disp("Source", x); disp("Brand", t);
        printf("  find: %s, %s\n", x, t);
        press("Find");
        CHECK(wait_for("tones found", "TONE3000", 60, t), "Find didn't finish (%s)", t);
        printf("  %s\n", t);
        CHECK(strstr(t, "tones found") && atoi(t) > 0, "no tones found");
        disp("Tone", t); printf("  tone: %s\n", t);
        disp("Creator", t); printf("  %s\n", t);
        disp("Gear", t); printf("  %s\n", t);
        CHECK(wait_for("models:", "No A2", 60, t), "the models weren't listed (%s)", t);
        printf("  %s\n", t);
        disp("Model", t); printf("  model: %s\n", t);
        press("Model >");
        disp("Model", t); printf("  model: %s\n", t);
        char dir[256];
        snprintf(dir, sizeof dir, "%s", getenv("T3K_DEST") ? getenv("T3K_DEST") : "/tmp/t3k-test-dest");
        mkdir(dir, 0755);
        setenv("T3K_DEST", dir, 1);
        press("Download");
        CHECK(wait_for("Downloaded", NULL, 180, t), "the download didn't finish (%s)", t);
        char t2[64];
        disp("Status 2", t2);
        printf("  %s: %s\n", t, t2);
        CHECK(!strncmp(t, "Downloaded 1 to", 15), "the model didn't download");
        char cmd[400];
        snprintf(cmd, sizeof cmd, "ls -la \"%s\"/Models/* | head -5", dir);
        if (system(cmd)) {}
    }
    e->dispatcher(e, effClose, 0, 0, NULL, 0);
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
