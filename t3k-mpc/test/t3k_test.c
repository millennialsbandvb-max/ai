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
static void disp(const char *name, char *t) { memset(t, 0, 64); e->dispatcher(e, effGetParamDisplay, param(name), 0, t, 0); }
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
    if (getenv("T3K_FAKE_TONES")) {   /* the list page offline: T3K_FAKE_TONES=30 (made-up tones, no network) */
        CHECK(wait_for("tones found", "models:", 5, t), "the list didn't fill by itself (%s)", t);
        run(4);
        char r1[64], r12[64], pg[64], dl[64];
        disp("Row 1", r1); disp("Row 12", r12); disp("Page", pg);
        printf("  rows: %s .. %s, %s\n", r1, r12, pg);
        CHECK(!strcmp(r1, "Fake Tone 1") && !strcmp(r12, "Fake Tone 12") && strstr(pg, "of 3"), "the rows don't show the first page");
        CHECK(e->getParameter(e, param("Row 1")) > 0.5f && e->getParameter(e, param("Row 2")) < 0.5f, "the picked row isn't lit");
        CHECK(wait_for("models:", NULL, 5, t), "no models (%s)", t);
        run(2);
        disp("Download Tile", dl); printf("  %s\n", dl);
        CHECK(!strcmp(dl, "DOWNLOAD ALL 3"), "the download tile says %s", dl);
        e->setParameter(e, param("Row 5"), 1.0f);   /* a tap on row 5 */
        run(2);
        disp("Selected", t); printf("  tapped row 5: %s\n", t);
        CHECK(!strcmp(t, "Fake Tone 5") && e->getParameter(e, param("Row 5")) > 0.5f && e->getParameter(e, param("Row 1")) < 0.5f,
              "tapping row 5 didn't pick it");
        e->setParameter(e, param("Row 5"), 0.0f);   /* a second tap on the lit row: still picked */
        run(2);
        disp("Selected", t);
        CHECK(!strcmp(t, "Fake Tone 5") && e->getParameter(e, param("Row 5")) > 0.5f, "a second tap unpicked it");
        press("Page >"); run(2);
        disp("Row 1", r1); disp("Page", pg); printf("  page >: %s, %s\n", r1, pg);
        CHECK(!strcmp(r1, "Fake Tone 13") && !strncmp(pg, "Page 2", 6) && e->getParameter(e, param("Row 5")) < 0.5f, "Page > didn't turn");
        press("Page >"); run(2); disp("Row 6", t); disp("Row 7", r12);
        CHECK(!strcmp(t, "Fake Tone 30") && !strcmp(r12, " "), "the last page shows %s / %s", t, r12);
        press("Page >"); run(2); disp("Row 1", r1);
        CHECK(!strcmp(r1, "Fake Tone 1"), "pages don't wrap (%s)", r1);
        e->setParameter(e, param("Row 3"), 1.0f);
        for (int k = 0; k < 50; k++) { run(1); disp("Download Tile", dl); if (strstr(dl, "ALL")) break; usleep(100000); }
        press("Model >"); run(2);
        disp("Download Tile", dl); disp("Model", t); printf("  %s: %s\n", t, dl);
        CHECK(!strcmp(dl, "DOWNLOAD THIS MODEL"), "one model picked: the tile says %s", dl);
        e->setParameter(e, param("Source"), 4.0f / 6);   /* IRs: the search runs by itself */
        for (int k = 0; k < 50; k++) { run(1); disp("Row 1", r1); if (strstr(r1, "IRs")) break; usleep(100000); }
        printf("  Source IRs: %s\n", r1);
        CHECK(!strcmp(r1, "Fake IRs 1"), "a new Source didn't search (%s)", r1);
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
