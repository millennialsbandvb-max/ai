/* Web In: audio streamed from the remote page into one of MPC's inputs.
 * mpc-remote (the writer) puts the page's audio into this ring in shared memory; mpc-buttons.so, inside MPC (the
 * reader), adds it to the input pair the page chose as MPC reads its inputs. Either side creates the file. */
#ifndef MPC_WEBIN_H
#define MPC_WEBIN_H
#include <stdint.h>

#define WEBIN_PATH "/dev/shm/mpc-webin"
#define WEBIN_STATUS "/tmp/mpc-webin"   /* written by the reader: what MPC's input looks like */
#define WEBIN_MAGIC 0x4e494257u         /* "WBIN" */
#define WEBIN_FRAMES 65536u             /* ring size in stereo frames (power of two): 1.4 s at 48 kHz */

struct webin_ring {
    uint32_t magic;
    uint32_t rate;      /* the page's sample rate */
    uint32_t pair;      /* the input pair it goes to: 0 = inputs 1/2, 1 = 3/4, ... */
    uint32_t stream;    /* changes on every new start: the reader starts over */
    uint32_t write;     /* frames written so far (wraps); written last, with release order */
    uint32_t pad[11];
    float data[WEBIN_FRAMES * 2];   /* interleaved L R */
};
#endif
