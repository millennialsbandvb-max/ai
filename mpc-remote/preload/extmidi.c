/* extmidi.c (in mpc-buttons.so): MIDI from your controllers for our effect plugins. MPC doesn't pass MIDI on to
 * insert effects, so this copies what MPC itself reads from MIDI inputs (5-pin or USB, through ALSA raw MIDI or the
 * sequencer) into a ring that the plugins read, from the same process: they find mpc_ext_midi_read by dlsym
 * (tools/picklist.h) and take the messages on their MIDI Ch. Only Control Change and Program Change are kept, never
 * the MPC's own panel (its private port). MPC still gets everything as before: this only looks.
 * CC 89-92 (any channel, going to 64 or more) also ask mpc-remote to do its recorded shortcut 1-4 (MPC's Tuner, say),
 * through its FIFO /tmp/mpc-shortcut. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

typedef struct _snd_rawmidi snd_rawmidi_t;
typedef struct _snd_seq snd_seq_t;

#define RING 256
static unsigned char ring[RING][3];
static unsigned head;   /* messages written so far (the ring keeps the last RING) */

static void shortcut(unsigned char d1, unsigned char d2) {
    static unsigned char high[4];
    const int k = d1 - 89;
    if (k < 0 || k > 3) return;
    const int on = d2 >= 64;
    if (on && !high[k]) {
        const int fd = open("/tmp/mpc-shortcut", O_WRONLY | O_NONBLOCK | O_CLOEXEC);   /* (no mpc-remote: nothing) */
        if (fd >= 0) { const char c = (char)('1' + k); if (write(fd, &c, 1) < 0) {} close(fd); }
    }
    high[k] = (unsigned char)on;
}

static void put(unsigned char st, unsigned char d1, unsigned char d2) {
    if ((st & 0xf0) == 0xb0) shortcut(d1, d2);
    const unsigned at = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
    ring[at % RING][0] = st; ring[at % RING][1] = d1; ring[at % RING][2] = d2;
    __atomic_store_n(&head, at + 1, __ATOMIC_RELEASE);   /* (MPC reads its inputs from one thread at a time) */
}

/* the plugins' side: *pos = UINT32_MAX first (starts at now); 1 = a message in msg, 0 = none left */
__attribute__((visibility("default"))) int mpc_ext_midi_read(unsigned *pos, unsigned char msg[3]) {
    const unsigned h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
    if (*pos == 0xffffffffu) { *pos = h; return 0; }
    if (*pos == h) return 0;
    if (h - *pos > RING) *pos = h - RING;   /* (fell behind: the oldest kept) */
    memcpy(msg, ring[*pos % RING], 3);
    (*pos)++;
    return 1;
}

/* the panel's input (mpc-buttons.c tells us), never copied */
static snd_rawmidi_t *panel[4];
void extmidi_panel(snd_rawmidi_t *in) {
    for (int i = 0; i < 4; i++) if (!panel[i] || panel[i] == in) { panel[i] = in; return; }
}
static int panel_client = -1, panel_port = -1;
void extmidi_panel_port(int client, int port) { panel_client = client; panel_port = port; }

/* raw MIDI bytes -> messages, per input (running status kept) */
typedef struct { snd_rawmidi_t *h; unsigned char st, d[2]; int n; } Parser;
static Parser parsers[8];
static void parse(snd_rawmidi_t *h, const unsigned char *b, long n) {
    Parser *p = NULL;
    for (int i = 0; i < 8 && !p; i++) if (parsers[i].h == h) p = &parsers[i];
    for (int i = 0; i < 8 && !p; i++) if (!parsers[i].h) { p = &parsers[i]; p->h = h; }
    if (!p) return;
    for (long i = 0; i < n; i++) {
        const unsigned char c = b[i];
        if (c >= 0xf8) continue;                         /* clock and the like, anywhere */
        if (c & 0x80) { p->st = c < 0xf0 ? c : 0; p->n = 0; continue; }   /* (sysex and common: dropped) */
        if (!p->st) continue;
        p->d[p->n++] = c;
        const int type = p->st & 0xf0, need = type == 0xc0 || type == 0xd0 ? 1 : 2;
        if (p->n < need) continue;
        if (type == 0xb0) put(p->st, p->d[0], p->d[1]);
        else if (type == 0xc0) put(p->st, p->d[0], 0);
        p->n = 0;
    }
}

ssize_t snd_rawmidi_read(snd_rawmidi_t *h, void *buf, size_t size) {
    static ssize_t (*real)(snd_rawmidi_t *, void *, size_t);
    if (!real) real = (ssize_t (*)(snd_rawmidi_t *, void *, size_t))dlsym(RTLD_NEXT, "snd_rawmidi_read");
    if (!real) return -38;
    const ssize_t r = real(h, buf, size);
    if (r > 0) {
        int mine = 0;
        for (int i = 0; i < 4; i++) if (panel[i] == h) mine = 1;
        if (!mine) parse(h, (const unsigned char *)buf, (long)r);
    }
    return r;
}

/* the sequencer: snd_seq_event_t's controller/program events (type 10/11: data.control = channel, param, value) */
int snd_seq_event_input(snd_seq_t *seq, void **ev) {
    static int (*real)(snd_seq_t *, void **);
    if (!real) real = (int (*)(snd_seq_t *, void **))dlsym(RTLD_NEXT, "snd_seq_event_input");
    if (!real) return -38;
    const int r = real(seq, ev);
    if (r >= 0 && ev && *ev) {
        const unsigned char *e = (const unsigned char *)*ev;
        const int type = e[0], client = e[12], port = e[13];
        if ((type == 10 || type == 11) && !(client == panel_client && port == panel_port)) {
            unsigned char ch = e[16];
            uint32_t param;
            int32_t value;
            memcpy(&param, e + 20, 4);
            memcpy(&value, e + 24, 4);
            if (type == 10) put((unsigned char)(0xb0 | (ch & 15)), (unsigned char)(param & 127), (unsigned char)(value & 127));
            else put((unsigned char)(0xc0 | (ch & 15)), (unsigned char)(value & 127), 0);
        }
    }
    return r;
}
