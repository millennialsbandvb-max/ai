/* mpc-probe: look (read-only) at how MPC OS drives its screen and touchscreen, for the browser remote.
 *   mpc-probe <output dir>
 * Prints a report on stdout and saves screenshots in <output dir>:
 *   - the framebuffer device (/dev/fb0): its format, and fb0.bmp from its memory
 *   - DRM (/dev/dri/card*): connectors, CRTCs and planes, and for every framebuffer being shown, a screenshot taken
 *     two ways (a "dumb" buffer map, and a dma-buf export), so we learn which one gives the real picture
 *   - input devices: names, event types and touch ranges; and whether /dev/uinput (for injecting touches) opens
 * Nothing is changed: no modes are set, nothing grabs an input device, no input device is created. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <linux/dma-buf.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/uinput.h>

static const char *outdir;

/* ---- screenshots: any 16/32-bit pixel layout -> 24-bit BMP ---------------------------------------------------- */
typedef struct { int bpp, roff, rlen, goff, glen, boff, blen; } Layout;

static int save_bmp(const char *name, const uint8_t *src, int w, int h, int pitch, Layout L) {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return -1;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const int row = (w * 3 + 3) & ~3;
    const uint32_t size = 54 + (uint32_t)row * h;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t *line = calloc(1, row);
    for (int y = h - 1; y >= 0; y--) {   /* BMP rows go bottom-up */
        const uint8_t *p = src + (size_t)y * pitch;
        for (int x = 0; x < w; x++) {
            uint32_t v = L.bpp == 16 ? ((const uint16_t *)p)[x] : ((const uint32_t *)p)[x];
            uint32_t r = (v >> L.roff) & ((1u << L.rlen) - 1), g = (v >> L.goff) & ((1u << L.glen) - 1),
                     b = (v >> L.boff) & ((1u << L.blen) - 1);
            line[x * 3 + 0] = (uint8_t)(b * 255 / ((1u << L.blen) - 1));
            line[x * 3 + 1] = (uint8_t)(g * 255 / ((1u << L.glen) - 1));
            line[x * 3 + 2] = (uint8_t)(r * 255 / ((1u << L.rlen) - 1));
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    /* how much of the picture isn't black: an all-black capture means we read the wrong buffer */
    long lit = 0;
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            const uint8_t *p = src + (size_t)y * pitch;
            uint32_t v = L.bpp == 16 ? ((const uint16_t *)p)[x] : ((const uint32_t *)p)[x];
            if (v & 0x00ffffff) lit++;
        }
    printf("    saved %s (%dx%d), %ld%% non-black\n", name, w, h, lit * 100 / ((long)((w + 3) / 4) * ((h + 3) / 4)));
    return 0;
}

static int layout_of_fourcc(uint32_t fmt, Layout *L) {
    switch (fmt) {
    case DRM_FORMAT_XRGB8888: case DRM_FORMAT_ARGB8888: *L = (Layout){32, 16, 8, 8, 8, 0, 8}; return 0;
    case DRM_FORMAT_XBGR8888: case DRM_FORMAT_ABGR8888: *L = (Layout){32, 0, 8, 8, 8, 16, 8}; return 0;
    case DRM_FORMAT_RGB565: *L = (Layout){16, 11, 5, 5, 6, 0, 5}; return 0;
    case DRM_FORMAT_BGR565: *L = (Layout){16, 0, 5, 5, 6, 11, 5}; return 0;
    default: return -1;
    }
}

static void fourcc_str(uint32_t f, char *s) {
    for (int i = 0; i < 4; i++) { char c = (char)((f >> (8 * i)) & 0xff); s[i] = c >= 32 && c < 127 ? c : '?'; }
    s[4] = 0;
}

/* ---- /dev/fb* ------------------------------------------------------------------------------------------------ */
static void probe_fb(void) {
    printf("== framebuffer devices\n");
    for (int n = 0; n < 4; n++) {
        char dev[32];
        snprintf(dev, sizeof dev, "/dev/fb%d", n);
        int fd = open(dev, O_RDONLY);
        if (fd < 0) { if (errno != ENOENT) printf("%s: can't open: %s\n", dev, strerror(errno)); continue; }
        struct fb_var_screeninfo v; struct fb_fix_screeninfo x;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &x)) {
            printf("%s: info ioctl failed: %s\n", dev, strerror(errno)); close(fd); continue;
        }
        printf("%s: id=\"%.16s\" %ux%u (virtual %ux%u, offset %u,%u) bpp=%u line=%u mem=%u\n", dev, x.id, v.xres,
               v.yres, v.xres_virtual, v.yres_virtual, v.xoffset, v.yoffset, v.bits_per_pixel, x.line_length, x.smem_len);
        printf("    red %u/%u green %u/%u blue %u/%u alpha %u/%u, rotate %u\n", v.red.offset, v.red.length,
               v.green.offset, v.green.length, v.blue.offset, v.blue.length, v.transp.offset, v.transp.length, v.rotate);
        const size_t len = (size_t)x.line_length * v.yres_virtual;
        uint8_t *m = len ? mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
        if (m == MAP_FAILED) printf("    mmap failed: %s\n", strerror(errno));
        else {
            if (v.bits_per_pixel == 16 || v.bits_per_pixel == 32) {
                Layout L = {(int)v.bits_per_pixel, (int)v.red.offset, (int)v.red.length, (int)v.green.offset,
                            (int)v.green.length, (int)v.blue.offset, (int)v.blue.length};
                char name[32];
                snprintf(name, sizeof name, "fb%d.bmp", n);
                if (L.rlen && L.glen && L.blen)
                    save_bmp(name, m + (size_t)v.yoffset * x.line_length + v.xoffset * (v.bits_per_pixel / 8),
                             (int)v.xres, (int)v.yres, (int)x.line_length, L);
            }
            munmap(m, len);
        }
        close(fd);
    }
}

/* ---- DRM ----------------------------------------------------------------------------------------------------- */
static const char *conn_type(uint32_t t) {
    static const char *names[] = {"Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS",
                                  "Component", "9PinDIN", "DisplayPort", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual",
                                  "DSI", "DPI", "Writeback", "SPI", "USB"};
    return t < sizeof names / sizeof *names ? names[t] : "?";
}

static void capture_fb(int fd, uint32_t fb_id, const char *tag) {
    struct drm_mode_fb_cmd2 f2 = {.fb_id = fb_id};
    char name[64], fc[5];
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &f2) == 0) {
        fourcc_str(f2.pixel_format, fc);
        printf("    fb %u: %ux%u format %s pitch %u offset %u handle %u modifier 0x%llx (flags %u)\n", fb_id, f2.width,
               f2.height, fc, f2.pitches[0], f2.offsets[0], f2.handles[0], (unsigned long long)f2.modifier[0], f2.flags);
    } else {
        printf("    GETFB2 failed (%s), trying GETFB\n", strerror(errno));
        struct drm_mode_fb_cmd f = {.fb_id = fb_id};
        if (ioctl(fd, DRM_IOCTL_MODE_GETFB, &f)) { printf("    GETFB failed: %s\n", strerror(errno)); return; }
        printf("    fb %u: %ux%u bpp %u depth %u pitch %u handle %u\n", fb_id, f.width, f.height, f.bpp, f.depth, f.pitch,
               f.handle);
        f2.width = f.width; f2.height = f.height; f2.pitches[0] = f.pitch; f2.handles[0] = f.handle;
        f2.pixel_format = f.bpp == 16 ? DRM_FORMAT_RGB565 : DRM_FORMAT_XRGB8888;
    }
    if (!f2.handles[0]) { printf("    no buffer handle (needs root / CAP_SYS_ADMIN)\n"); return; }
    Layout L;
    const int known = layout_of_fourcc(f2.pixel_format, &L) == 0;
    if (!known) printf("    pixel format not decoded here; saving raw bytes only\n");
    const size_t len = (size_t)f2.pitches[0] * f2.height + f2.offsets[0];

    /* way 1: map it as a dumb buffer through the DRM device */
    struct drm_mode_map_dumb md = {.handle = f2.handles[0]};
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0) {
        uint8_t *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, (off_t)md.offset);
        if (m != MAP_FAILED) {
            printf("    dumb map: ok\n");
            snprintf(name, sizeof name, "drm-%s-fb%u-dumb.bmp", tag, fb_id);
            if (known) save_bmp(name, m + f2.offsets[0], (int)f2.width, (int)f2.height, (int)f2.pitches[0], L);
            munmap(m, len);
        } else printf("    dumb map: mmap failed: %s\n", strerror(errno));
    } else printf("    dumb map: failed: %s\n", strerror(errno));

    /* way 2: export it as a dma-buf and map that */
    struct drm_prime_handle ph = {.handle = f2.handles[0], .flags = DRM_CLOEXEC | DRM_RDWR};
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0 ||
        (ph.flags = DRM_CLOEXEC, ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0)) {
        uint8_t *m = mmap(NULL, len, PROT_READ, MAP_SHARED, ph.fd, 0);
        if (m != MAP_FAILED) {
            struct dma_buf_sync s = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
            const int synced = ioctl(ph.fd, DMA_BUF_IOCTL_SYNC, &s) == 0;
            printf("    dma-buf map: ok (sync %s)\n", synced ? "ok" : strerror(errno));
            snprintf(name, sizeof name, "drm-%s-fb%u-dmabuf.bmp", tag, fb_id);
            if (known) save_bmp(name, m + f2.offsets[0], (int)f2.width, (int)f2.height, (int)f2.pitches[0], L);
            else {
                char path[512];
                snprintf(path, sizeof path, "%s/drm-%s-fb%u.raw", outdir, tag, fb_id);
                FILE *r = fopen(path, "wb");
                if (r) { fwrite(m, 1, len, r); fclose(r); }
            }
            if (synced) { s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ; ioctl(ph.fd, DMA_BUF_IOCTL_SYNC, &s); }
            munmap(m, len);
        } else printf("    dma-buf map: mmap failed: %s\n", strerror(errno));
        close(ph.fd);
    } else printf("    dma-buf export: failed: %s\n", strerror(errno));

    for (int i = 0; i < 4; i++) {   /* GETFB2 made new handles: let them go */
        if (!f2.handles[i]) continue;
        int dup = 0;
        for (int j = 0; j < i; j++) dup |= f2.handles[j] == f2.handles[i];
        if (!dup) { struct drm_gem_close c = {.handle = f2.handles[i]}; ioctl(fd, DRM_IOCTL_GEM_CLOSE, &c); }
    }
}

static void probe_drm(void) {
    printf("== DRM devices\n");
    for (int n = 0; n < 4; n++) {
        char dev[32];
        snprintf(dev, sizeof dev, "/dev/dri/card%d", n);
        int fd = open(dev, O_RDWR | O_CLOEXEC);
        if (fd < 0) { if (errno != ENOENT) printf("%s: can't open: %s\n", dev, strerror(errno)); continue; }
        char drv[64] = {0}, date[64] = {0}, desc[128] = {0};
        struct drm_version ver = {.name_len = sizeof drv - 1, .name = drv, .date_len = sizeof date - 1, .date = date,
                                  .desc_len = sizeof desc - 1, .desc = desc};
        ioctl(fd, DRM_IOCTL_VERSION, &ver);
        printf("%s: driver \"%s\" (%s) %d.%d.%d\n", dev, drv, desc, ver.version_major, ver.version_minor,
               ver.version_patchlevel);
        struct drm_set_client_cap cap = {.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1};
        ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);

        struct drm_mode_card_res res = {0};
        if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { printf("    GETRESOURCES failed: %s\n", strerror(errno)); close(fd); continue; }
        uint32_t *crtcs = calloc(res.count_crtcs + 1, 4), *conns = calloc(res.count_connectors + 1, 4),
                 *encs = calloc(res.count_encoders + 1, 4), *fbs = calloc(res.count_fbs + 1, 4);
        res.crtc_id_ptr = (uintptr_t)crtcs; res.connector_id_ptr = (uintptr_t)conns;
        res.encoder_id_ptr = (uintptr_t)encs; res.fb_id_ptr = (uintptr_t)fbs;
        ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
        printf("    %u connectors, %u crtcs, %u encoders; size %u-%u x %u-%u\n", res.count_connectors, res.count_crtcs,
               res.count_encoders, res.min_width, res.max_width, res.min_height, res.max_height);

        for (uint32_t i = 0; i < res.count_connectors; i++) {
            struct drm_mode_get_connector c = {.connector_id = conns[i]};
            if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
            struct drm_mode_modeinfo *modes = calloc(c.count_modes + 1, sizeof *modes);
            uint32_t *props = calloc(c.count_props + 1, 4), *encs2 = calloc(c.count_encoders + 1, 4);
            uint64_t *vals = calloc(c.count_props + 1, 8);
            c.modes_ptr = (uintptr_t)modes; c.props_ptr = (uintptr_t)props; c.prop_values_ptr = (uintptr_t)vals;
            c.encoders_ptr = (uintptr_t)encs2;
            ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
            printf("    connector %u: %s-%u %s, %umm x %umm, encoder %u, %u modes", c.connector_id,
                   conn_type(c.connector_type), c.connector_type_id,
                   c.connection == 1 ? "connected" : c.connection == 2 ? "disconnected" : "unknown", c.mm_width,
                   c.mm_height, c.encoder_id, c.count_modes);
            for (uint32_t k = 0; k < c.count_modes && k < 3; k++) printf(" [%s]", modes[k].name);
            printf("\n");
            free(modes); free(props); free(vals); free(encs2);
        }
        for (uint32_t i = 0; i < res.count_crtcs; i++) {
            struct drm_mode_crtc c = {.crtc_id = crtcs[i]};
            if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c)) continue;
            printf("    crtc %u: %s mode \"%s\" %ux%u, fb %u at %u,%u\n", c.crtc_id, c.mode_valid ? "active" : "off",
                   c.mode.name, c.mode.hdisplay, c.mode.vdisplay, c.fb_id, c.x, c.y);
            if (c.fb_id) { char tag[32]; snprintf(tag, sizeof tag, "card%d-crtc%u", n, c.crtc_id); capture_fb(fd, c.fb_id, tag); }
        }
        struct drm_mode_get_plane_res pr = {0};
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) == 0 && pr.count_planes) {
            uint32_t *planes = calloc(pr.count_planes, 4);
            pr.plane_id_ptr = (uintptr_t)planes;
            ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
            for (uint32_t i = 0; i < pr.count_planes; i++) {
                struct drm_mode_get_plane p = {.plane_id = planes[i]};
                if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p)) continue;
                printf("    plane %u: crtc %u fb %u, %u formats\n", p.plane_id, p.crtc_id, p.fb_id, p.count_format_types);
                if (p.fb_id) { char tag[32]; snprintf(tag, sizeof tag, "card%d-plane%u", n, p.plane_id); capture_fb(fd, p.fb_id, tag); }
            }
            free(planes);
        }
        free(crtcs); free(conns); free(encs); free(fbs);
        close(fd);
    }
}

/* ---- input --------------------------------------------------------------------------------------------------- */
#define BITS(n) (((n) + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long)))
static int test_bit(const unsigned long *b, int i) { return (b[i / (8 * sizeof *b)] >> (i % (8 * sizeof *b))) & 1; }

static void probe_input(void) {
    printf("== input devices\n");
    DIR *d = opendir("/dev/input");
    if (!d) { printf("no /dev/input\n"); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char dev[300];
        snprintf(dev, sizeof dev, "/dev/input/%s", e->d_name);
        int fd = open(dev, O_RDONLY | O_NONBLOCK);
        if (fd < 0) { printf("%s: can't open: %s\n", dev, strerror(errno)); continue; }
        char name[128] = {0}, phys[128] = {0};
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        ioctl(fd, EVIOCGPHYS(sizeof phys - 1), phys);
        struct input_id id = {0};
        ioctl(fd, EVIOCGID, &id);
        unsigned long ev[BITS(EV_CNT)] = {0}, abs[BITS(ABS_CNT)] = {0}, key[BITS(KEY_CNT)] = {0},
                      prop[BITS(INPUT_PROP_CNT)] = {0};
        ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
        ioctl(fd, EVIOCGPROP(sizeof prop), prop);
        printf("%s: \"%s\" phys=\"%s\" bus %04x vendor %04x product %04x;%s%s%s%s%s%s\n", dev, name, phys, id.bustype,
               id.vendor, id.product, test_bit(ev, EV_KEY) ? " KEY" : "", test_bit(ev, EV_ABS) ? " ABS" : "",
               test_bit(ev, EV_REL) ? " REL" : "", test_bit(ev, EV_MSC) ? " MSC" : "",
               test_bit(prop, INPUT_PROP_DIRECT) ? " (direct: a touchscreen)" : "",
               test_bit(key, BTN_TOUCH) ? " BTN_TOUCH" : "");
        static const struct { int code; const char *name; } axes[] = {
            {ABS_X, "ABS_X"}, {ABS_Y, "ABS_Y"}, {ABS_PRESSURE, "ABS_PRESSURE"}, {ABS_MT_SLOT, "MT_SLOT"},
            {ABS_MT_TOUCH_MAJOR, "MT_TOUCH_MAJOR"}, {ABS_MT_POSITION_X, "MT_POSITION_X"},
            {ABS_MT_POSITION_Y, "MT_POSITION_Y"}, {ABS_MT_TRACKING_ID, "MT_TRACKING_ID"},
            {ABS_MT_PRESSURE, "MT_PRESSURE"}};
        for (size_t k = 0; k < sizeof axes / sizeof *axes; k++) {
            if (!test_bit(abs, axes[k].code)) continue;
            struct input_absinfo a = {0};
            ioctl(fd, EVIOCGABS(axes[k].code), &a);
            printf("    %s: %d..%d (fuzz %d, flat %d, res %d)\n", axes[k].name, a.minimum, a.maximum, a.fuzz, a.flat,
                   a.resolution);
        }
        close(fd);
    }
    closedir(d);

    printf("== uinput (for sending touches from the browser)\n");
    int u = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (u < 0) printf("/dev/uinput: can't open: %s\n", strerror(errno));
    else {
        unsigned int v = 0;
        const int r = ioctl(u, UI_GET_VERSION, &v);
        printf("/dev/uinput: opens; version %s%u\n", r ? "unknown " : "", v);
        close(u);   /* no device is created */
    }
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <output dir>\n", argv[0]); return 2; }
    outdir = argv[1];
    setvbuf(stdout, NULL, _IOLBF, 0);
    probe_fb();
    probe_drm();
    probe_input();
    return 0;
}
