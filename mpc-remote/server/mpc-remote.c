/* mpc-remote: see and touch the MPC's screen from a web browser on the same network.
 *   mpc-remote [port]            (default 8080; open http://<mpc address>:<port>/)
 *
 * Screen: MPC OS draws its UI in software into a plain 32-bit DRM "dumb" buffer (800x1280, the panel is portrait and
 * mounted sideways). We read whichever buffer the display is showing, convert it to 16-bit colour, and send the 32x32
 * tiles that changed, over a WebSocket; the page rotates the picture upright. Nothing is captured while nobody is
 * watching, and the process runs at low priority, so audio isn't disturbed.
 * The display device is only opened while someone is watching, and only once MPC itself has it open: the first
 * program to open it becomes its owner ("DRM master"), and if that isn't MPC, MPC can't show anything. Right after
 * opening, we give up ownership anyway (DROP_MASTER), and we close it again when the last viewer leaves.
 * Touch: the page's clicks and drags are written into the touchscreen's own input device (/dev/input/eventN, the
 * ILI2117), so MPC sees them exactly like a finger. How the touch axes line up with the picture is found once by
 * calibration (three taps on the real screen, while MPC is kept from seeing them) and kept in /data/mpc-remote/.
 *
 * Name: it also answers multicast DNS for <hostname>.local and mpc.local, so the page can be opened by name
 * (http://mpc-live-ii.local:8080/) instead of by the address the router happened to give.
 *
 * Built with -DMPC_REMOTE_FAKE for testing on a PC: the screen comes from a BMP file ($MPC_REMOTE_FAKE_BMP) and
 * touches are printed instead of injected. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sound/asequencer.h>

#include "index_html.h"   /* generated from index.html: static const char INDEX_HTML[] */

#define TILE 32
#if defined(__arm__)
_Static_assert(sizeof(struct input_event) == 16, "32-bit ARM kernels expect 16-byte input events");
#endif
#define MAX_CLIENTS 8
#define FAST_MS 40           /* 25 screen updates a second while you're working it (for 2 s after a touch)... */
#define SLOW_MS 100          /* ...and 10 a second otherwise, to leave the processor to MPC's audio */
#define BUSY_MS 2000
#define MAX_MSG (192 * 1024) /* tiles per WebSocket message, so a slow client gets fresh tiles rather than a backlog */
#define CONF_DIR "/data/mpc-remote"
#define CONF_FILE CONF_DIR "/touch.conf"

/* log: to stderr (the system journal) and the last lines kept for http://<mpc>:8080/log */
#define LOG_LINES 200
static char log_ring[LOG_LINES][200];
static int log_next, log_count;
static struct timespec log_t0;
static char why_waiting[160] = "starting";   /* what the page shows while there's no picture */

static void logf_(const char *fmt, ...) {
    char msg[180];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", msg);
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!log_t0.tv_sec) log_t0 = t;
    snprintf(log_ring[log_next], sizeof log_ring[0], "%7.1fs %s", (double)(t.tv_sec - log_t0.tv_sec) +
             (t.tv_nsec - log_t0.tv_nsec) / 1e9, msg);
    log_next = (log_next + 1) % LOG_LINES;
    if (log_count < LOG_LINES) log_count++;
}

__attribute__((unused)) static void waiting(const char *why) {   /* note why there's no picture, logging only when the reason changes */
    if (strcmp(why, why_waiting)) { snprintf(why_waiting, sizeof why_waiting, "%s", why); logf_("no picture: %s", why); }
}
#define LOG(...) logf_(__VA_ARGS__)

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- the screen ------------------------------------------------------------------------------------------------ */
static int W, H, TW, TH;              /* screen size (portrait, as the panel scans) and in tiles */
static uint16_t *cur, *prev;           /* 16-bit (RGB565) frames */
static uint32_t *rowbuf;               /* one 32-bit row, copied out of the display buffer */

#ifdef MPC_REMOTE_FAKE
static uint32_t *fake_px;
static int screen_open(void) {
    const char *path = getenv("MPC_REMOTE_FAKE_BMP");
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) { LOG("set MPC_REMOTE_FAKE_BMP to a 24-bit BMP"); return -1; }
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54) { fclose(f); return -1; }
    int32_t w, h;
    memcpy(&w, hdr + 18, 4);
    memcpy(&h, hdr + 22, 4);
    W = w; H = h < 0 ? -h : h;
    fake_px = malloc((size_t)W * H * 4);
    const int row = (W * 3 + 3) & ~3;
    uint8_t *line = malloc(row);
    for (int y = 0; y < H; y++) {
        if (fread(line, 1, row, f) != (size_t)row) break;
        const int yy = h > 0 ? H - 1 - y : y;
        for (int x = 0; x < W; x++)
            fake_px[yy * W + x] = (uint32_t)line[x * 3 + 2] << 16 | line[x * 3 + 1] << 8 | line[x * 3];
    }
    free(line);
    fclose(f);
    return 0;
}
static void screen_close(void) { free(fake_px); fake_px = NULL; }
static int screen_grab(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t v = fake_px[y * W + x];
            if (getenv("MPC_REMOTE_FAKE_ANIM") && y < 64 && x < 64) v ^= (uint32_t)(now_ms() / 500 % 2) * 0xffffff;
            cur[y * W + x] = (uint16_t)((v >> 8 & 0xf800) | (v >> 5 & 0x07e0) | (v >> 3 & 0x001f));
        }
    return 0;
}
#else
static int drm_fd = -1;
static uint32_t crtc_id;
static struct { uint32_t fb_id, handle, pitch, format; uint8_t *map; size_t len; } fbm = {0};

static void fb_unmap(void) {
    if (fbm.map) munmap(fbm.map, fbm.len);
    if (fbm.handle) { struct drm_gem_close c = {.handle = fbm.handle}; ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &c); }
    memset(&fbm, 0, sizeof fbm);
}

static int fb_map(uint32_t fb_id) {   /* map the framebuffer the display is showing */
    fb_unmap();
    struct drm_mode_fb_cmd2 f = {.fb_id = fb_id};
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETFB2, &f) || !f.handles[0]) { LOG("GETFB2 %u: %s (handle %u)", fb_id, strerror(errno), f.handles[0]); return -1; }
    for (int i = 1; i < 4; i++)
        if (f.handles[i] && f.handles[i] != f.handles[0]) { struct drm_gem_close c = {.handle = f.handles[i]}; ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &c); }
    fbm.fb_id = fb_id; fbm.handle = f.handles[0]; fbm.pitch = f.pitches[0]; fbm.format = f.pixel_format;
    if (f.pixel_format != DRM_FORMAT_XRGB8888 && f.pixel_format != DRM_FORMAT_ARGB8888) { LOG("unexpected pixel format %08x", f.pixel_format); fb_unmap(); return -1; }
    if ((int)f.width != W || (int)f.height != H) { LOG("screen size changed to %ux%u", f.width, f.height); fb_unmap(); return -1; }
    struct drm_mode_map_dumb md = {.handle = f.handles[0]};
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { LOG("MAP_DUMB: %s", strerror(errno)); fb_unmap(); return -1; }
    fbm.len = (size_t)f.pitches[0] * f.height + f.offsets[0];
    fbm.map = mmap(NULL, fbm.len, PROT_READ, MAP_SHARED, drm_fd, (off_t)md.offset);
    if (fbm.map == MAP_FAILED) { LOG("mmap: %s", strerror(errno)); fbm.map = NULL; fb_unmap(); return -1; }
    return 0;
}

/* the display device (/dev/dri/cardN) that another program (MPC) already has open, found through the open files
 * of every process; -1 if nothing has opened it yet. (Not by MPC's process name: MPC renames itself.) */
static int mpc_display(char *path, size_t n) {
    DIR *proc = opendir("/proc");
    if (!proc) return -1;
    struct dirent *e;
    int found = -1;
    const pid_t self = getpid();
    while (found && (e = readdir(proc))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9' || atoi(e->d_name) == self) continue;
        char p[300];
        snprintf(p, sizeof p, "/proc/%s/fd", e->d_name);
        DIR *fds = opendir(p);
        struct dirent *fe;
        while (fds && found && (fe = readdir(fds))) {
            char l[600], t[128] = {0};
            snprintf(l, sizeof l, "/proc/%s/fd/%s", e->d_name, fe->d_name);
            const ssize_t r = readlink(l, t, sizeof t - 1);
            if (r > 0 && !strncmp(t, "/dev/dri/card", 13)) {
                snprintf(path, n, "%s", t);
                found = 0;
                char comm[64] = {0};
                snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
                FILE *f = fopen(p, "r");
                if (f) { if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0; fclose(f); }
                static char last[300];
                char now[300];
                snprintf(now, sizeof now, "%s is open in process %s (%s)", t, e->d_name, comm);
                if (strcmp(now, last)) { LOG("display: %s", now); snprintf(last, sizeof last, "%s", now); }
            }
        }
        if (fds) closedir(fds);
    }
    closedir(proc);
    return found;
}

static int screen_open(void) {
    char dev[128];
    if (mpc_display(dev, sizeof dev)) { waiting("MPC hasn't opened its display yet"); return -1; }   /* never before MPC */
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) { char m[160]; snprintf(m, sizeof m, "can't open %s: %s", dev, strerror(errno)); waiting(m); return -1; }
    if (ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) == 0) LOG("had become display owner by accident; gave it up");
    struct drm_mode_card_res res = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs) {
        char m[160]; snprintf(m, sizeof m, "%s: no display outputs (%s)", dev, strerror(errno)); waiting(m); close(fd); return -1;
    }
    uint32_t *crtcs = calloc(res.count_crtcs, 4);
    struct drm_mode_card_res r2 = {.crtc_id_ptr = (uintptr_t)crtcs, .count_crtcs = res.count_crtcs};
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2);
    for (uint32_t i = 0; i < res.count_crtcs; i++) {
        struct drm_mode_crtc c = {.crtc_id = crtcs[i]};
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) || !c.mode_valid || !c.fb_id) continue;
        drm_fd = fd; crtc_id = c.crtc_id; W = c.mode.hdisplay; H = c.mode.vdisplay;
        LOG("screen: %s crtc %u, %dx%d", dev, crtc_id, W, H);
        free(crtcs);
        return 0;
    }
    free(crtcs);
    close(fd);
    waiting("the display is on but shows no picture yet");
    return -1;
}

static void screen_close(void) {
    fb_unmap();
    if (drm_fd >= 0) close(drm_fd);
    drm_fd = -1;
}

static int screen_grab(void) {
    struct drm_mode_crtc c = {.crtc_id = crtc_id};
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCRTC, &c) || !c.fb_id) { waiting("can't see which picture is on screen"); return -1; }
    if (c.fb_id != fbm.fb_id && fb_map(c.fb_id)) { waiting("can't read the picture on screen"); return -1; }   /* MPC switched buffers */
    for (int y = 0; y < H; y++) {
        memcpy(rowbuf, fbm.map + (size_t)y * fbm.pitch, (size_t)W * 4);   /* one sequential read of uncached memory */
        uint16_t *d = cur + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            const uint32_t v = rowbuf[x];
            d[x] = (uint16_t)((v >> 8 & 0xf800) | (v >> 5 & 0x07e0) | (v >> 3 & 0x001f));
        }
    }
    return 0;
}
#endif

/* ---- touch ----------------------------------------------------------------------------------------------------- */
/* orientation: how screen position (u across, v down the portrait picture, 0..1) becomes raw touch x/y:
 * bit 0 swap (raw x follows v), bit 1 flip raw x, bit 2 flip raw y */
static int orient = -1;   /* -1: not calibrated yet, use the guess */
static const int GUESS = 0;
static int tmin_x, tmax_x = 2048, tmin_y, tmax_y = 2048;
#ifndef MPC_REMOTE_FAKE
static int touch_fd = -1;     /* writing into the touchscreen's device */
#endif
static char touch_dev[64];
static int tracking = 100, touching = 0;

static int find_touch(void) {
#ifdef MPC_REMOTE_FAKE
    snprintf(touch_dev, sizeof touch_dev, "(fake)");
    return 0;
#else
    for (int n = 0; n < 16; n++) {
        char dev[64];
        snprintf(dev, sizeof dev, "/dev/input/event%d", n);
        int fd = open(dev, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long abs[(ABS_CNT + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        const int mt = (abs[ABS_MT_POSITION_X / (8 * sizeof(long))] >> (ABS_MT_POSITION_X % (8 * sizeof(long)))) & 1;
        if (mt) {
            struct input_absinfo ax = {0}, ay = {0};
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
            tmin_x = ax.minimum; tmax_x = ax.maximum; tmin_y = ay.minimum; tmax_y = ay.maximum;
            char name[128] = {0};
            ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
            close(fd);
            snprintf(touch_dev, sizeof touch_dev, "%s", dev);
            touch_fd = open(dev, O_WRONLY | O_CLOEXEC);
            LOG("touch: %s \"%s\" x %d..%d y %d..%d%s", dev, name, tmin_x, tmax_x, tmin_y, tmax_y,
                touch_fd < 0 ? " (can't open for writing)" : "");
            return touch_fd < 0 ? -1 : 0;
        }
        close(fd);
    }
    LOG("touch: no touchscreen found");
    return -1;
#endif
}

static void to_raw(int px, int py, int *rx, int *ry) {
    double u = W > 1 ? (double)px / (W - 1) : 0, v = H > 1 ? (double)py / (H - 1) : 0;
    const int o = orient < 0 ? GUESS : orient;
    double a = (o & 1) ? v : u, b = (o & 1) ? u : v;
    if (o & 2) a = 1 - a;
    if (o & 4) b = 1 - b;
    a = a < 0 ? 0 : a > 1 ? 1 : a;
    b = b < 0 ? 0 : b > 1 ? 1 : b;
    *rx = tmin_x + (int)(a * (tmax_x - tmin_x) + 0.5);
    *ry = tmin_y + (int)(b * (tmax_y - tmin_y) + 0.5);
}

static void emit(struct input_event *ev, int n) {
#ifdef MPC_REMOTE_FAKE
    for (int i = 0; i < n; i++) printf("inject type %d code %d value %d\n", ev[i].type, ev[i].code, ev[i].value);
    fflush(stdout);
#else
    if (touch_fd >= 0 && write(touch_fd, ev, sizeof *ev * n) < 0) LOG("touch write failed: %s", strerror(errno));
#endif
}

/* ---- a keyboard ------------------------------------------------------------------------------------------------
 * The page's typing becomes key presses on a virtual USB keyboard (uinput), which MPC (libinput + xkb) uses like a
 * real one: names of tracks, programs, projects. Made the first time the page types, and kept from then on. */
static int kbd_fd = -1;
static uint8_t kbd_down[KEY_F24 + 1];

static int kbd_open(void) {
    if (kbd_fd >= 0) return 0;
#ifdef MPC_REMOTE_FAKE
    kbd_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    return kbd_fd < 0 ? -1 : 0;
#else
    const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { LOG("keyboard: can't open /dev/uinput: %s", strerror(errno)); return -1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    for (int k = KEY_ESC; k <= KEY_F24; k++) ioctl(fd, UI_SET_KEYBIT, k);   /* a full PC keyboard */
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1d6b;   /* "Linux Foundation": a virtual device */
    us.id.product = 0x0104;
    us.id.version = 1;
    snprintf(us.name, sizeof us.name, "MPC Remote Keyboard");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        LOG("keyboard: can't create it: %s", strerror(errno));
        close(fd);
        return -1;
    }
    kbd_fd = fd;
    LOG("keyboard: MPC Remote Keyboard created");
    return 0;
#endif
}

static void kbd_key(int code, int down) {
    if (code < 1 || code > KEY_F24 || kbd_open()) return;
    if (!down && !kbd_down[code]) return;
    kbd_down[code] = (uint8_t)(down != 0);
    struct input_event ev[2];
    memset(ev, 0, sizeof ev);
    ev[0].type = EV_KEY; ev[0].code = (uint16_t)code; ev[0].value = down != 0;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
#ifdef MPC_REMOTE_FAKE
    printf("key %d %s\n", code, down ? "down" : "up");
    fflush(stdout);
#else
    if (write(kbd_fd, ev, sizeof ev) < 0) LOG("keyboard: write failed: %s", strerror(errno));
#endif
}

static void kbd_release_all(void) {   /* nothing left held when the page goes away */
    for (int k = 1; k <= KEY_F24; k++) if (kbd_down[k]) kbd_key(k, 0);
}

static void touch(char kind, int px, int py) {   /* 'd'own, 'm'ove, 'u'p, in portrait screen pixels */
    struct input_event ev[8];
    int n = 0, rx = 0, ry = 0;
#define EV(t, c, v) (memset(&ev[n], 0, sizeof ev[n]), ev[n].type = (t), ev[n].code = (c), ev[n].value = (v), n++)
    if (kind == 'u') {
        if (!touching) return;
        touching = 0;
        EV(EV_ABS, ABS_MT_SLOT, 0);
        EV(EV_ABS, ABS_MT_TRACKING_ID, -1);
        EV(EV_KEY, BTN_TOUCH, 0);
        EV(EV_SYN, SYN_REPORT, 0);
    } else {
        if (kind == 'm' && !touching) return;
        to_raw(px, py, &rx, &ry);
        EV(EV_ABS, ABS_MT_SLOT, 0);
        if (kind == 'd') {
            if (touching) touch('u', 0, 0), n = 0, EV(EV_ABS, ABS_MT_SLOT, 0);
            touching = 1;
            EV(EV_ABS, ABS_MT_TRACKING_ID, tracking = tracking % 60000 + 1);
        }
        EV(EV_ABS, ABS_MT_POSITION_X, rx);
        EV(EV_ABS, ABS_MT_POSITION_Y, ry);
        if (kind == 'd') EV(EV_KEY, BTN_TOUCH, 1);
        EV(EV_ABS, ABS_X, rx);
        EV(EV_ABS, ABS_Y, ry);
        EV(EV_SYN, SYN_REPORT, 0);
    }
#undef EV
    emit(ev, n);
}

static void load_conf(void) {
    FILE *f = fopen(CONF_FILE, "r");
    if (!f) return;
    int o;
    if (fscanf(f, "orient=%d", &o) == 1 && o >= 0 && o < 8) orient = o;
    fclose(f);
    LOG("touch orientation %d (from %s)", orient, CONF_FILE);
}

static void save_conf(void) {
    mkdir(CONF_DIR, 0755);
    FILE *f = fopen(CONF_FILE ".new", "w");
    if (!f) { LOG("can't save %s: %s", CONF_FILE, strerror(errno)); return; }
    fprintf(f, "orient=%d\n", orient);
    fclose(f);
    rename(CONF_FILE ".new", CONF_FILE);
    sync();
}

/* calibration: while MPC is kept from seeing the touchscreen, read three taps on the real screen:
 * top-left, top-right, bottom-left of the screen as it's seen (landscape) */
static struct { int fd, step, x, y, down; uint64_t started; int rx[3], ry[3]; } cal = {.fd = -1};

static void cal_stop(void) {
    if (cal.fd >= 0) { ioctl(cal.fd, EVIOCGRAB, 0); close(cal.fd); }
    cal.fd = -1;
    cal.step = 0;
}

static int cal_start(void) {
#ifdef MPC_REMOTE_FAKE
    cal.fd = open("/dev/null", O_RDONLY);
#else
    cal_stop();
    cal.fd = open(touch_dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (cal.fd < 0) return -1;
    if (ioctl(cal.fd, EVIOCGRAB, 1)) { close(cal.fd); cal.fd = -1; return -1; }   /* MPC won't see these taps */
#endif
    cal.step = 1; cal.down = 0; cal.started = now_ms();
    return 0;
}

static int cal_finish(void) {   /* -> orientation, or -1 if the taps don't make sense */
    /* landscape TL is portrait (u=0, v=1); TR is (u=0, v=0); BL is (u=1, v=1) */
    const int dvx = cal.rx[1] - cal.rx[0], dvy = cal.ry[1] - cal.ry[0];   /* moving along v (TL -> TR: v 1 -> 0) */
    const int dux = cal.rx[2] - cal.rx[0], duy = cal.ry[2] - cal.ry[0];   /* moving along u (TL -> BL: u 0 -> 1) */
    const int span = (tmax_x - tmin_x + tmax_y - tmin_y) / 2;
    int o;
    if (abs(dux) > abs(duy) && abs(dvy) > abs(dvx)) {          /* raw x follows u, raw y follows v */
        o = 0;
        if (dux < 0) o |= 2;
        if (dvy > 0) o |= 4;   /* v went down while raw y went up: flipped */
    } else if (abs(duy) > abs(dux) && abs(dvx) > abs(dvy)) {   /* raw x follows v, raw y follows u */
        o = 1;
        if (dvx > 0) o |= 2;
        if (duy < 0) o |= 4;
    } else return -1;
    if (abs(dux) + abs(duy) < span / 4 || abs(dvx) + abs(dvy) < span / 4) return -1;   /* taps too close together */
    return o;
}

/* ---- clients: HTTP + WebSocket --------------------------------------------------------------------------------- */
typedef struct {
    int fd, ws;
    char in[8192];
    size_t inlen;
    uint8_t *out;
    size_t outlen, outcap, outpos;
    uint8_t *dirty;   /* tiles this client hasn't been sent since they changed */
    int ndirty;
} Client;
static Client clients[MAX_CLIENTS];

static void out_add(Client *c, const void *p, size_t n) {
    if (c->outlen + n > c->outcap) {
        size_t cap = c->outcap ? c->outcap : 65536;
        while (cap < c->outlen + n) cap *= 2;
        c->out = realloc(c->out, cap);
        c->outcap = cap;
    }
    memcpy(c->out + c->outlen, p, n);
    c->outlen += n;
}

static void ws_frame(Client *c, int opcode, const void *p, size_t n) {
    uint8_t h[10];
    size_t hl;
    h[0] = (uint8_t)(0x80 | opcode);
    if (n < 126) { h[1] = (uint8_t)n; hl = 2; }
    else if (n < 65536) { h[1] = 126; h[2] = (uint8_t)(n >> 8); h[3] = (uint8_t)n; hl = 4; }
    else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)n >> (56 - 8 * i)); hl = 10; }
    out_add(c, h, hl);
    out_add(c, p, n);
}

static void ws_text(Client *c, const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    ws_frame(c, 1, b, (size_t)n);
}

static void broadcast_text(const char *msg) {
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].ws) ws_frame(&clients[i], 1, msg, strlen(msg));
}

static void cal_tap(int x, int y) {   /* a finished tap on the real screen during calibration */
    if (cal.fd < 0 || cal.step < 1 || cal.step > 3) return;
    cal.rx[cal.step - 1] = x;
    cal.ry[cal.step - 1] = y;
    LOG("calibration tap %d: raw %d,%d", cal.step, x, y);
    char m[96];
    if (++cal.step <= 3) { snprintf(m, sizeof m, "{\"type\":\"cal\",\"step\":%d}", cal.step); broadcast_text(m); return; }
    const int o = cal_finish();
    cal_stop();
    if (o < 0) { broadcast_text("{\"type\":\"cal\",\"error\":\"those taps didn't line up; try again\"}"); return; }
    orient = o;
    save_conf();
    snprintf(m, sizeof m, "{\"type\":\"cal\",\"done\":true,\"orient\":%d}", o);
    broadcast_text(m);
    LOG("touch orientation %d saved", o);
}

/* ---- MPC's buttons ----------------------------------------------------------------------------------------------
 * MPC reads its panel as MIDI from its internal controller. mpc-buttons.so (preloaded into MPC by mpc-launch) feeds
 * that input through the ALSA sequencer and says where in /tmp/mpc-buttons; a press here is the same MIDI message the
 * real button sends, sent to that input. Which message each button sends is learned once (press the real button
 * when asked) and kept in /data/mpc-remote/buttons.conf. */
#define BTN_FILE CONF_DIR "/buttons.conf"
#define BTN_STATUS "/tmp/mpc-buttons"
#define NO_BUTTONS CONF_DIR "/no-buttons"
#define LAUNCHES CONF_DIR "/launches"
static const char *const BTN_NAMES[] = {"menu", "main", "mix", "mute", "rec", "overdub", "stop", "play", "playstart",
                                       "shift", "undo"};
#define NBTN (int)(sizeof BTN_NAMES / sizeof *BTN_NAMES)
static struct { int set; uint8_t on[3], off[3]; } btn[NBTN];
static int seq_fd = -1, seq_me = -1, seq_port = -1;
static struct { int idx, have_on, subscribed; uint64_t started, on_at; struct snd_seq_addr hw; } learn = {.idx = -1};

static int btn_index(const char *name) {
    for (int i = 0; i < NBTN; i++) if (!strcmp(name, BTN_NAMES[i])) return i;
    return -1;
}

static void btn_load(void) {
    FILE *f = fopen(BTN_FILE, "r");
    if (!f) return;
    char name[32];
    unsigned a, b, c, d, e, g;
    int n = 0;
    while (fscanf(f, "%31s %x %x %x %x %x %x", name, &a, &b, &c, &d, &e, &g) == 7) {
        const int i = btn_index(name);
        if (i < 0) continue;
        btn[i].set = 1;
        btn[i].on[0] = (uint8_t)a; btn[i].on[1] = (uint8_t)b; btn[i].on[2] = (uint8_t)c;
        btn[i].off[0] = (uint8_t)d; btn[i].off[1] = (uint8_t)e; btn[i].off[2] = (uint8_t)g;
        n++;
    }
    fclose(f);
    LOG("%d buttons learned (from %s)", n, BTN_FILE);
}

static void btn_save(void) {
    mkdir(CONF_DIR, 0755);
    FILE *f = fopen(BTN_FILE ".new", "w");
    if (!f) { LOG("can't save %s: %s", BTN_FILE, strerror(errno)); return; }
    for (int i = 0; i < NBTN; i++)
        if (btn[i].set)
            fprintf(f, "%s %02x %02x %02x %02x %02x %02x\n", BTN_NAMES[i], btn[i].on[0], btn[i].on[1], btn[i].on[2],
                    btn[i].off[0], btn[i].off[1], btn[i].off[2]);
    fclose(f);
    rename(BTN_FILE ".new", BTN_FILE);
    sync();
}

/* where MPC's panel input is now (it changes when MPC restarts): virt, and the real controller's port hw */
static int btn_where(struct snd_seq_addr *virt, struct snd_seq_addr *hw, int *pid) {
    FILE *f = fopen(BTN_STATUS, "r");
    if (!f) return -1;
    int vc, vp, hc, hp, p;
    const int n = fscanf(f, "virt=%d:%d hw=%d:%d pid=%d", &vc, &vp, &hc, &hp, &p);
    fclose(f);
    if (n != 5) return -1;
    char d[32];
    snprintf(d, sizeof d, "/proc/%d", p);
    if (access(d, F_OK)) return -1;   /* that MPC has gone */
    virt->client = (unsigned char)vc; virt->port = (unsigned char)vp;
    hw->client = (unsigned char)hc; hw->port = (unsigned char)hp;
    if (pid) *pid = p;
    return 0;
}

static void seq_open_(void) {
    if (seq_fd >= 0) return;
    seq_fd = open("/dev/snd/seq", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (seq_fd < 0) { LOG("buttons: can't open the ALSA sequencer: %s", strerror(errno)); return; }
    struct snd_seq_client_info ci;
    memset(&ci, 0, sizeof ci);
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_CLIENT_ID, &seq_me) < 0) goto fail;
    ci.client = seq_me;
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_GET_CLIENT_INFO, &ci) < 0) goto fail;
    snprintf(ci.name, sizeof ci.name, "MPC Remote");   /* mpc-buttons.so hides this name from MPC's MIDI devices */
    ioctl(seq_fd, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &ci);
    struct snd_seq_port_info pi;
    memset(&pi, 0, sizeof pi);
    pi.addr.client = (unsigned char)seq_me;
    snprintf(pi.name, sizeof pi.name, "MPC Remote");
    pi.capability = SNDRV_SEQ_PORT_CAP_READ | SNDRV_SEQ_PORT_CAP_WRITE | SNDRV_SEQ_PORT_CAP_SUBS_WRITE |
                    SNDRV_SEQ_PORT_CAP_NO_EXPORT;
    pi.type = SNDRV_SEQ_PORT_TYPE_APPLICATION;
    if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_CREATE_PORT, &pi) < 0) goto fail;
    seq_port = pi.addr.port;
    LOG("buttons: sequencer client %d:%d", seq_me, seq_port);
    return;
fail:
    LOG("buttons: sequencer setup failed: %s", strerror(errno));
    close(seq_fd);
    seq_fd = -1;
}

static int seq_send(const uint8_t m[3], struct snd_seq_addr dest) {
    struct snd_seq_event ev;
    memset(&ev, 0, sizeof ev);
    const int st = m[0] & 0xf0;
    if (st == 0x90) { ev.type = SNDRV_SEQ_EVENT_NOTEON; ev.data.note.note = m[1]; ev.data.note.velocity = m[2]; }
    else if (st == 0x80) { ev.type = SNDRV_SEQ_EVENT_NOTEOFF; ev.data.note.note = m[1]; ev.data.note.velocity = m[2]; }
    else if (st == 0xb0) { ev.type = SNDRV_SEQ_EVENT_CONTROLLER; ev.data.control.param = m[1]; ev.data.control.value = m[2]; }
    else return -1;
    ev.data.note.channel = m[0] & 15;   /* the same place in note and control events */
    ev.data.control.channel = m[0] & 15;
    ev.flags = SNDRV_SEQ_TIME_STAMP_TICK | SNDRV_SEQ_TIME_MODE_ABS | SNDRV_SEQ_EVENT_LENGTH_FIXED;
    ev.queue = SNDRV_SEQ_QUEUE_DIRECT;
    ev.source.client = (unsigned char)seq_me;
    ev.source.port = (unsigned char)seq_port;
    ev.dest = dest;
    return write(seq_fd, &ev, sizeof ev) == (ssize_t)sizeof ev ? 0 : -1;
}

static void btn_press(const char *name, int down) {
    const int i = btn_index(name);
    if (i < 0) return;
    if (!btn[i].set) { broadcast_text("{\"type\":\"btn\",\"error\":\"that button isn't learned yet (Learn buttons)\"}"); return; }
    struct snd_seq_addr virt, hw;
    seq_open_();
    if (seq_fd < 0 || btn_where(&virt, &hw, NULL)) {
        broadcast_text("{\"type\":\"btn\",\"error\":\"MPC's button input isn't available (details: /log)\"}");
        return;
    }
    if (seq_send(down ? btn[i].on : btn[i].off, virt)) LOG("buttons: sending %s failed: %s", name, strerror(errno));
}

static void learn_subscribe(int on) {
    if (learn.subscribed == on || seq_fd < 0) return;
    struct snd_seq_port_subscribe s;
    memset(&s, 0, sizeof s);
    s.sender = learn.hw;
    s.dest.client = (unsigned char)seq_me;
    s.dest.port = (unsigned char)seq_port;
    if (ioctl(seq_fd, on ? SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT : SNDRV_SEQ_IOCTL_UNSUBSCRIBE_PORT, &s) < 0 && on) {
        LOG("buttons: can't listen to the controller %d:%d: %s", s.sender.client, s.sender.port, strerror(errno));
        return;
    }
    learn.subscribed = on;
}

static void learn_stop(void) { learn_subscribe(0); learn.idx = -1; }

static void learn_start(const char *name) {
    const int i = btn_index(name);
    if (i < 0) return;
    struct snd_seq_addr virt;
    seq_open_();
    learn_stop();
    if (seq_fd < 0 || btn_where(&virt, &learn.hw, NULL)) {
        broadcast_text("{\"type\":\"learn\",\"error\":\"MPC's button input isn't available (details: /log)\"}");
        return;
    }
    learn_subscribe(1);
    if (!learn.subscribed) { broadcast_text("{\"type\":\"learn\",\"error\":\"can't listen to the MPC's buttons\"}"); return; }
    learn.idx = i; learn.have_on = 0; learn.started = now_ms();
    char m[96];
    snprintf(m, sizeof m, "{\"type\":\"learn\",\"name\":\"%s\"}", BTN_NAMES[i]);
    broadcast_text(m);
}

static void learn_done(void) {
    const int i = learn.idx;
    btn[i].set = 1;
    btn_save();
    LOG("button %s: on %02x %02x %02x, off %02x %02x %02x", BTN_NAMES[i], btn[i].on[0], btn[i].on[1], btn[i].on[2],
        btn[i].off[0], btn[i].off[1], btn[i].off[2]);
    char m[96];
    snprintf(m, sizeof m, "{\"type\":\"learn\",\"done\":\"%s\"}", BTN_NAMES[i]);
    learn_stop();
    broadcast_text(m);
}

static void seq_read(void) {   /* the controller's messages, while learning */
    uint8_t buf[4096];
    const ssize_t r = read(seq_fd, buf, sizeof buf);
    for (ssize_t p = 0; r > 0 && p + (ssize_t)sizeof(struct snd_seq_event) <= r;) {
        struct snd_seq_event ev;
        memcpy(&ev, buf + p, sizeof ev);
        p += (ssize_t)sizeof ev;
        if ((ev.flags & SNDRV_SEQ_EVENT_LENGTH_MASK) == SNDRV_SEQ_EVENT_LENGTH_VARIABLE) p += ev.data.ext.len;
        if (learn.idx < 0 || ev.source.client != learn.hw.client || ev.source.port != learn.hw.port) continue;
        uint8_t m[3];
        int press, key;
        if (ev.type == SNDRV_SEQ_EVENT_NOTEON || ev.type == SNDRV_SEQ_EVENT_NOTEOFF) {
            m[0] = (uint8_t)((ev.type == SNDRV_SEQ_EVENT_NOTEON ? 0x90 : 0x80) | (ev.data.note.channel & 15));
            m[1] = ev.data.note.note & 127; m[2] = ev.data.note.velocity & 127;
            press = ev.type == SNDRV_SEQ_EVENT_NOTEON && m[2];
            key = 0x100 | m[1];
        } else if (ev.type == SNDRV_SEQ_EVENT_CONTROLLER) {
            m[0] = (uint8_t)(0xb0 | (ev.data.control.channel & 15));
            m[1] = (uint8_t)(ev.data.control.param & 127); m[2] = (uint8_t)(ev.data.control.value & 127);
            press = m[2] != 0;
            key = 0x200 | m[1];
        } else continue;
        LOG("learn: controller sent %02x %02x %02x", m[0], m[1], m[2]);
        const int i = learn.idx;
        if (!learn.have_on) {
            if (!press) continue;
            memcpy(btn[i].on, m, 3);
            learn.have_on = key;
            learn.on_at = now_ms();
        } else if (!press && key == learn.have_on) {
            memcpy(btn[i].off, m, 3);
            learn_done();
            return;
        }
    }
}

static void learn_tick(void) {
    if (learn.idx < 0) return;
    const uint64_t t = now_ms();
    if (learn.have_on && t - learn.on_at > 3000) {   /* no release message: a press is a toggle; send it as is */
        const int i = learn.idx;
        memcpy(btn[i].off, btn[i].on, 3);
        btn[i].off[2] = (btn[i].on[0] & 0xf0) == 0x90 ? 0 : btn[i].on[2];
        if ((btn[i].on[0] & 0xf0) == 0xb0) btn[i].off[2] = 0;
        learn_done();
    } else if (!learn.have_on && t - learn.started > 30000) {
        learn_stop();
        broadcast_text("{\"type\":\"learn\",\"error\":\"timed out\"}");
    }
}

/* MPC has run a minute with mpc-buttons.so: it works, so mpc-launch may keep loading it */
static void launch_ok(void) {
    static int done_pid;
    struct snd_seq_addr v, h;
    int pid;
    struct stat st;
    if (btn_where(&v, &h, &pid) || pid == done_pid || stat(BTN_STATUS, &st) || time(NULL) - st.st_mtime < 60) return;
    done_pid = pid;
    FILE *f = fopen(LAUNCHES, "w");
    if (f) { fputs("0\n", f); fclose(f); sync(); }
    LOG("buttons: MPC (pid %d) is up with the button input; panel input via %d:%d", pid, v.client, v.port);
}

static void client_close(Client *c) {
    if (c->fd >= 0) close(c->fd);
    free(c->out);
    free(c->dirty);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

/* SHA-1 and base64, for the WebSocket handshake */
static void sha1(const uint8_t *msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t *m = calloc(1, total);
    memcpy(m, msg, len);
    m[len] = 0x80;
    const uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m[total - 1 - i] = (uint8_t)(bits >> (8 * i));
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[off + 4 * i] << 24 | m[off + 4 * i + 1] << 16 | m[off + 4 * i + 2] << 8 | m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
            else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
            const uint32_t t = ROL(a, 5) + f + e + k + w[i];
            e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += cc; h[3] += d; h[4] += e;
    }
#undef ROL
    free(m);
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

static void base64(const uint8_t *in, size_t n, char *out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}

static const char *header(const char *req, const char *name) {   /* value of a request header, or NULL */
    const size_t n = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p && p[2]; p = strstr(p + 2, "\r\n")) {
        if (!strncasecmp(p + 2, name, n) && p[2 + n] == ':') {
            const char *v = p + 3 + n;
            while (*v == ' ') v++;
            return v;
        }
    }
    return NULL;
}

static void http_reply(Client *c, const char *status, const char *type, const char *body, size_t n) {
    char h[256];
    const int hl = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                            "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, n);
    out_add(c, h, (size_t)hl);
    out_add(c, body, n);
}

static int screen_ready = 0, have_prev = 0;

static void send_hello(Client *c) {   /* screen size, and the whole screen to come */
    free(c->dirty);
    c->dirty = malloc((size_t)TW * TH);
    memset(c->dirty, 1, (size_t)TW * TH);
    c->ndirty = TW * TH;
    char learned[256] = "";
    for (int i = 0; i < NBTN; i++)
        if (btn[i].set) snprintf(learned + strlen(learned), sizeof learned - strlen(learned), "%s\"%s\"", learned[0] ? "," : "", BTN_NAMES[i]);
    ws_text(c, "{\"type\":\"hello\",\"w\":%d,\"h\":%d,\"tile\":%d,\"calibrated\":%s,\"touch\":\"%s\",\"learned\":[%s],"
            "\"buttons\":%s}", W, H, TILE, orient >= 0 ? "true" : "false", touch_dev, learned,
            access(NO_BUTTONS, F_OK) ? "true" : "false");
}

static void handle_http(Client *c) {   /* a complete request is in c->in */
    if (!strncmp(c->in, "GET /ws ", 8) && header(c->in, "Sec-WebSocket-Key")) {
        const char *k = header(c->in, "Sec-WebSocket-Key");
        char key[128];
        size_t kl = strcspn(k, "\r\n");
        if (kl > 60) kl = 60;
        snprintf(key, sizeof key, "%.*s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", (int)kl, k);
        uint8_t dg[20];
        char acc[40], h[256];
        sha1((const uint8_t *)key, strlen(key), dg);
        base64(dg, 20, acc);
        const int hl = snprintf(h, sizeof h, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
        out_add(c, h, (size_t)hl);
        c->ws = 1;
        if (screen_ready) send_hello(c);
        else ws_text(c, "{\"type\":\"wait\",\"why\":\"%s\"}", why_waiting);
        LOG("viewer connected");
    } else if (!strncmp(c->in, "GET /log ", 9)) {   /* what the remote has been doing, for fixing problems */
        static char body[LOG_LINES * 202 + 8192];
        size_t n = (size_t)snprintf(body, 512, "mpc-remote log (newest last)\npicture: %s\ntouch: %s, orientation %d%s\n\n",
                                    screen_ready ? "ok" : why_waiting, touch_dev[0] ? touch_dev : "none",
                                    orient < 0 ? GUESS : orient, orient < 0 ? " (not calibrated)" : "");
        struct snd_seq_addr v, h;
        char b[200] = "";
        if (!access(NO_BUTTONS, F_OK)) snprintf(b, sizeof b, "off (switched off on the page)");
        else if (!btn_where(&v, &h, NULL)) snprintf(b, sizeof b, "MPC's panel input at %d:%d (controller %d:%d)", v.client, v.port, h.client, h.port);
        else snprintf(b, sizeof b, "not available: MPC wasn't started with mpc-buttons.so (see below)");
        n += (size_t)snprintf(body + n, 300, "buttons: %s\n", b);
        FILE *pl = fopen("/tmp/mpc-buttons.log", "r");   /* what mpc-buttons.so did inside MPC */
        if (pl) {
            char line[200];
            n += (size_t)snprintf(body + n, 64, "\nmpc-buttons.so (inside MPC):\n");
            while (fgets(line, sizeof line, pl) && n < 4096) n += (size_t)snprintf(body + n, 202, "  %s", line);
            fclose(pl);
        }
        n += (size_t)snprintf(body + n, 8, "\n");
        for (int i = 0; i < log_count; i++)
            n += (size_t)snprintf(body + n, 202, "%s\n", log_ring[(log_next - log_count + i + LOG_LINES) % LOG_LINES]);
        http_reply(c, "200 OK", "text/plain; charset=utf-8", body, n);
    } else if (!strncmp(c->in, "GET / ", 6) || !strncmp(c->in, "GET /index.html ", 16)) {
        http_reply(c, "200 OK", "text/html; charset=utf-8", INDEX_HTML, sizeof INDEX_HTML - 1);
    } else {
        http_reply(c, "404 Not Found", "text/plain", "not found\n", 10);
    }
    c->inlen = 0;
}

static uint64_t touched_at;   /* when the page last sent a touch */

static void handle_command(Client *c, char *s) {   /* a text message from the page */
    int x, y;
    if (sscanf(s, "d %d %d", &x, &y) == 2) { if (cal.fd < 0) touch('d', x, y); touched_at = now_ms(); }
    else if (sscanf(s, "m %d %d", &x, &y) == 2) { if (cal.fd < 0) touch('m', x, y); touched_at = now_ms(); }
    else if (s[0] == 'u') { if (cal.fd < 0) touch('u', 0, 0); touched_at = now_ms(); }
    else if (!strcmp(s, "cal")) {
        if (touching) touch('u', 0, 0);
        if (cal_start()) broadcast_text("{\"type\":\"cal\",\"error\":\"can't read the touchscreen\"}");
        else broadcast_text("{\"type\":\"cal\",\"step\":1}");
    } else if (!strcmp(s, "calcancel")) {
        cal_stop();
        broadcast_text("{\"type\":\"cal\",\"cancelled\":true}");
    } else if (sscanf(s, "kd %d", &x) == 1) kbd_key(x, 1);
    else if (sscanf(s, "ku %d", &x) == 1) kbd_key(x, 0);
    else if (!strncmp(s, "bd ", 3)) btn_press(s + 3, 1);
    else if (!strncmp(s, "bu ", 3)) btn_press(s + 3, 0);
    else if (!strncmp(s, "learn ", 6)) learn_start(s + 6);
    else if (!strcmp(s, "learncancel")) learn_stop();
    else if (!strcmp(s, "buttons off") || !strcmp(s, "buttons on")) {   /* takes effect when MPC next starts */
        mkdir(CONF_DIR, 0755);
        if (s[9] == 'f') { FILE *f = fopen(NO_BUTTONS, "w"); if (f) fclose(f); }
        else { unlink(NO_BUTTONS); unlink(LAUNCHES); }
        sync();
        LOG("buttons %s from the next MPC start", s[9] == 'f' ? "off" : "on");
        if (screen_ready) send_hello(c);
    }
#ifdef MPC_REMOTE_FAKE
    else if (sscanf(s, "fakecal %d %d", &x, &y) == 2) cal_tap(x, y);   /* test hook: a raw tap on the "real" screen */
#endif
    (void)c;
}

static void handle_ws(Client *c) {   /* parse complete frames from c->in */
    size_t pos = 0;
    while (c->inlen - pos >= 2) {
        const uint8_t *p = (uint8_t *)c->in + pos;
        const int op = p[0] & 15, masked = p[1] & 0x80;
        uint64_t n = p[1] & 127;
        size_t hl = 2;
        if (n == 126) { if (c->inlen - pos < 4) break; n = (uint64_t)p[2] << 8 | p[3]; hl = 4; }
        else if (n == 127) { client_close(c); return; }   /* the page never sends big messages */
        if (masked) hl += 4;
        if (c->inlen - pos < hl + n) break;
        char msg[1024];
        if (n >= sizeof msg) { client_close(c); return; }
        for (uint64_t i = 0; i < n; i++) msg[i] = (char)(p[hl + i] ^ (masked ? p[hl - 4 + (i & 3)] : 0));
        msg[n] = 0;
        if (op == 8) { ws_frame(c, 8, "", 0); c->ws = 2; }   /* close: reply, then close once sent */
        else if (op == 9) ws_frame(c, 10, msg, n);
        else if (op == 1) handle_command(c, msg);
        pos += hl + n;
    }
    memmove(c->in, c->in + pos, c->inlen - pos);
    c->inlen -= pos;
}

/* tiles: raw RGB565, or runs of one colour, whichever is smaller.
 * record: [tile x u16][tile y u16][mode u8: 0 raw, 1 runs][0 u8][0 u16][data length u32][data]
 * runs are [count u16][colour u16] pairs, in reading order; all little-endian */
static size_t encode_tile(int tx, int ty, uint8_t *o) {
    const int x0 = tx * TILE, y0 = ty * TILE, w = W - x0 < TILE ? W - x0 : TILE, h = H - y0 < TILE ? H - y0 : TILE;
    uint8_t *d = o + 12;
    size_t runs = 0;
    uint16_t col = cur[(size_t)y0 * W + x0];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint16_t v = cur[(size_t)(y0 + y) * W + x0 + x];
            if (v != col) { runs++; col = v; }
        }
    runs++;
    const size_t raw = (size_t)w * h * 2;
    uint32_t len = 0;
    if (runs * 4 < raw) {
        uint16_t run = 0;
        col = cur[(size_t)y0 * W + x0];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint16_t v = cur[(size_t)(y0 + y) * W + x0 + x];
                if (v == col) { run++; continue; }
                memcpy(d + len, &run, 2); memcpy(d + len + 2, &col, 2); len += 4;
                col = v; run = 1;
            }
        memcpy(d + len, &run, 2); memcpy(d + len + 2, &col, 2); len += 4;
        o[4] = 1;
    } else {
        for (int y = 0; y < h; y++) { memcpy(d + len, cur + (size_t)(y0 + y) * W + x0, (size_t)w * 2); len += (uint32_t)w * 2; }
        o[4] = 0;
    }
    const uint16_t txx = (uint16_t)tx, tyy = (uint16_t)ty;
    memcpy(o, &txx, 2);
    memcpy(o + 2, &tyy, 2);
    o[5] = o[6] = o[7] = 0;
    memcpy(o + 8, &len, 4);
    return 12 + len;
}

/* zlib, from the MPC's own libz.so.1 (loaded at run time, so nothing is needed at build time); without it,
 * messages go uncompressed */
static int (*z_compress2)(uint8_t *, unsigned long *, const uint8_t *, unsigned long, int);
static unsigned long (*z_bound)(unsigned long);
static void zlib_load(void) {
    void *z = dlopen("libz.so.1", RTLD_NOW);
    if (z) { z_compress2 = (int (*)(uint8_t *, unsigned long *, const uint8_t *, unsigned long, int))dlsym(z, "compress2");
             z_bound = (unsigned long (*)(unsigned long))dlsym(z, "compressBound"); }
    if (!z_compress2 || !z_bound) z_compress2 = NULL;
    LOG("compression: %s", z_compress2 ? "zlib" : "none (no libz.so.1)");
}

static struct { unsigned frames, changed; double grab_ms, send_ms; unsigned long raw, sent; uint64_t since; } st;

/* a message: [format u8: 0 plain, 1 zlib][payload], payload = [tile count u32][tile records] */
static void send_tiles(Client *c) {   /* as many of this client's changed tiles as fit in one message */
    static uint8_t *buf, *zbuf;
    static unsigned long zcap;
    if (!buf) buf = malloc(MAX_MSG + 8 + 4 + TILE * TILE * 4 + 16);
    const uint64_t t0 = now_ms();
    size_t n = 4;
    uint32_t count = 0;
    for (int t = 0; t < TW * TH && n < MAX_MSG; t++) {
        if (!c->dirty[t]) continue;
        n += encode_tile(t % TW, t / TW, buf + n);
        c->dirty[t] = 0;
        c->ndirty--;
        count++;
    }
    if (!count) return;
    memcpy(buf, &count, 4);
    unsigned long zn = 0;
    if (z_compress2) {
        const unsigned long need = z_bound(n) + 1;
        if (need > zcap) { free(zbuf); zbuf = malloc(need); zcap = need; }
        zn = zcap - 1;
        if (z_compress2(zbuf + 1, &zn, buf, n, 1) != 0 || zn >= n) zn = 0;   /* level 1: fast */
    }
    uint8_t fmt = zn ? 1 : 0;
    if (zn) { zbuf[0] = fmt; ws_frame(c, 2, zbuf, zn + 1); }
    else { static uint8_t *pbuf; static size_t pcap; if (n + 1 > pcap) { free(pbuf); pbuf = malloc(n + 1); pcap = n + 1; }
           pbuf[0] = 0; memcpy(pbuf + 1, buf, n); ws_frame(c, 2, pbuf, n + 1); }
    st.raw += n; st.sent += (zn ? zn : n) + 1;
    st.send_ms += (double)(now_ms() - t0);
}

/* ---- multicast DNS: answer "<hostname>.local" and "mpc.local" with this MPC's address ---------------------------- */
static int mdns_fd = -1;
static char mdns_names[2][64];   /* "mpc-live-ii", "mpc" (without .local) */

static void mdns_join(void) {   /* join the mDNS group on every IPv4 interface (Wi-Fi may come up after us) */
    struct ifaddrs *ifs, *i;
    if (mdns_fd < 0 || getifaddrs(&ifs)) return;
    for (i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        struct ip_mreq m = {.imr_multiaddr.s_addr = inet_addr("224.0.0.251"),
                            .imr_interface = ((struct sockaddr_in *)i->ifa_addr)->sin_addr};
        if (setsockopt(mdns_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0)
            LOG("name: answering on %s (%s)", i->ifa_name, inet_ntoa(m.imr_interface));
    }
    freeifaddrs(ifs);
}

static void mdns_open(void) {
    char host[64] = {0};
    gethostname(host, sizeof host - 1);
    for (char *c = host; *c; c++) if (*c == '.') { *c = 0; break; }
    snprintf(mdns_names[0], sizeof mdns_names[0], "%s", host[0] ? host : "mpc");
    snprintf(mdns_names[1], sizeof mdns_names[1], "mpc");
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0), one = 1;
    unsigned char ttl = 255, loop = 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof one);   /* to learn which of our addresses was asked */
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(5353), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a)) {
        LOG("name: can't use port 5353 (%s); open the page by address", strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    mdns_fd = fd;
    LOG("name: http://%s.local and http://%s.local", mdns_names[0], mdns_names[1]);
    mdns_join();
}

/* a DNS name at p (labels, maybe compressed) -> "a.b.c" lowercase; returns the offset after it in the message, or -1 */
static int dns_name(const uint8_t *msg, int len, int p, char *out, int outn) {
    int o = 0, end = -1, hops = 0;
    while (p < len && hops < 16) {
        const int l = msg[p];
        if (l == 0) { if (end < 0) end = p + 1; out[o] = 0; return end; }
        if ((l & 0xc0) == 0xc0) { if (p + 1 >= len) return -1; if (end < 0) end = p + 2; p = (l & 0x3f) << 8 | msg[p + 1]; hops++; continue; }
        if (p + 1 + l > len || o + l + 2 > outn) return -1;
        if (o) out[o++] = '.';
        for (int k = 0; k < l; k++) { char c = (char)msg[p + 1 + k]; out[o++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
        p += 1 + l;
    }
    return -1;
}

static void mdns_handle(void) {
    uint8_t msg[1500], cbuf[256];
    struct sockaddr_in from;
    struct iovec iov = {msg, sizeof msg};
    struct msghdr mh = {.msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = cbuf, .msg_controllen = sizeof cbuf};
    const ssize_t n = recvmsg(mdns_fd, &mh, 0);
    if (n < 12) return;
    struct in_addr me = {0};
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) me = ((struct in_pktinfo *)CMSG_DATA(c))->ipi_spec_dst;
    if (!me.s_addr) return;
    if (msg[2] & 0x80) return;   /* a response, not a question */
    const int qd = msg[4] << 8 | msg[5];
    int p = 12;
    for (int q = 0; q < qd && q < 8; q++) {
        char name[256];
        const int next = dns_name(msg, (int)n, p, name, sizeof name);
        if (next < 0 || next + 4 > n) return;
        const int type = msg[next] << 8 | msg[next + 1];
        p = next + 4;
        if (type != 1 && type != 255) continue;   /* A or ANY */
        int which = -1;
        for (int k = 0; k < 2; k++) {
            char want[140];
            snprintf(want, sizeof want, "%s.local", mdns_names[k]);
            for (char *c = want; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
            if (!strcmp(name, want)) which = k;
        }
        if (which < 0) continue;
        /* answer: [header][question, only for a one-shot (legacy) query][A record] */
        const int legacy = ntohs(from.sin_port) != 5353;
        uint8_t r[512];
        int o = 0;
        r[o++] = legacy ? msg[0] : 0; r[o++] = legacy ? msg[1] : 0;
        r[o++] = 0x84; r[o++] = 0;                   /* response, authoritative */
        r[o++] = 0; r[o++] = legacy ? 1 : 0;        /* questions */
        r[o++] = 0; r[o++] = 1;                     /* answers */
        r[o++] = 0; r[o++] = 0; r[o++] = 0; r[o++] = 0;
        uint8_t enc[128];
        int e = 0;
        const char *parts[2] = {mdns_names[which], "local"};
        for (int k = 0; k < 2; k++) { const int l = (int)strlen(parts[k]); enc[e++] = (uint8_t)l; memcpy(enc + e, parts[k], (size_t)l); e += l; }
        enc[e++] = 0;
        if (legacy) { memcpy(r + o, enc, (size_t)e); o += e; r[o++] = 0; r[o++] = 1; r[o++] = 0; r[o++] = 1; }
        memcpy(r + o, enc, (size_t)e); o += e;
        r[o++] = 0; r[o++] = 1;                               /* type A */
        r[o++] = legacy ? 0x00 : 0x80; r[o++] = 1;            /* class IN (+ cache flush for multicast) */
        r[o++] = 0; r[o++] = 0; r[o++] = 0; r[o++] = 120;     /* TTL 120 s */
        r[o++] = 0; r[o++] = 4;
        memcpy(r + o, &me.s_addr, 4); o += 4;
        struct sockaddr_in to = from;
        if (!legacy) { to.sin_addr.s_addr = inet_addr("224.0.0.251"); to.sin_port = htons(5353); }
        sendto(mdns_fd, r, (size_t)o, 0, (struct sockaddr *)&to, sizeof to);
        return;
    }
}

/* ---- main loop ------------------------------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    const int port = argc > 1 ? atoi(argv[1]) : 8080;
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setpriority(PRIO_PROCESS, 0, 10);
#ifdef SCHED_BATCH
    struct sched_param sp = {0};
    sched_setscheduler(0, SCHED_BATCH, &sp);
#endif
    find_touch();
    load_conf();
    btn_load();
    seq_open_();

    int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8)) { LOG("can't listen on port %d: %s", port, strerror(errno)); return 1; }
    LOG("mpc-remote: http://<this MPC's address>:%d/", port);
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    uint64_t next_frame = 0, next_try = 0, busy_until = 0, next_join = 0;
    zlib_load();
    mdns_open();
    for (;;) {
        struct pollfd pf[MAX_CLIENTS + 4];
        int np = 0, viewers = 0;
        pf[np++] = (struct pollfd){.fd = ls, .events = POLLIN};
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &clients[i];
            if (c->fd < 0) continue;
            if (c->ws == 1) viewers++;
            pf[np++] = (struct pollfd){.fd = c->fd, .events = (short)(POLLIN | (c->outpos < c->outlen ? POLLOUT : 0))};
        }
        const int calidx = np;
        if (cal.fd >= 0) pf[np++] = (struct pollfd){.fd = cal.fd, .events = POLLIN};
        const int mdnsidx = np;
        if (mdns_fd >= 0) pf[np++] = (struct pollfd){.fd = mdns_fd, .events = POLLIN};
        const int seqidx = np;
        if (seq_fd >= 0) pf[np++] = (struct pollfd){.fd = seq_fd, .events = POLLIN};
        const uint64_t t = now_ms();
        int wait = learn.idx >= 0 ? 200 : !viewers ? 1000 : !screen_ready ? 500 : (int)(next_frame > t ? next_frame - t : 0);
        poll(pf, (nfds_t)np, wait);

        if (pf[0].revents & POLLIN) {
            int fd = accept4(ls, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd < 0) { slot = i; break; }
                if (slot < 0) close(fd);
                else { memset(&clients[slot], 0, sizeof clients[slot]); clients[slot].fd = fd; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
            }
        }
        for (int k = 1; k < calidx; k++) {
            Client *c = NULL;
            for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd == pf[k].fd) c = &clients[i];
            if (!c) continue;
            if (pf[k].revents & (POLLERR | POLLHUP)) { if (c->ws) { LOG("viewer left"); kbd_release_all(); } client_close(c); continue; }
            if (pf[k].revents & POLLIN) {
                ssize_t r = read(c->fd, c->in + c->inlen, sizeof c->in - 1 - c->inlen);
                if (r <= 0) { if (c->ws) { LOG("viewer left"); kbd_release_all(); } client_close(c); continue; }
                c->inlen += (size_t)r;
                c->in[c->inlen] = 0;
                if (c->ws) handle_ws(c);
                else if (strstr(c->in, "\r\n\r\n")) handle_http(c);
                else if (c->inlen >= sizeof c->in - 1) { client_close(c); continue; }
                if (c->fd < 0) continue;
            }
            if ((pf[k].revents & POLLOUT) && c->outpos < c->outlen) {
                ssize_t w = write(c->fd, c->out + c->outpos, c->outlen - c->outpos);
                if (w < 0 && errno != EAGAIN) { client_close(c); continue; }
                if (w > 0) c->outpos += (size_t)w;
                if (c->outpos == c->outlen) {
                    c->outpos = c->outlen = 0;
                    if (!c->ws || c->ws == 2) { client_close(c); continue; }   /* plain HTTP, or a closed WebSocket */
                }
            }
        }

        if (mdns_fd >= 0 && np > mdnsidx && (pf[mdnsidx].revents & POLLIN)) mdns_handle();
        if (seq_fd >= 0 && np > seqidx && (pf[seqidx].revents & POLLIN)) seq_read();
        learn_tick();
        static uint64_t next_launch_check;
        if (now_ms() >= next_launch_check) { next_launch_check = now_ms() + 10000; launch_ok(); }
        if (mdns_fd >= 0 && now_ms() >= next_join) { next_join = now_ms() + 15000; mdns_join(); }   /* Wi-Fi (re)connects */

        /* calibration taps */
        if (cal.fd >= 0 && np > calidx && (pf[calidx].revents & POLLIN)) {
            struct input_event ev[64];
            ssize_t r = read(cal.fd, ev, sizeof ev);
            for (ssize_t i = 0; i < r / (ssize_t)sizeof *ev; i++) {
                if (ev[i].type == EV_ABS && (ev[i].code == ABS_MT_POSITION_X || ev[i].code == ABS_X)) cal.x = ev[i].value;
                if (ev[i].type == EV_ABS && (ev[i].code == ABS_MT_POSITION_Y || ev[i].code == ABS_Y)) cal.y = ev[i].value;
                if (ev[i].type == EV_KEY && ev[i].code == BTN_TOUCH) {
                    if (ev[i].value) cal.down = 1;
                    else if (cal.down) { cal.down = 0; cal_tap(cal.x, cal.y); }
                }
            }
        }
        if (cal.fd >= 0 && now_ms() - cal.started > 60000) {   /* never keep MPC's touchscreen for long */
            cal_stop();
            broadcast_text("{\"type\":\"cal\",\"error\":\"timed out\"}");
        }

        /* the display: open it while someone watches (once MPC has it), close it when nobody does */
        if (viewers && !screen_ready && now_ms() >= next_try) {
            next_try = now_ms() + 2000;
            const int ok = screen_open() == 0;
            static char told[160];
            if (!ok && strcmp(told, why_waiting)) {   /* tell the page why there's no picture yet */
                snprintf(told, sizeof told, "%s", why_waiting);
                char m[256];
                snprintf(m, sizeof m, "{\"type\":\"wait\",\"why\":\"%s\"}", why_waiting);
                broadcast_text(m);
            }
            if (ok) {
                told[0] = 0;
                const int tw = (W + TILE - 1) / TILE, th = (H + TILE - 1) / TILE;
                if (!cur || tw != TW || th != TH) {
                    TW = tw; TH = th;
                    free(cur); free(prev); free(rowbuf);
                    cur = calloc((size_t)W * H, 2);
                    prev = calloc((size_t)W * H, 2);
                    rowbuf = malloc((size_t)W * 4);
                }
                screen_ready = 1;
                have_prev = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].ws == 1) send_hello(&clients[i]);
            }
        }
        if (!viewers && screen_ready) { screen_close(); screen_ready = 0; LOG("nobody watching: display closed"); }

        /* the screen: grab a frame, mark what changed for every viewer, send to whoever is ready */
        if (touched_at) {   /* a touch from the page: look again very soon, and keep looking often for a while */
            busy_until = touched_at + BUSY_MS;
            if (next_frame > touched_at + 20) next_frame = touched_at + 20;   /* ~20 ms for MPC to redraw */
            touched_at = 0;
        }
        if (viewers && screen_ready && now_ms() >= next_frame) {
            const uint64_t t0 = now_ms();
            next_frame = t0 + (t0 < busy_until ? FAST_MS : SLOW_MS);
            const int ok = screen_grab() == 0;
            st.grab_ms += (double)(now_ms() - t0);
            st.frames++;
            if (!st.since) st.since = t0;
            if (t0 - st.since >= 10000) {   /* every 10 s while watched: how it's doing, for /log */
                LOG("last 10s: %u frames (%u changed), grab %.1f ms, send %.1f ms per frame; %lu KB/s sent (%lu%% of raw)",
                    st.frames, st.changed, st.grab_ms / st.frames, st.send_ms / st.frames, st.sent / 1024 / 10,
                    st.raw ? st.sent * 100 / st.raw : 0);
                memset(&st, 0, sizeof st);
                st.since = t0;
            }
            if (ok) {
                int any = 0;
                for (int ty = 0; ty < TH; ty++)
                    for (int tx = 0; tx < TW; tx++) {
                        int changed = !have_prev;
                        const int x0 = tx * TILE, w = W - x0 < TILE ? W - x0 : TILE;
                        for (int y = ty * TILE; y < H && y < (ty + 1) * TILE && !changed; y++)
                            changed = memcmp(cur + (size_t)y * W + x0, prev + (size_t)y * W + x0, (size_t)w * 2) != 0;
                        if (!changed) continue;
                        any = 1;
                        for (int i = 0; i < MAX_CLIENTS; i++) {
                            Client *c = &clients[i];
                            if (c->fd >= 0 && c->ws == 1 && c->dirty && !c->dirty[ty * TW + tx]) { c->dirty[ty * TW + tx] = 1; c->ndirty++; }
                        }
                    }
                have_prev = 1;
                if (any) { st.changed++; memcpy(prev, cur, (size_t)W * H * 2); }
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &clients[i];
            if (c->fd >= 0 && c->ws == 1 && c->dirty && c->ndirty && c->outlen == 0) send_tiles(c);
        }
    }
}
