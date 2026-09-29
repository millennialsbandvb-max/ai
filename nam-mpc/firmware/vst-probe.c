/* vst-probe: load one VST2 plugin outside MPC and print its MPC.settings plugin-list entry.
 *   vst-probe <plugin.so> <file= path to write in the entry>
 * Prints one <PLUGIN .../> line on stdout and exits 0, or an error on stderr and exits non-zero. The boot loader
 * (nam-mpc-boot.sh) runs this in its own process with a timeout, so a plugin that crashes or hangs on load only
 * takes the probe down, never MPC. Hand-written VST2 ABI subset (vst2.h), no Steinberg SDK. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/vst2.h"

enum { audioMasterVersion = 1, audioMasterGetSampleRate = 16, audioMasterGetBlockSize = 17,
       audioMasterGetVendorString = 32, audioMasterGetProductString = 33, audioMasterGetVendorVersion = 34,
       audioMasterCanDo = 37 };
enum { effFlagsIsSynth = 1 << 8, kPlugCategSynth = 2, kPlugCategShell = 10 };

static intptr_t host(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    (void)e; (void)idx; (void)v; (void)o;
    switch (op) {
    case audioMasterVersion: return 2400;
    case audioMasterGetSampleRate: return 44100;
    case audioMasterGetBlockSize: return 128;
    case audioMasterGetVendorString: if (p) strcpy(p, "Akai"); return 1;
    case audioMasterGetProductString: if (p) strcpy(p, "MPC"); return 1;
    case audioMasterGetVendorVersion: return 1;
    default: return 0;
    }
}

/* XML attribute text: printable ASCII only, with the five XML specials escaped */
static void attr(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '&') fputs("&amp;", stdout);
        else if (c == '<') fputs("&lt;", stdout);
        else if (c == '>') fputs("&gt;", stdout);
        else if (c == '"') fputs("&quot;", stdout);
        else if (c == '\'') fputs("&apos;", stdout);
        else if (c >= 32 && c < 127) putchar(c);
    }
}

static void get_string(AEffect *e, int op, char *buf, size_t n) {
    memset(buf, 0, n);
    e->dispatcher(e, op, 0, 0, buf, 0);
    buf[n - 1] = 0;
    /* trim trailing spaces */
    for (size_t k = strlen(buf); k > 0 && buf[k - 1] == ' '; k--) buf[k - 1] = 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <plugin.so> <file path for the entry>\n", argv[0]); return 2; }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "can't load: %s\n", dlerror()); return 3; }
    AEffect *(*entry)(audioMasterCallback) = (AEffect * (*)(audioMasterCallback)) dlsym(lib, "VSTPluginMain");
    if (!entry) entry = (AEffect * (*)(audioMasterCallback)) dlsym(lib, "main");
    if (!entry) { fprintf(stderr, "not a VST2 plugin (no VSTPluginMain)\n"); return 4; }
    AEffect *e = entry(host);
    if (!e || e->magic != VST_MAGIC) { fprintf(stderr, "not a VST2 plugin (bad AEffect)\n"); return 5; }
    e->dispatcher(e, effOpen, 0, 0, NULL, 0);

    char name[256], vendor[256], product[256];
    get_string(e, effGetEffectName, name, 64);
    get_string(e, effGetProductString, product, 64);
    get_string(e, effGetVendorString, vendor, 64);
    if (!name[0]) snprintf(name, sizeof name, "%s", product);
    if (!name[0]) {   /* fall back to the file name without .so */
        const char *b = strrchr(argv[1], '/');
        snprintf(name, sizeof name, "%s", b ? b + 1 : argv[1]);
        char *dot = strrchr(name, '.');
        if (dot) *dot = 0;
    }
    if (!vendor[0]) snprintf(vendor, sizeof vendor, "Unknown");
    const int category = (int)e->dispatcher(e, effGetPlugCategory, 0, 0, NULL, 0);
    if (category == kPlugCategShell) { fprintf(stderr, "shell plugins (several plugins in one file) aren't supported\n"); return 6; }
    const int synth = (e->flags & effFlagsIsSynth) || category == kPlugCategSynth;
    const int ins = e->numInputs, outs = e->numOutputs;
    const unsigned uid = (unsigned)e->uniqueID;
    const int version = e->version;

    printf("<PLUGIN name=\"");
    attr(name);
    printf("\" descriptiveName=\"");
    attr(name);
    printf("\" format=\"VST\" category=\"%s\" manufacturer=\"", synth ? "Synth" : "Effect");
    attr(vendor);
    if (version > 0xffff)   /* packed as bytes (DPF, JUCE): 0x030211 = 3.2.17 */
        printf("\" version=\"%d.%d.%d\" file=\"", (version >> 16) & 0xff, (version >> 8) & 0xff, version & 0xff);
    else                    /* decimal digits: 1000 = 1.0.0 */
        printf("\" version=\"%d.%d.%d\" file=\"", version / 1000, (version / 100) % 10, version % 100);
    attr(argv[2]);
    printf("\" uid=\"%x\" isInstrument=\"%d\" fileTime=\"0\" infoUpdateTime=\"0\" numInputs=\"%d\" numOutputs=\"%d\" "
           "isShell=\"0\"/>\n", uid, synth, ins, outs);
    fflush(stdout);
    /* The plugin is left open and not closed: the process exits now, and some plugins crash in effClose when
     * nothing was ever processed, which would only turn a good probe into a failure. */
    _exit(0);
}
