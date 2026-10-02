/* mpc-buttons.so: loaded into MPC (LD_PRELOAD, added by mpc-launch) so the browser remote can press MPC's buttons.
 *
 * MPC reads its own panel (buttons, pads, knobs) as MIDI from the "private" port of its internal controller, by
 * opening the ALSA raw MIDI device hw:<card>,0,1. Here MPC gets an ALSA "virtual" raw MIDI input in its place, fed by
 * the ALSA sequencer: the controller's private port is connected to it, so MPC receives everything exactly as before,
 * and mpc-remote can send button presses to the same input. MPC's output to the controller (its lights) still goes
 * straight to the real device. Hakai's own driver does the same thing. If any step fails, MPC simply gets the real
 * device, as without this library.
 *
 * Where to send: /tmp/mpc-buttons ("virt=<client>:<port> hw=<client>:<port> pid=<MPC's pid>"), what happened:
 * /tmp/mpc-buttons.log. The virtual input is hidden from MPC's own list of MIDI devices, so MPC can't be set to send
 * MIDI into its own panel input. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct _snd_rawmidi snd_rawmidi_t;
typedef struct _snd_seq snd_seq_t;
typedef struct _snd_seq_client_info snd_seq_client_info_t;
typedef struct _snd_seq_port_info snd_seq_port_info_t;
typedef struct _snd_seq_port_subscribe snd_seq_port_subscribe_t;
typedef struct { unsigned char client, port; } snd_seq_addr_t;
/* the parts of alsa-lib used here, looked up when first needed: MPC links libasound, but this library is also loaded
 * into the programs the launcher runs on the way to MPC (systemd-inhibit), which don't */
static int (*p_snd_rawmidi_close)(snd_rawmidi_t *);
static int (*p_snd_seq_open)(snd_seq_t **, const char *, int, int);
static int (*p_snd_seq_close)(snd_seq_t *);
static int (*p_snd_seq_client_info_malloc)(snd_seq_client_info_t **);
static void (*p_snd_seq_client_info_free)(snd_seq_client_info_t *);
static void (*p_snd_seq_client_info_set_client)(snd_seq_client_info_t *, int);
static int (*p_snd_seq_client_info_get_client)(const snd_seq_client_info_t *);
static int (*p_snd_seq_client_info_get_card)(const snd_seq_client_info_t *);
static int (*p_snd_seq_client_info_get_pid)(const snd_seq_client_info_t *);
static const char * (*p_snd_seq_client_info_get_name)(snd_seq_client_info_t *);
static int (*p_snd_seq_port_info_malloc)(snd_seq_port_info_t **);
static void (*p_snd_seq_port_info_free)(snd_seq_port_info_t *);
static void (*p_snd_seq_port_info_set_client)(snd_seq_port_info_t *, int);
static void (*p_snd_seq_port_info_set_port)(snd_seq_port_info_t *, int);
static int (*p_snd_seq_port_info_get_port)(const snd_seq_port_info_t *);
static const char * (*p_snd_seq_port_info_get_name)(const snd_seq_port_info_t *);
static int (*p_snd_seq_get_any_port_info)(snd_seq_t *, int, int, snd_seq_port_info_t *);
static int (*p_snd_seq_query_next_port)(snd_seq_t *, snd_seq_port_info_t *);
static int (*p_snd_seq_port_subscribe_malloc)(snd_seq_port_subscribe_t **);
static void (*p_snd_seq_port_subscribe_free)(snd_seq_port_subscribe_t *);
static void (*p_snd_seq_port_subscribe_set_sender)(snd_seq_port_subscribe_t *, const snd_seq_addr_t *);
static void (*p_snd_seq_port_subscribe_set_dest)(snd_seq_port_subscribe_t *, const snd_seq_addr_t *);
static int (*p_snd_seq_subscribe_port)(snd_seq_t *, snd_seq_port_subscribe_t *);
static int alsa_ok(void) {
    static int ok = -1;
    if (ok >= 0) return ok;
    ok = 1;
#define SYM(n) if (!(*(void **)&p_##n = dlsym(RTLD_DEFAULT, #n))) ok = 0;
    SYM(snd_rawmidi_close)
    SYM(snd_seq_open)
    SYM(snd_seq_close)
    SYM(snd_seq_client_info_malloc)
    SYM(snd_seq_client_info_free)
    SYM(snd_seq_client_info_set_client)
    SYM(snd_seq_client_info_get_client)
    SYM(snd_seq_client_info_get_card)
    SYM(snd_seq_client_info_get_pid)
    SYM(snd_seq_client_info_get_name)
    SYM(snd_seq_port_info_malloc)
    SYM(snd_seq_port_info_free)
    SYM(snd_seq_port_info_set_client)
    SYM(snd_seq_port_info_set_port)
    SYM(snd_seq_port_info_get_port)
    SYM(snd_seq_port_info_get_name)
    SYM(snd_seq_get_any_port_info)
    SYM(snd_seq_query_next_port)
    SYM(snd_seq_port_subscribe_malloc)
    SYM(snd_seq_port_subscribe_free)
    SYM(snd_seq_port_subscribe_set_sender)
    SYM(snd_seq_port_subscribe_set_dest)
    SYM(snd_seq_subscribe_port)
#undef SYM
    return ok;
}
#define SND_SEQ_OPEN_DUPLEX 3

#define STATUS "/tmp/mpc-buttons"
#define LOGFILE "/tmp/mpc-buttons.log"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int virt_client = -1;   /* hidden from MPC's list of MIDI devices */

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOGFILE, "a");
    if (!f) return;
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    fprintf(f, "%s [%d] ", ts, (int)getpid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static int (*real_open(void))(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int) {
    return (int (*)(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int))dlsym(RTLD_NEXT, "snd_rawmidi_open");
}

static int read_line(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    const int ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

/* the MPC's internal controller: an Akai (USB vendor 09e8) sound card, or one named MPC */
static int is_mpc_card(int card) {
    char p[64], b[64];
    snprintf(p, sizeof p, "/proc/asound/card%d/usbid", card);
    if (!read_line(p, b, sizeof b) && !strncmp(b, "09e8:", 5)) return 1;
    snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
    return !read_line(p, b, sizeof b) && strstr(b, "MPC") != NULL;
}

/* seq client <-> port helpers; *found gets the client, the port name is checked with want (NULL: any) */
static int find_client(snd_seq_t *seq, int card, int pid, const char *port_name, const unsigned char *skip,
                       snd_seq_addr_t *out, int want_port) {
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    int found = -1;
    if (p_snd_seq_client_info_malloc(&ci)) return -1;
    if (p_snd_seq_port_info_malloc(&pi)) { p_snd_seq_client_info_free(ci); return -1; }
    int (*next_client)(snd_seq_t *, snd_seq_client_info_t *) =
        (int (*)(snd_seq_t *, snd_seq_client_info_t *))dlsym(RTLD_NEXT, "snd_seq_query_next_client");
    p_snd_seq_client_info_set_client(ci, -1);
    while (found < 0 && next_client && next_client(seq, ci) >= 0) {
        const int c = p_snd_seq_client_info_get_client(ci);
        if (skip && skip[c]) continue;
        if (card >= 0 && p_snd_seq_client_info_get_card(ci) != card) continue;
        if (pid > 0) {
            const int p = p_snd_seq_client_info_get_pid(ci);
            if (p > 0 && p != pid) continue;
        }
        if (want_port >= 0) {
            if (p_snd_seq_get_any_port_info(seq, c, want_port, pi) < 0) continue;
            out->client = (unsigned char)c;
            out->port = (unsigned char)want_port;
            logf_("controller's private port: %d:%d \"%s\" (client \"%s\")", c, want_port,
                  p_snd_seq_port_info_get_name(pi), p_snd_seq_client_info_get_name(ci));
            found = c;
            break;
        }
        p_snd_seq_port_info_set_client(pi, c);
        p_snd_seq_port_info_set_port(pi, -1);
        while (p_snd_seq_query_next_port(seq, pi) >= 0) {
            if (port_name && strcmp(p_snd_seq_port_info_get_name(pi), port_name)) continue;
            out->client = (unsigned char)c;
            out->port = (unsigned char)p_snd_seq_port_info_get_port(pi);
            found = c;
            break;
        }
    }
    p_snd_seq_port_info_free(pi);
    p_snd_seq_client_info_free(ci);
    return found;
}

/* give MPC a virtual input fed from the controller's private port; 0 on success (*in set), else nothing changed */
static int redirect(int card, snd_rawmidi_t **in, int mode) {
    snd_seq_t *seq;
    if (p_snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) { logf_("can't open the ALSA sequencer"); return -1; }
    snd_seq_addr_t hw, virt;
    unsigned char before[256] = {0};
    int r = -1;
    if (find_client(seq, card, 0, NULL, NULL, &hw, 1) < 0) { logf_("no sequencer port for card %d's private port", card); goto out; }
    { /* the clients that exist now, to tell the new virtual one apart */
        snd_seq_client_info_t *ci;
        int (*next_client)(snd_seq_t *, snd_seq_client_info_t *) =
            (int (*)(snd_seq_t *, snd_seq_client_info_t *))dlsym(RTLD_NEXT, "snd_seq_query_next_client");
        if (!next_client || p_snd_seq_client_info_malloc(&ci)) goto out;
        p_snd_seq_client_info_set_client(ci, -1);
        while (next_client(seq, ci) >= 0) before[p_snd_seq_client_info_get_client(ci) & 255] = 1;
        p_snd_seq_client_info_free(ci);
    }
    /* MERGE=0: every message with its status byte. The default sends "running status" (a repeated status byte left
     * out, e.g. a button's release as just "7b 00"), which MPC's panel reader doesn't understand. */
    if (real_open()(in, NULL, "virtual:MERGE=0", mode) < 0) { logf_("can't open a virtual MIDI input"); *in = NULL; goto out; }
    if (find_client(seq, -1, (int)getpid(), "Virtual RawMIDI", before, &virt, -1) < 0) {
        logf_("can't find the virtual input's sequencer port");
        goto fail;
    }
    snd_seq_port_subscribe_t *sub;
    if (p_snd_seq_port_subscribe_malloc(&sub)) goto fail;
    p_snd_seq_port_subscribe_set_sender(sub, &hw);
    p_snd_seq_port_subscribe_set_dest(sub, &virt);
    const int e = p_snd_seq_subscribe_port(seq, sub);
    p_snd_seq_port_subscribe_free(sub);
    if (e < 0) { logf_("can't connect %d:%d to %d:%d (%d)", hw.client, hw.port, virt.client, virt.port, e); goto fail; }
    virt_client = virt.client;
    FILE *f = fopen(STATUS ".new", "w");
    if (f) {
        fprintf(f, "virt=%d:%d hw=%d:%d pid=%d\n", virt.client, virt.port, hw.client, hw.port, (int)getpid());
        fclose(f);
        rename(STATUS ".new", STATUS);
    }
    logf_("MPC's panel input now comes through %d:%d (from %d:%d): remote buttons ready", virt.client, virt.port,
          hw.client, hw.port);
    r = 0;
    goto out;
fail:
    p_snd_rawmidi_close(*in);
    *in = NULL;
out:
    p_snd_seq_close(seq);
    return r;
}

int snd_rawmidi_open(snd_rawmidi_t **in, snd_rawmidi_t **out, const char *name, int mode) {
    int (*real)(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int) = real_open();
    if (!real) return -38;   /* -ENOSYS */
    int card, dev, sub, n = 0;
    if (in && name) logf_("raw MIDI input opened: %s", name);   /* (for /log, if MPC ever names it differently) */
    if (!in || !name || sscanf(name, "hw:%d,%d,%d%n", &card, &dev, &sub, &n) != 3 || name[n] || dev || sub != 1 ||
        !is_mpc_card(card) || !alsa_ok())
        return real(in, out, name, mode);
    pthread_mutex_lock(&lock);
    logf_("that's the MPC controller's private port%s", out ? " (output stays the real device)" : "");
    int r = -1;
    if (out) {   /* its output (the lights) stays the real device */
        r = real(NULL, out, name, mode);
        if (r < 0) { pthread_mutex_unlock(&lock); return r; }
    }
    if (redirect(card, in, mode) == 0) { pthread_mutex_unlock(&lock); return 0; }
    logf_("using the real device instead");
    r = real(in, NULL, name, mode);
    if (r < 0 && out) { p_snd_rawmidi_close(*out); *out = NULL; }
    pthread_mutex_unlock(&lock);
    return r;
}

/* hide the virtual input (and the remote's own client) from MPC's list of MIDI devices */
int snd_seq_query_next_client(snd_seq_t *seq, snd_seq_client_info_t *info) {
    static int (*real)(snd_seq_t *, snd_seq_client_info_t *);
    if (!real) real = (int (*)(snd_seq_t *, snd_seq_client_info_t *))dlsym(RTLD_NEXT, "snd_seq_query_next_client");
    if (!real) return -38;
    int r;
    if (virt_client < 0 && !alsa_ok()) return real(seq, info);
    while ((r = real(seq, info)) >= 0) {
        const int c = p_snd_seq_client_info_get_client(info);
        const char *nm = p_snd_seq_client_info_get_name(info);
        if (c == virt_client || (nm && !strcmp(nm, "MPC Remote"))) continue;
        break;
    }
    return r;
}
