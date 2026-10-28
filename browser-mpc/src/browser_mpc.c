/* MPC Browser: a VST2 "effect" whose page is where the MPC browser shows on the MPC's own screen.
 * Its page is mostly an area painted OVERLAY_MARKER (see ../../mpc-remote/overlay.h): mpc-buttons.so, inside MPC,
 * finds that area on screen and puts the browser's picture over it; touches there go to the browser. The plugin
 * itself only has the page's buttons (back, forward, reload, home, keyboard), passed on to mpc-remote, and lets the audio
 * through untouched. The browser's sound reaches MPC as an input (Web In). */
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "vst2.h"
#include "../../mpc-remote/overlay.h"

enum { kBack, kForward, kReload, kHome, kKeyboard, kNumParams };
static const char *const NAMES[kNumParams] = {"Back", "Forward", "Reload", "Home", "Keyboard"};
static const int CMDS[kNumParams] = {OV_CMD_BACK, OV_CMD_FORWARD, OV_CMD_RELOAD, OV_CMD_HOME, OV_CMD_KEYBOARD};

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    volatile int release[kNumParams];   /* report the momentary buttons back to 0 */
} Plugin;

static struct overlay_shm *ov;

static void ov_map(void) {
    if (ov) return;
    const int fd = open(OVERLAY_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (!ftruncate(fd, sizeof(struct overlay_shm))) {
        void *m = mmap(NULL, sizeof(struct overlay_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) ov = m;
    }
    close(fd);
}

static void copy_str(void *dst, const char *src, size_t max) {
    strncpy((char *)dst, src, max - 1);
    ((char *)dst)[max - 1] = 0;
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams || v <= 0.5f) return;
    if (ov) { ov->cmd = (uint32_t)CMDS[i]; __atomic_add_fetch(&ov->cmd_seq, 1, __ATOMIC_RELEASE); }
    s->release[i] = 1;
}
static float getParameter(AEffect *e, int32_t i) { (void)e; (void)i; return 0; }

static void process(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *s = (Plugin *)e->object;
    if (s->master)
        for (int i = 0; i < kNumParams; i++)
            if (s->release[i]) { s->release[i] = 0; s->master(e, audioMasterAutomate, i, 0, NULL, 0.0f); }
    for (int c = 0; c < 2; c++) {
        const float *src = in && in[c] ? in[c] : NULL;
        for (int k = 0; k < n; k++) {
            float v = src ? src[k] : 0.0f;
            uint32_t u;
            memcpy(&u, &v, 4);
            if ((u & 0x7f800000u) == 0x7f800000u || v > 4.0f || v < -4.0f) v = 0;   /* only clean audio into MPC */
            out[c][k] = v;
        }
    }
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    (void)val; (void)opt;
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effClose: free(s); return 1;
    case effGetParamName: if (idx >= 0 && idx < kNumParams) copy_str(p, NAMES[idx], 32); return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay: copy_str(p, "", 24); return 1;
    case effCanBeAutomated: return 0;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "MPC Browser", 32); return 1;
    case effGetVendorString: copy_str(p, "Other Things", 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effOpen: case effSetSampleRate: case effSetBlockSize: case effMainsChanged: return 1;
    default: return 0;
    }
}

__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    ov_map();
    s->master = master;
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
    e->uniqueID = 0x4d425257;   /* 'MBRW' */
    e->version = 1000;
    return e;
}
