/* The MPC browser on the MPC's own screen (part of mpc-buttons.so, inside MPC). See ../overlay.h.
 *
 * MPC draws its screen in software and shows each frame with a DRM atomic commit (Hakai's mouse cursor hooks the
 * same call to add its cursor layer). Here, at each commit:
 *   - MPC's picture is checked for the MPC Browser plugin's page: an area painted OVERLAY_MARKER. Anything MPC puts
 *     over it (a menu, a popup) breaks the area up, and then it doesn't count;
 *   - while that page is on screen, the browser's picture (from mpc-remote) is shown exactly over that area on a
 *     spare hardware layer ("overlay plane"). MPC's own picture isn't touched: the layer just sits in front of it;
 *   - touches that start on that area go to the browser instead of MPC (libinput_get_event).
 * If the display refuses the extra layer even once, the browser layer is switched off for the rest of the run, and
 * MPC goes on as if none of this were here. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include "../overlay.h"

static void olog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void olog(const char *fmt, ...) {
    FILE *f = fopen("/tmp/mpc-buttons.log", "a");
    if (!f) return;
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    fprintf(f, "%s [%d] browser layer: ", ts, (int)getpid());
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    __builtin_va_end(ap);
    fputc('\n', f);
    fclose(f);
}

enum { DRM_PLANE_TYPE_OVERLAY = 0, DRM_PLANE_TYPE_PRIMARY = 1, DRM_PLANE_TYPE_CURSOR = 2 };   /* (libdrm's values; not in the kernel headers) */
#define PW 800    /* the panel, as it scans (portrait) */
#define PH 1280

static struct overlay_shm *ov;
static int broken;              /* gave up: never touch the display again */
static int ready;               /* planes and properties found */
static uint32_t primary, layer, crtc;
static uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch;   /* layer properties */
static uint32_t prim_fb_prop;
static uint32_t p_zpos;         /* the layer's stacking order, if it can be set: put it in front of MPC's */
static uint64_t zpos_front;
static uint64_t mpc_fb;         /* the picture MPC is showing (its primary plane's FB_ID) */
/* the mouse pointer: Hakai's mpc_cursor.so draws it on the cursor plane; where it puts that plane is where it is */
static uint32_t cursor_plane, c_x, c_y, c_w, c_h;
static volatile int64_t cur_x = -1, cur_y = -1, cur_w = 32, cur_h = 32;
static int shown;               /* our layer is on */
static volatile int vis;        /* the page is on screen (for the touch hook) */
static int rx0, ry0, rw, rh;    /* where (landscape) */
static int orient;              /* how the touchscreen's axes lie (the remote's calibration) */

/* ---- MPC's own picture: map its framebuffers to look for the page --------------------------------------- */
static struct { uint32_t id; uint32_t *px; uint32_t pitch; size_t size; } maps[8];

static uint32_t *fb_pixels(int fd, uint32_t id, uint32_t *pitch) {
    for (int i = 0; i < 8; i++) if (maps[i].id == id) { *pitch = maps[i].pitch; return maps[i].px; }
    struct drm_mode_fb_cmd f = {.fb_id = id};
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB, &f) || f.bpp != 32 || f.width != PW || f.height != PH || !f.handle) return NULL;
    struct drm_mode_map_dumb m = {.handle = f.handle};
    uint32_t *px = NULL;
    const size_t size = (size_t)f.pitch * f.height;
    if (!ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) {
        void *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, (off_t)m.offset);
        if (p != MAP_FAILED) px = p;
    }
    struct drm_gem_close c = {.handle = f.handle};
    ioctl(fd, DRM_IOCTL_GEM_CLOSE, &c);   /* the mapping stays */
    if (!px) return NULL;
    int slot = 0;
    for (int i = 0; i < 8; i++) if (!maps[i].id) { slot = i; break; }
    if (maps[slot].px) munmap(maps[slot].px, maps[slot].size);
    maps[slot].id = id; maps[slot].px = px; maps[slot].pitch = f.pitch; maps[slot].size = size;
    *pitch = f.pitch;
    return px;
}

/* landscape (as seen) -> the panel's portrait pixels */
static inline uint32_t at(const uint32_t *px, uint32_t pitch, int lx, int ly) {
    return px[(size_t)(PH - 1 - lx) * (pitch / 4) + ly] & 0xffffff;
}

/* is the plugin's page on screen, and where (the marker area, exact to the pixel)? */
static int find_page(const uint32_t *px, uint32_t pitch, int *x, int *y, int *w, int *h) {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1, n = 0;
    for (int ly = 8; ly < PW; ly += 16)
        for (int lx = 8; lx < PH; lx += 16)
            if (at(px, pitch, lx, ly) == OVERLAY_MARKER) {
                n++;
                if (lx < x0) x0 = lx;
                if (lx > x1) x1 = lx;
                if (ly < y0) y0 = ly;
                if (ly > y1) y1 = ly;
            }
    if (n < 64) return 0;   /* (a real page area is hundreds of samples) */
    const int my = (y0 + y1) / 2, mx = (x0 + x1) / 2;   /* exact edges, along the middle */
    while (x0 > 0 && at(px, pitch, x0 - 1, my) == OVERLAY_MARKER) x0--;
    while (x1 < PH - 1 && at(px, pitch, x1 + 1, my) == OVERLAY_MARKER) x1++;
    while (y0 > 0 && at(px, pitch, mx, y0 - 1) == OVERLAY_MARKER) y0--;
    while (y1 < PW - 1 && at(px, pitch, mx, y1 + 1) == OVERLAY_MARKER) y1++;
    /* all of it must be the marker: anything MPC drew over it (a popup) means it's not ours to cover */
    int in = 0, hit = 0;
    for (int ly = y0 + 4; ly <= y1 - 4; ly += 12)
        for (int lx = x0 + 4; lx <= x1 - 4; lx += 12) { in++; hit += at(px, pitch, lx, ly) == OVERLAY_MARKER; }
    if (!in || hit < in) return 0;
    *x = x0; *y = y0; *w = x1 - x0 + 1; *h = y1 - y0 + 1;
    return 1;
}

/* ---- planes and properties ---------------------------------------------------------------------------- */
static uint32_t prop_id_ex(int fd, uint32_t obj, const char *name, uint64_t *value, uint32_t *flags, uint64_t *max);
static uint32_t prop_id(int fd, uint32_t obj, const char *name, uint64_t *value) { return prop_id_ex(fd, obj, name, value, NULL, NULL); }
static uint32_t prop_id_ex(int fd, uint32_t obj, const char *name, uint64_t *value, uint32_t *flags, uint64_t *max) {
    uint32_t ids[64];
    uint64_t vals[64];
    struct drm_mode_obj_get_properties gp = {.props_ptr = (uintptr_t)ids, .prop_values_ptr = (uintptr_t)vals,
                                             .count_props = 64, .obj_id = obj, .obj_type = DRM_MODE_OBJECT_PLANE};
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp)) return 0;
    for (uint32_t i = 0; i < gp.count_props && i < 64; i++) {
        uint64_t range[2] = {0, 0};
        struct drm_mode_get_property p = {.prop_id = ids[i]};
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p)) continue;
        if (strcmp(p.name, name)) continue;
        if (max && (p.flags & DRM_MODE_PROP_RANGE) && p.count_values == 2) {
            struct drm_mode_get_property q = {.prop_id = ids[i], .values_ptr = (uintptr_t)range, .count_values = 2};
            if (!ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &q)) *max = range[1];
        }
        if (flags) *flags = p.flags;
        if (value) *value = vals[i];
        return ids[i];
    }
    return 0;
}

static int setup(int fd) {
    uint32_t planes[32];
    struct drm_mode_get_plane_res pr = {.plane_id_ptr = (uintptr_t)planes, .count_planes = 32};
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) return -1;
    uint32_t crtcs[8];
    struct drm_mode_card_res cr = {.crtc_id_ptr = (uintptr_t)crtcs, .count_crtcs = 8};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &cr)) return -1;
    int crtc_index = -1, best_formats = 0;
    for (uint32_t i = 0; i < pr.count_planes && i < 32; i++) {   /* MPC's primary plane: the one showing */
        uint64_t type = 0;
        struct drm_mode_get_plane gp = {.plane_id = planes[i]};
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp) || !prop_id(fd, planes[i], "type", &type)) continue;
        if (type == DRM_PLANE_TYPE_PRIMARY && gp.crtc_id) { primary = planes[i]; crtc = gp.crtc_id; }
    }
    for (uint32_t i = 0; i < cr.count_crtcs && i < 8; i++) if (crtcs[i] == crtc) crtc_index = (int)i;
    if (!primary || crtc_index < 0) return -1;
    for (uint32_t i = 0; i < pr.count_planes && i < 32; i++) {   /* a free overlay plane that takes XRGB8888 */
        uint64_t type = 0;
        uint32_t fmts[64];
        struct drm_mode_get_plane gp = {.plane_id = planes[i], .format_type_ptr = (uintptr_t)fmts, .count_format_types = 64};
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp) || !prop_id(fd, planes[i], "type", &type)) continue;
        if (type != DRM_PLANE_TYPE_OVERLAY || gp.crtc_id || !(gp.possible_crtcs & (1u << crtc_index))) continue;
        int xrgb = 0;
        for (uint32_t k = 0; k < gp.count_format_types && k < 64; k++) xrgb |= fmts[k] == DRM_FORMAT_XRGB8888;
        if (xrgb && (int)gp.count_format_types > best_formats) { layer = planes[i]; best_formats = (int)gp.count_format_types; }
    }
    if (!layer) return -1;
    for (uint32_t i = 0; i < pr.count_planes && i < 32 && !cursor_plane; i++) {
        uint64_t type = 0;
        if (prop_id(fd, planes[i], "type", &type) && type == DRM_PLANE_TYPE_CURSOR) {
            cursor_plane = planes[i];
            c_x = prop_id(fd, planes[i], "CRTC_X", NULL); c_y = prop_id(fd, planes[i], "CRTC_Y", NULL);
            c_w = prop_id(fd, planes[i], "CRTC_W", NULL); c_h = prop_id(fd, planes[i], "CRTC_H", NULL);
        }
    }
    prim_fb_prop = prop_id(fd, primary, "FB_ID", NULL);
    p_fb = prop_id(fd, layer, "FB_ID", NULL); p_crtc = prop_id(fd, layer, "CRTC_ID", NULL);
    p_sx = prop_id(fd, layer, "SRC_X", NULL); p_sy = prop_id(fd, layer, "SRC_Y", NULL);
    p_sw = prop_id(fd, layer, "SRC_W", NULL); p_sh = prop_id(fd, layer, "SRC_H", NULL);
    p_cx = prop_id(fd, layer, "CRTC_X", NULL); p_cy = prop_id(fd, layer, "CRTC_Y", NULL);
    p_cw = prop_id(fd, layer, "CRTC_W", NULL); p_ch = prop_id(fd, layer, "CRTC_H", NULL);
    if (!prim_fb_prop || !p_fb || !p_crtc || !p_sx || !p_sy || !p_sw || !p_sh || !p_cx || !p_cy || !p_cw || !p_ch) return -1;
    uint64_t zp = 0, zl = 0, zmax = 0;
    uint32_t zflags = 0;
    if (prop_id(fd, primary, "zpos", &zp) && (p_zpos = prop_id_ex(fd, layer, "zpos", &zl, &zflags, &zmax))) {
        if ((zflags & DRM_MODE_PROP_IMMUTABLE) || zl > zp) p_zpos = 0;   /* fixed, or already in front */
        else if (zmax > zp) zpos_front = zmax;
        else { olog("plane %u can't go in front of MPC's (zpos %llu, max %llu)", layer, (unsigned long long)zp, (unsigned long long)zmax); return -1; }
    }
    olog("MPC shows plane %u on crtc %u; the browser can use plane %u (%d formats%s)", primary, crtc, layer, best_formats,
         p_zpos ? ", raised in front" : "");
    return 0;
}

/* ---- our layer's pictures: two buffers, so the one on screen is never the one being written ---------------- */
static struct { uint32_t fb, handle, pitch, w, h; uint32_t *px; size_t size; } buf[2];
static int cur = -1;
static uint32_t copied_seq = ~0u;

static void free_bufs(int fd) {
    for (int i = 0; i < 2; i++) {
        if (buf[i].px) munmap(buf[i].px, buf[i].size);
        if (buf[i].fb) ioctl(fd, DRM_IOCTL_MODE_RMFB, &buf[i].fb);
        if (buf[i].handle) { struct drm_mode_destroy_dumb d = {.handle = buf[i].handle}; ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d); }
        memset(&buf[i], 0, sizeof buf[i]);
    }
    cur = -1;
    copied_seq = ~0u;
}

static int make_bufs(int fd, uint32_t w, uint32_t h) {
    if (buf[0].fb && buf[0].w == w && buf[0].h == h) return 0;
    free_bufs(fd);
    for (int i = 0; i < 2; i++) {
        struct drm_mode_create_dumb c = {.width = w, .height = h, .bpp = 32};
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) goto fail;
        buf[i].handle = c.handle; buf[i].pitch = c.pitch; buf[i].w = w; buf[i].h = h; buf[i].size = c.size;
        struct drm_mode_fb_cmd2 f = {.width = w, .height = h, .pixel_format = DRM_FORMAT_XRGB8888};
        f.handles[0] = c.handle; f.pitches[0] = c.pitch;
        if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f)) goto fail;
        buf[i].fb = f.fb_id;
        struct drm_mode_map_dumb m = {.handle = c.handle};
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) goto fail;
        void *p = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
        if (p == MAP_FAILED) goto fail;
        buf[i].px = p;
        memset(p, 0, c.size);
    }
    return 0;
fail:
    olog("can't make the layer's %ux%u pictures", w, h);
    free_bufs(fd);
    return -1;
}

static void ov_map(void) {
    if (ov) return;
    const int fd = open(OVERLAY_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (!ftruncate(fd, sizeof(struct overlay_shm))) {
        void *m = mmap(NULL, sizeof(struct overlay_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) ov = m;
    }
    close(fd);
    if (ov && ov->magic != OVERLAY_MAGIC) { memset(ov, 0, offsetof(struct overlay_shm, frame)); ov->magic = OVERLAY_MAGIC; }
    FILE *f = fopen("/data/mpc-remote/touch.conf", "r");
    if (f) { if (fscanf(f, "orient=%d", &orient) != 1 || orient < 0 || orient > 7) orient = 0; fclose(f); }
}

/* ---- the hooks ------------------------------------------------------------------------------------------ */
static struct { uint32_t obj, prop; uint64_t val; } recent[32];   /* properties set before setup() ran */
static unsigned nrecent;

int drmModeAtomicAddProperty(void *req, uint32_t obj, uint32_t prop, uint64_t value) {
    static int (*real)(void *, uint32_t, uint32_t, uint64_t);
    if (!real) real = (int (*)(void *, uint32_t, uint32_t, uint64_t))dlsym(RTLD_NEXT, "drmModeAtomicAddProperty");
    if (!real) return -38;
    if (ready && obj == primary && prop == prim_fb_prop) mpc_fb = value;
    else if (ready && cursor_plane && obj == cursor_plane) {
        if (prop == c_x) cur_x = (int32_t)value;
        else if (prop == c_y) cur_y = (int32_t)value;
        else if (prop == c_w) cur_w = (int64_t)value;
        else if (prop == c_h) cur_h = (int64_t)value;
    }
    else if (!ready) {
        unsigned k = 0;
        while (k < nrecent && !(recent[k].obj == obj && recent[k].prop == prop)) k++;
        if (k == nrecent && nrecent < 32) nrecent++;
        if (k < 32) { recent[k].obj = obj; recent[k].prop = prop; recent[k].val = value; }
    }
    return real(req, obj, prop, value);
}

int drmModeAtomicCommit(int fd, void *req, uint32_t flags, void *user) {
    static int (*real)(int, void *, uint32_t, void *);
    static int (*add)(void *, uint32_t, uint32_t, uint64_t);
    static int (*get_cursor)(void *);
    static void (*set_cursor)(void *, int);
    if (!real) {
        real = (int (*)(int, void *, uint32_t, void *))dlsym(RTLD_NEXT, "drmModeAtomicCommit");
        add = (int (*)(void *, uint32_t, uint32_t, uint64_t))dlsym(RTLD_NEXT, "drmModeAtomicAddProperty");
        get_cursor = (int (*)(void *))dlsym(RTLD_DEFAULT, "drmModeAtomicGetCursor");
        set_cursor = (void (*)(void *, int))dlsym(RTLD_DEFAULT, "drmModeAtomicSetCursor");
    }
    if (!real) return -38;
    if (broken || !add || !get_cursor || !set_cursor) return real(fd, req, flags, user);
    if (!ready) {
        ov_map();
        if (!ov || setup(fd)) { broken = 1; olog("no spare display layer: the browser can't show on the MPC's screen"); return real(fd, req, flags, user); }
        ready = 1;
        /* the FB_ID MPC may have set just before, in this same request */
        for (unsigned k = 0; k < nrecent; k++) if (recent[k].obj == primary && recent[k].prop == prim_fb_prop) mpc_fb = recent[k].val;
    }
    /* the page: on screen, and where? */
    int x = 0, y = 0, w = 0, h = 0, on = 0;
    uint32_t pitch = 0;
    const uint32_t *px = mpc_fb ? fb_pixels(fd, (uint32_t)mpc_fb, &pitch) : NULL;
    if (px) on = find_page(px, pitch, &x, &y, &w, &h);
    if (on != vis || x != rx0 || y != ry0 || w != rw || h != rh) {
        rx0 = x; ry0 = y; rw = w; rh = h;
        ov->x = x; ov->y = y; ov->w = w; ov->h = h;
        __atomic_store_n(&ov->visible, (uint32_t)on, __ATOMIC_RELEASE);
        vis = on;
    }
    const int before = get_cursor(req);
    int added = 0;
    const uint32_t seq = __atomic_load_n(&ov->frame_seq, __ATOMIC_ACQUIRE);
    /* the browser's picture, turned for the panel: (h x w) -- shown once mpc-remote has one that size */
    if (on && ov->fw == (uint32_t)h && ov->fh == (uint32_t)w && seq && !make_bufs(fd, (uint32_t)h, (uint32_t)w)) {
        if (seq != copied_seq || cur < 0) {
            const int b = cur < 0 ? 0 : 1 - cur;
            const uint32_t *src = ov->frame[seq & 1];
            for (int r = 0; r < w; r++) memcpy((uint8_t *)buf[b].px + (size_t)r * buf[b].pitch, src + (size_t)r * h, (size_t)h * 4);
            cur = b;
            copied_seq = seq;
        }
        /* landscape (x, y, w, h) on the panel: portrait x = y, portrait y = PH - x - w */
        add(req, layer, p_fb, buf[cur].fb);
        add(req, layer, p_crtc, crtc);
        add(req, layer, p_sx, 0); add(req, layer, p_sy, 0);
        add(req, layer, p_sw, (uint64_t)h << 16); add(req, layer, p_sh, (uint64_t)w << 16);
        add(req, layer, p_cx, (uint64_t)y); add(req, layer, p_cy, (uint64_t)(PH - x - w));
        add(req, layer, p_cw, (uint64_t)h); add(req, layer, p_ch, (uint64_t)w);
        if (p_zpos) add(req, layer, p_zpos, zpos_front);
        added = 1;
    } else if (shown) {
        add(req, layer, p_fb, 0);
        add(req, layer, p_crtc, 0);
        added = 2;
    }
    int r = real(fd, req, flags, user);
    if (r < 0 && added) {   /* the display didn't take it: take our part out, and leave the display alone from now on */
        set_cursor(req, before);
        broken = 1;
        olog("the display refused the browser layer (%d): switched off until MPC restarts", r);
        r = real(fd, req, flags, user);
        __atomic_store_n(&ov->visible, 0u, __ATOMIC_RELEASE);
        vis = 0;
    } else if (added == 1 && !shown) { shown = 1; olog("showing the browser at %d,%d %dx%d", x, y, w, h); }
    else if (added == 2) shown = 0;
    return r;
}

/* touches that start on the browser's area go to the browser, not MPC (until they lift) */
struct libinput;
struct libinput_event;
struct libinput_event *libinput_get_event(struct libinput *li) {
    static struct libinput_event *(*real)(struct libinput *);
    static int (*type)(struct libinput_event *);
    static void *(*touch)(struct libinput_event *);
    static int (*slotf)(void *);
    static double (*xt)(void *, uint32_t), (*yt)(void *, uint32_t);
    static void (*destroy)(struct libinput_event *);
    static uint8_t mine[16];
    static void *(*pointer)(struct libinput_event *);
    static int (*has_axis)(void *, int);
    static double (*v120)(void *, int);
    if (!real) {
        real = (struct libinput_event * (*)(struct libinput *))dlsym(RTLD_NEXT, "libinput_get_event");
        type = (int (*)(struct libinput_event *))dlsym(RTLD_DEFAULT, "libinput_event_get_type");
        touch = (void *(*)(struct libinput_event *))dlsym(RTLD_DEFAULT, "libinput_event_get_touch_event");
        slotf = (int (*)(void *))dlsym(RTLD_DEFAULT, "libinput_event_touch_get_slot");
        xt = (double (*)(void *, uint32_t))dlsym(RTLD_DEFAULT, "libinput_event_touch_get_x_transformed");
        yt = (double (*)(void *, uint32_t))dlsym(RTLD_DEFAULT, "libinput_event_touch_get_y_transformed");
        destroy = (void (*)(struct libinput_event *))dlsym(RTLD_DEFAULT, "libinput_event_destroy");
        pointer = (void *(*)(struct libinput_event *))dlsym(RTLD_DEFAULT, "libinput_event_get_pointer_event");
        has_axis = (int (*)(void *, int))dlsym(RTLD_DEFAULT, "libinput_event_pointer_has_axis");
        v120 = (double (*)(void *, int))dlsym(RTLD_DEFAULT, "libinput_event_pointer_get_scroll_value_v120");
    }
    if (!real) return NULL;
    for (;;) {
        struct libinput_event *e = real(li);
        if (!e || !ov || !type || !touch || !slotf || !xt || !yt || !destroy) return e;
        const int t = type(e);
        /* the mouse wheel over the browser: scroll the browser, not MPC */
        enum { AXIS = 403, SCROLL_WHEEL = 404, SCROLL_CONTINUOUS = 406 };
        if (t >= AXIS && t <= SCROLL_CONTINUOUS && vis && cur_x >= 0 && pointer && has_axis && v120) {
            /* the pointer's tip, as seen: the cursor picture's top-left corner (the panel is turned) */
            const int lx = PH - (int)cur_y - (int)cur_h, ly = (int)cur_x;
            if (lx >= rx0 && lx < rx0 + rw && ly >= ry0 && ly < ry0 + rh) {
                if (t == SCROLL_WHEEL) {   /* (the older AXIS and touchpad events are just kept from MPC) */
                    void *pe = pointer(e);
                    const int d = has_axis(pe, 0) ? (int)v120(pe, 0) : 0;
                    if (d) {
                        const uint32_t i = ov->touch_wr % OVERLAY_TOUCHES;
                        ov->touch[i].kind = OV_WHEEL;
                        ov->touch[i].x = (int16_t)(lx - rx0);
                        ov->touch[i].y = (int16_t)(ly - ry0);
                        ov->touch[i].slot = (int16_t)(d > 32000 ? 32000 : d < -32000 ? -32000 : d);
                        __atomic_store_n(&ov->touch_wr, ov->touch_wr + 1, __ATOMIC_RELEASE);
                    }
                }
                destroy(e);
                continue;
            }
        }
        enum { DOWN = 500, UP = 501, MOTION = 502, CANCEL = 503 };
        if (t < DOWN || t > CANCEL) return e;
        void *te = touch(e);
        int slot = slotf(te);
        if (slot < 0 || slot > 15) slot = 0;
        if (t == DOWN && !vis) return e;
        if (t != DOWN && !mine[slot]) return e;
        int lx = 0, ly = 0;
        if (t == DOWN || t == MOTION) {   /* touchscreen axes -> the panel (as calibrated in the remote) -> as seen */
            double a = xt(te, 10000) / 10000.0, b = yt(te, 10000) / 10000.0;
            if (orient & 2) a = 1 - a;
            if (orient & 4) b = 1 - b;
            const double u = (orient & 1) ? b : a, v = (orient & 1) ? a : b;
            const int pxp = (int)(u * (PW - 1) + 0.5), pyp = (int)(v * (PH - 1) + 0.5);
            lx = PH - 1 - pyp; ly = pxp;
        }
        if (t == DOWN && !(lx >= rx0 && lx < rx0 + rw && ly >= ry0 && ly < ry0 + rh)) return e;   /* not on it */
        const uint32_t i = ov->touch_wr % OVERLAY_TOUCHES;
        ov->touch[i].kind = (int16_t)(t == DOWN ? OV_DOWN : t == MOTION ? OV_MOVE : OV_UP);
        ov->touch[i].x = (int16_t)(lx - rx0);
        ov->touch[i].y = (int16_t)(ly - ry0);
        ov->touch[i].slot = (int16_t)slot;
        __atomic_store_n(&ov->touch_wr, ov->touch_wr + 1, __ATOMIC_RELEASE);
        mine[slot] = t == DOWN || t == MOTION;
        destroy(e);   /* MPC doesn't see it */
    }
}
