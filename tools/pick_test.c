/* pick_test: checks a plugin's full-page lists (tools/picklist.h) offline.
 *   pick_test <plugin.so> ["<param name>=<display text>"...]   exit 0 = PASSED
 * (the settings first, e.g. "A Algorithm=DrivPdls" so its Pedal knob is a list; a list that stays shut, a knob
 * that isn't a choice, is skipped)
 * (NAM_MPC_DIR / SFZ_MPC_DIR as the plugin's own test wants them.)
 * For every "<title> List" parameter: opening it shows a title and rows, opening another closes it, the pages
 * turn, a tap on the second entry of page 1 picks it and closes the list, and reopened, that entry is the one
 * marked "> " (the plugin took it). Close closes. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../nam-mpc/src/vst2.h"

#define N 128
static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)
static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) { (void)e; (void)i; (void)v; (void)p; (void)o; return op == 1 ? 2400 : 0; }
static AEffect *fx;
static void name(int i, char *t) { memset(t, 0, 64); fx->dispatcher(fx, effGetParamName, i, 0, t, 0); }
static void disp(int i, char *t) { memset(t, 0, 64); fx->dispatcher(fx, effGetParamDisplay, i, 0, t, 0); }
static int param(const char *want) {
    for (int i = 0; i < fx->numParams; i++) { char t[64]; name(i, t); if (!strcmp(t, want)) return i; }
    return -1;
}
static void run(int blocks) {
    static float a[N], b[N], c[N], d[N];
    float *in[2] = {a, b}, *out[2] = {c, d};
    for (int k = 0; k < blocks; k++) fx->processReplacing(fx, in, out, N);
}
static void tap(int i) { fx->setParameter(fx, i, 1); run(1); }
static void settings(int argc, char **argv) {   /* the settings asked for (again before each list: a program pick changes them) */
    for (int a = 2; a < argc; a++) {
        char pn[64], want[64], t[64];
        const char *eq = strchr(argv[a], '=');
        if (!eq) continue;
        snprintf(pn, sizeof pn, "%.*s", (int)(eq - argv[a]), argv[a]);
        snprintf(want, sizeof want, "%s", eq + 1);
        const int i = param(pn);
        int ok = 0;
        for (int k = 0; i >= 0 && k <= 4000 && !ok; k++) { fx->setParameter(fx, i, (float)k / 4000); disp(i, t); ok = !strcmp(t, want); }
        CHECK(ok, "can't set %s to %s", pn, want);
        run(2);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: pick_test <plugin.so> [\"<param>=<text>\"...]\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    AEffect *(*entry)(audioMasterCallback) = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    fx = entry(host);
    fx->dispatcher(fx, effOpen, 0, 0, NULL, 0);
    fx->dispatcher(fx, effSetSampleRate, 0, 0, NULL, 48000);
    fx->dispatcher(fx, effSetBlockSize, 0, N, NULL, 0);
    fx->dispatcher(fx, effMainsChanged, 0, 1, NULL, 0);
    for (int w = 0; w < 30; w++) { run(4); usleep(100000); }   /* (libraries scanned in the background) */
    settings(argc, argv);
    const int row1 = param("List 1"), prev = param("List <"), next = param("List >"), close = param("List Close"), title = param("List Title");
    char t[64], u[64];
    CHECK(row1 >= 0 && prev >= 0 && next >= 0 && close >= 0 && title >= 0, "no list parameters");
    if (failures) { printf("FAILED (%d)\n", failures); return 1; }
    int lists[64], nl = 0;
    for (int i = row1 - 1; i >= 0 && nl < 64; i--) {   /* the open flags: just before the rows */
        name(i, t);
        const size_t n = strlen(t);
        if (n <= 5 || strcmp(t + n - 5, " List")) break;
        lists[nl++] = i;
    }
    for (int l = 0; l < nl / 2; l++) { const int x = lists[l]; lists[l] = lists[nl - 1 - l]; lists[nl - 1 - l] = x; }
    printf("%d lists\n", nl);
    CHECK(nl > 0, "no lists");
    for (int l = 0; l < nl; l++) {
        const int f = lists[l];
        settings(argc, argv);
        name(f, t);
        tap(f);
        disp(f, u);
        if (strcmp(u, "Open")) { printf("  %s: shut (nothing to list now)\n", t); continue; }
        for (int o = 0; o < nl; o++) if (o != l) { char v[64]; disp(lists[o], v); CHECK(!strcmp(v, "Closed"), "%s open beside %s", v, t); }
        char ti[64];
        disp(title, ti);
        int rows = 0;
        for (int r = 0; r < 24; r++) { disp(row1 + r, u); if (strcmp(u, " ")) rows++; }
        printf("  %s: \"%s\", %d rows", t, ti, rows);
        if (strstr(ti, ": none")) { printf(" (empty)\n"); tap(close); continue; }
        CHECK(rows > 0, "%s shows no rows", t);
        if (strchr(ti, '/')) {   /* pages: forward and back, then to page 1 */
            char t2[64];
            tap(next);
            disp(title, t2);
            CHECK(strcmp(t2, ti), "%s: Page > didn't turn (%s)", t, t2);
            tap(prev);
            disp(title, t2);
            CHECK(!strcmp(t2, ti), "%s: < Page didn't come back (%s)", t, t2);
            for (int k = 0; k < 100; k++) { disp(title, t2); const char *s = strrchr(t2, ' '); if (s && !strncmp(s, " 1/", 3)) break; tap(prev); }
        }
        const int pick = rows > 1 ? 1 : 0;
        char want[64];
        disp(row1 + pick, u);
        snprintf(want, sizeof want, "%s", strncmp(u, "> ", 2) ? u : u + 2);
        printf(", first \"%s\", picking \"%s\"", (disp(row1, u), u), want);
        tap(row1 + pick);
        disp(f, u);
        CHECK(!strcmp(u, "Closed"), "%s didn't close on a pick", t);
        for (int w = 0; w < 50; w++) { run(4); usleep(50000); }   /* (a model loads in the background) */
        tap(f);
        int marked = -1;
        for (int r = 0; r < 24; r++) { disp(row1 + r, u); if (!strncmp(u, "> ", 2)) marked = r; }
        disp(row1 + pick, u);
        printf(", then marked row %d\n", marked + 1);
        CHECK(marked == pick && !strcmp(u + 2, want), "%s: the pick (\"%s\") isn't the current entry (row %d: %s)", t, want, marked + 1, u);
        tap(close);
        disp(f, u);
        CHECK(!strcmp(u, "Closed"), "%s: Close didn't close", t);
    }
    fx->dispatcher(fx, effClose, 0, 0, NULL, 0);
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
