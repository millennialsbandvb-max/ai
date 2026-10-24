/* Web In (part of mpc-buttons.so, inside MPC): audio streamed from the remote page becomes one of MPC's inputs.
 *
 * MPC reads its inputs with snd_pcm_readi/readn. After each read, the page's audio (from the ring mpc-remote fills,
 * see ../webin.h) is added to the input pair the page chose, so any audio track set to that input hears it, monitors
 * it and records it like a cable plugged in. Nothing changes while nothing is streamed.
 *
 * The reads run on MPC's audio thread, so the work there takes no locks and makes no system calls: the ring is
 * mapped when MPC sets up its input (snd_pcm_hw_params, not real time). The page's clock and the MPC's differ a
 * little, so the reader keeps about 0.2 s of audio in hand by playing very slightly faster or slower (resampling,
 * which also converts the page's sample rate). It fades in and out instead of clicking when the stream starts,
 * stops or runs dry. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../webin.h"

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
enum { STREAM_CAPTURE = 1, ACCESS_RW_INTERLEAVED = 3, ACCESS_RW_NONINTERLEAVED = 4 };
enum { FMT_S16_LE = 2, FMT_S24_LE = 6, FMT_S32_LE = 10, FMT_FLOAT_LE = 14 };

static void wlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void wlog(const char *fmt, ...) {
    FILE *f = fopen("/tmp/mpc-buttons.log", "a");
    if (!f) return;
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    fprintf(f, "%s [%d] web in: ", ts, (int)getpid());
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    __builtin_va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* the capture stream MPC reads its inputs from (only one is followed) */
static struct { snd_pcm_t *pcm; int channels, format, access; unsigned rate; } cap;
static struct webin_ring *ring;

/* reader state, audio thread only */
static uint32_t rd_pos, rd_stream;
static double rd_frac;
static int running;        /* playing the stream (else silent, waiting for enough audio) */
static float fade;         /* 0..1 */

static void ring_map(void) {
    if (ring) return;
    const int fd = open(WEBIN_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) { wlog("can't open %s", WEBIN_PATH); return; }
    if (ftruncate(fd, sizeof(struct webin_ring)) == 0) {
        void *m = mmap(NULL, sizeof(struct webin_ring), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) ring = m;
    }
    close(fd);
    if (ring && ring->magic != WEBIN_MAGIC) { memset(ring, 0, 64); ring->magic = WEBIN_MAGIC; ring->rate = 48000; ring->pair = 0; }
}

/* the page's audio for n frames at the MPC's rate, added into out (interleaved stereo float, zeroed by the caller);
 * returns 0 when there's nothing to add */
static int webin_render(float *out, int n) {
    struct webin_ring *r = ring;
    if (!r || r->magic != WEBIN_MAGIC || !cap.rate) return 0;
    const uint32_t w = __atomic_load_n(&r->write, __ATOMIC_ACQUIRE), stream = r->stream;
    const double src_rate = r->rate >= 8000 && r->rate <= 192000 ? r->rate : 48000;
    const double target = 0.2 * src_rate;   /* audio kept in hand */
    if (stream != rd_stream) { rd_stream = stream; running = 0; }
    int32_t avail = (int32_t)(w - rd_pos);
    if (!running) {
        if (avail < target) { if (fade <= 0) return 0; }   /* still fading out the last bit */
        else if (fade <= 0) { rd_pos = w - (uint32_t)target; rd_frac = 0; running = 1; avail = (int32_t)target; }
    }
    if (avail < 0 || avail > (int32_t)(WEBIN_FRAMES - 4096)) {   /* way off (the writer jumped): start over */
        running = 0;
        rd_pos = w;
        return fade > 0;
    }
    /* play a touch faster when ahead, slower when behind: at most 0.5% (inaudible) */
    double adj = running ? (avail - target) / target * 0.005 : 0;
    if (adj > 0.005) adj = 0.005;
    if (adj < -0.005) adj = -0.005;
    const double step = src_rate / cap.rate * (1 + adj);
    const float fstep = 1.0f / 256;   /* ~5 ms fades */
    for (int i = 0; i < n; i++) {
        const int32_t left = (int32_t)(w - rd_pos);
        if (running && left < 320) running = 0;   /* running dry: fade out on what's left, wait for more */
        if (running) { if (fade < 1) fade += fstep; }
        else if (fade > 0) fade -= fstep;
        if (fade <= 0) { fade = 0; if (!running) break; }
        if (left < 2) continue;
        const float *a = r->data + 2 * (rd_pos & (WEBIN_FRAMES - 1)), *b = r->data + 2 * ((rd_pos + 1) & (WEBIN_FRAMES - 1));
        const float t = (float)rd_frac;
        float L = a[0] + (b[0] - a[0]) * t, R = a[1] + (b[1] - a[1]) * t;
        if (!(L == L) || L > 1 || L < -1) L = 0;   /* only clean audio reaches MPC */
        if (!(R == R) || R > 1 || R < -1) R = 0;
        out[2 * i] += L * fade;
        out[2 * i + 1] += R * fade;
        rd_frac += step;
        const uint32_t whole = (uint32_t)rd_frac;
        rd_pos += whole;
        rd_frac -= whole;
    }
    return 1;
}

static float mixbuf[2 * 8192];

static inline int32_t clip32(double v, double lim) { return v > lim ? (int32_t)lim : v < -lim - 1 ? (int32_t)(-lim - 1) : (int32_t)lrint(v); }

/* add the stream to the chosen pair of channel c0/c0+1 of frame i (interleaved: base = whole buffer) */
static void add_sample(void *base, int frame, int ch, int stride, float v) {
    const int idx = frame * stride + ch;
    switch (cap.format) {
    case FMT_S16_LE: { int16_t *p = (int16_t *)base + idx; *p = (int16_t)clip32(*p + v * 32767.0, 32767); break; }
    case FMT_S32_LE: { int32_t *p = (int32_t *)base + idx; *p = clip32((double)*p + v * 2147483647.0, 2147483647.0); break; }
    case FMT_S24_LE: {
        int32_t *p = (int32_t *)base + idx;
        const int32_t s = (*p << 8) >> 8;   /* 24 bits in 32, sign-extended */
        *p = clip32(s + v * 8388607.0, 8388607.0);   /* kept sign-extended, as it came */
        break;
    }
    case FMT_FLOAT_LE: { float *p = (float *)base + idx; *p += v; if (*p > 1) *p = 1; if (*p < -1) *p = -1; break; }
    }
}

static void mix_into(snd_pcm_t *pcm, void *interleaved, void **planes, long frames) {
    if (pcm != cap.pcm || frames <= 0) return;
    const struct webin_ring *r = ring;
    if (!r) return;
    const int c0 = (int)r->pair * 2;
    if (c0 + 1 >= cap.channels) return;
    long done = 0;
    while (done < frames) {
        const int n = frames - done > 8192 ? 8192 : (int)(frames - done);
        memset(mixbuf, 0, sizeof(float) * 2 * n);
        if (!webin_render(mixbuf, n)) return;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < 2; k++) {
                if (interleaved) add_sample(interleaved, (int)done + i, c0 + k, cap.channels, mixbuf[2 * i + k]);
                else if (planes && planes[c0 + k]) add_sample(planes[c0 + k], (int)done + i, 0, 1, mixbuf[2 * i + k]);
            }
        done += n;
    }
}

/* ---- the hooks -------------------------------------------------------------------------------------------- */
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) {
    static int (*real)(snd_pcm_t *, snd_pcm_hw_params_t *);
    if (!real) real = (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *))dlsym(RTLD_NEXT, "snd_pcm_hw_params");
    if (!real) return -38;
    const int r = real(pcm, params);
    int (*stream)(snd_pcm_t *) = (int (*)(snd_pcm_t *))dlsym(RTLD_DEFAULT, "snd_pcm_stream");
    int (*get_ch)(const snd_pcm_hw_params_t *, unsigned *) =
        (int (*)(const snd_pcm_hw_params_t *, unsigned *))dlsym(RTLD_DEFAULT, "snd_pcm_hw_params_get_channels");
    int (*get_fmt)(const snd_pcm_hw_params_t *, int *) =
        (int (*)(const snd_pcm_hw_params_t *, int *))dlsym(RTLD_DEFAULT, "snd_pcm_hw_params_get_format");
    int (*get_acc)(const snd_pcm_hw_params_t *, int *) =
        (int (*)(const snd_pcm_hw_params_t *, int *))dlsym(RTLD_DEFAULT, "snd_pcm_hw_params_get_access");
    int (*get_rate)(const snd_pcm_hw_params_t *, unsigned *, int *) =
        (int (*)(const snd_pcm_hw_params_t *, unsigned *, int *))dlsym(RTLD_DEFAULT, "snd_pcm_hw_params_get_rate");
    const char *(*name)(snd_pcm_t *) = (const char *(*)(snd_pcm_t *))dlsym(RTLD_DEFAULT, "snd_pcm_name");
    if (r < 0 || !stream || !get_ch || !get_fmt || !get_acc || !get_rate || stream(pcm) != STREAM_CAPTURE) return r;
    unsigned ch = 0, rate = 0;
    int fmt = -1, acc = -1, dir = 0;
    get_ch(params, &ch);
    get_fmt(params, &fmt);
    get_acc(params, &acc);
    get_rate(params, &rate, &dir);
    const int ok = ch >= 2 && (fmt == FMT_S16_LE || fmt == FMT_S24_LE || fmt == FMT_S32_LE || fmt == FMT_FLOAT_LE) &&
                   (acc == ACCESS_RW_INTERLEAVED || acc == ACCESS_RW_NONINTERLEAVED);
    wlog("MPC's input %s: %u channels, format %d, access %d, %u Hz%s", name ? name(pcm) : "?", ch, fmt, acc, rate,
         ok ? "" : " (not a kind Web In can add to)");
    if (!ok) return r;
    ring_map();
    cap.pcm = NULL;   /* (re)configured: the audio thread isn't reading it right now */
    cap.channels = (int)ch; cap.format = fmt; cap.access = acc; cap.rate = rate;
    cap.pcm = pcm;
    FILE *f = fopen(WEBIN_STATUS ".new", "w");
    if (f) {
        fprintf(f, "channels=%u rate=%u pid=%d\n", ch, rate, (int)getpid());
        fclose(f);
        rename(WEBIN_STATUS ".new", WEBIN_STATUS);
    }
    return r;
}

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t size) {
    static snd_pcm_sframes_t (*real)(snd_pcm_t *, void *, snd_pcm_uframes_t);
    if (!real) real = (snd_pcm_sframes_t (*)(snd_pcm_t *, void *, snd_pcm_uframes_t))dlsym(RTLD_NEXT, "snd_pcm_readi");
    if (!real) return -38;
    const snd_pcm_sframes_t r = real(pcm, buf, size);
    if (r > 0 && pcm == cap.pcm && cap.access == ACCESS_RW_INTERLEAVED) mix_into(pcm, buf, NULL, r);
    return r;
}

snd_pcm_sframes_t snd_pcm_readn(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t size) {
    static snd_pcm_sframes_t (*real)(snd_pcm_t *, void **, snd_pcm_uframes_t);
    if (!real) real = (snd_pcm_sframes_t (*)(snd_pcm_t *, void **, snd_pcm_uframes_t))dlsym(RTLD_NEXT, "snd_pcm_readn");
    if (!real) return -38;
    const snd_pcm_sframes_t r = real(pcm, bufs, size);
    if (r > 0 && pcm == cap.pcm && cap.access == ACCESS_RW_NONINTERLEAVED) mix_into(pcm, NULL, bufs, r);
    return r;
}

int snd_pcm_close(snd_pcm_t *pcm) {
    static int (*real)(snd_pcm_t *);
    if (!real) real = (int (*)(snd_pcm_t *))dlsym(RTLD_NEXT, "snd_pcm_close");
    if (!real) return -38;
    if (pcm == cap.pcm) cap.pcm = NULL;
    return real(pcm);
}
