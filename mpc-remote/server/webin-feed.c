/* webin-feed <rate>: 16-bit stereo audio on stdin -> Web In (the ring in ../webin.h), at real-time pace.
 * The MPC browser's sound comes this way: its ALSA output is a "file" device piping raw audio here. ALSA's null
 * device behind it never waits, so the pace comes from here: reading stdin only as fast as the audio plays (plus a
 * little in hand), the pipe fills up and the browser waits for it. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "../webin.h"

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    const int rate = argc > 1 ? atoi(argv[1]) : 48000;
    if (rate < 8000 || rate > 192000) return 2;
    const int fd = open(WEBIN_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0 || ftruncate(fd, sizeof(struct webin_ring))) return 1;
    struct webin_ring *r = mmap(NULL, sizeof *r, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (r == MAP_FAILED) return 1;
    if (r->magic != WEBIN_MAGIC) { memset(r, 0, 64); r->magic = WEBIN_MAGIC; }
    r->rate = (uint32_t)rate;
    __atomic_add_fetch(&r->stream, 1, __ATOMIC_RELEASE);

    static uint8_t buf[4096 * 4];
    const double lead = 0.3;   /* seconds of audio allowed ahead of real time */
    double t0 = now();
    uint64_t sent = 0;
    for (;;) {
        /* wait until real time has caught up to within the lead */
        double ahead = (double)sent / rate - (now() - t0);
        if (ahead > lead) {
            const double s = ahead - lead;
            struct timespec ts = {(time_t)s, (long)((s - (time_t)s) * 1e9)};
            nanosleep(&ts, NULL);
        } else if (ahead < -0.5) {   /* the browser paused (or started late): don't rush to catch up */
            t0 = now() - (double)sent / rate;
        }
        const ssize_t n = read(0, buf, sizeof buf);
        if (n <= 0) break;
        const uint32_t frames = (uint32_t)(n / 4), w = r->write;
        for (uint32_t i = 0; i < frames; i++) {
            const int16_t L = (int16_t)(buf[4 * i] | buf[4 * i + 1] << 8), R = (int16_t)(buf[4 * i + 2] | buf[4 * i + 3] << 8);
            float *d = r->data + 2 * ((w + i) & (WEBIN_FRAMES - 1));
            d[0] = L / 32768.0f;
            d[1] = R / 32768.0f;
        }
        __atomic_store_n(&r->write, w + frames, __ATOMIC_RELEASE);
        sent += frames;
        /* (an odd byte count would shift the channels: the pipe from ALSA always carries whole frames) */
    }
    return 0;
}
