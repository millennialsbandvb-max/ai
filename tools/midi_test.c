/* midi_test: checks a plugin's MIDI from the MPC's inputs (tools/picklist.h, mpc-remote/preload/extmidi.c) offline.
 * This program stands in for the MPC add-on: it has mpc_ext_midi_read (built with -rdynamic, so the plugin's weak
 * reference finds it) and a ring of messages the steps put in.
 *   midi_test <plugin.so> <step>...   exit 0 = PASSED
 * Steps: "set <param>=<text>" (finds the value showing that text), "send <st> <d1> <d2>" (hex; then a few blocks
 * run), "expect <param>=<text>", "reload" (the state into a new instance: its chunk, or every parameter). */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../nam-mpc/src/vst2.h"

static unsigned char ring[256][3];
static unsigned head;
__attribute__((visibility("default"))) int mpc_ext_midi_read(unsigned *pos, unsigned char msg[3]) {
    if (*pos == 0xffffffffu) { *pos = head; return 0; }
    if (*pos == head) return 0;
    memcpy(msg, ring[*pos % 256], 3);
    (*pos)++;
    return 1;
}

static intptr_t host(AEffect *e, int32_t op, int32_t i, intptr_t v, void *p, float o) { (void)e; (void)i; (void)v; (void)p; (void)o; return op == 1 ? 2400 : 0; }
static AEffect *(*entry)(audioMasterCallback);
static AEffect *make(void) {
    AEffect *e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, NULL, 48000);
    e->dispatcher(e, effSetBlockSize, 0, 128, NULL, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0);
    return e;
}
static void run(AEffect *e, int blocks) {
    static float a[128], b[128], c[128], d[128];
    float *in[2] = {a, b}, *out[2] = {c, d};
    for (int k = 0; k < blocks; k++) e->processReplacing(e, in, out, 128);
}
static int param(AEffect *e, const char *want) {
    for (int i = 0; i < e->numParams; i++) { char t[64] = {0}; e->dispatcher(e, effGetParamName, i, 0, t, 0); if (!strcmp(t, want)) return i; }
    return -1;
}
static void disp(AEffect *e, int i, char *t) { memset(t, 0, 64); e->dispatcher(e, effGetParamDisplay, i, 0, t, 0); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: midi_test <plugin.so> <step>...\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    entry = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    AEffect *e = make();
    run(e, 4);
    int failures = 0;
    for (int a = 2; a < argc; a++) {
        char pn[64], want[64], t[64];
        const char *s = argv[a], *eq = strchr(s, '=');
        if (!strncmp(s, "set ", 4) || !strncmp(s, "expect ", 7)) {
            const int set = s[0] == 's';
            const char *nm = s + (set ? 4 : 7);
            snprintf(pn, sizeof pn, "%.*s", eq ? (int)(eq - nm) : 0, nm);
            snprintf(want, sizeof want, "%s", eq ? eq + 1 : "");
            const int i = param(e, pn);
            int ok = i >= 0;
            if (set) {
                ok = 0;
                for (int k = 0; i >= 0 && k <= 2000 && !ok; k++) { e->setParameter(e, i, (float)k / 2000); disp(e, i, t); ok = !strcmp(t, want); }
                run(e, 2);
            } else if (ok) { disp(e, i, t); ok = !strcmp(t, want); }
            printf("  %s %s: %s%s\n", set ? "set" : "expect", pn, ok ? "" : "FAIL, ", ok ? want : (i < 0 ? "no such parameter" : t));
            failures += !ok;
        } else if (!strncmp(s, "send ", 5)) {
            unsigned x, y, z;
            if (sscanf(s + 5, "%x %x %x", &x, &y, &z) != 3) { printf("  bad step %s\n", s); failures++; continue; }
            ring[head % 256][0] = (unsigned char)x; ring[head % 256][1] = (unsigned char)y; ring[head % 256][2] = (unsigned char)z;
            head++;
            run(e, 3);
            printf("  sent %02x %02x %02x\n", x, y, z);
        } else if (!strcmp(s, "reload")) {
            AEffect *e2 = make();
            if (e->flags & effFlagsProgramChunks) {
                void *c = NULL;
                const intptr_t n = e->dispatcher(e, effGetChunk, 0, 0, &c, 0);
                e2->dispatcher(e2, effSetChunk, 0, n, c, 0);
                printf("  reload: chunk of %ld bytes\n", (long)n);
            } else {
                for (int i = 0; i < e->numParams; i++) e2->setParameter(e2, i, e->getParameter(e, i));
                printf("  reload: every parameter\n");
            }
            for (int w = 0; w < 20; w++) { run(e2, 2); usleep(20000); }
            e->dispatcher(e, effClose, 0, 0, NULL, 0);
            e = e2;
        } else { printf("  bad step %s\n", s); failures++; }
    }
    e->dispatcher(e, effClose, 0, 0, NULL, 0);
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
