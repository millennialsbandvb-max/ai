/* picklist.h: full-page lists for a plugin's steppers (presets, programs, models, ...), so tapping a stepper's name
 * opens a list of its entries instead of MPC's round knob menu (the page side: tools/page_lists.py).
 *
 * The plugin keeps its own parameters; pl_wrap puts an AEffect in front of it that passes everything through and
 * adds these after the plugin's own (VST index = the plugin's numParams + n):
 *   <key>__list  one per list (the n of PickLists), 1 while that list is open (opening one closes the others)
 *   pick_1..pick_24  the rows: the text is entry page*24 + r; a tap (a Toggle Switch: the value goes to 1) picks
 *                it and closes the list. Rows rest at 0; when their text changes they are reported as a tiny
 *                value and then 0 again, so MPC repaints them (a project restoring them never picks anything).
 *   pick_prev, pick_next  pages (momentary);  pick_close  closes the list (momentary);  pick_title  read-only
 * The plugin answers one function: PL_COUNT (entries), PL_NAME (entry i's name into out), PL_CURRENT (the entry
 * in use, or -1) and PL_CHOOSE (take entry i), for list number `list` (its order in PickLists).
 * Engines hosted inside another plugin (Quad Rig) are built with -DPICKLIST_OFF and stay unwrapped.
 * The plugin's own host calls (automation, display updates) go out under the wrapper's AEffect, which is the one
 * MPC knows.
 * With PickLists.midi set, MIDI from the MPC's MIDI inputs too: MPC doesn't pass MIDI on to insert effects, so our
 * MPC add-on (mpc-remote/preload/extmidi.c) copies controllers' Control and Program Changes into a ring; the wrapper
 * reads it every block and hands the plugin those on its MIDI Ch (effProcessEvents, as a host would). Then also:
 *   midi_ch (Omni, Ch 1..16), midi_ch_prev, midi_ch_next: a stepper, with its own list (after the plugin's lists).
 * It's saved with the plugin's other parameters, or for a plugin that saves a chunk, at the chunk's end
 * ("PLWMIDI <ch>", 16 bytes, taken off again before the plugin sees its chunk). */
#pragma once
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../nam-mpc/src/vst2.h"

#define PL_ROWS 24
#define PL_MAX 128
/* PL_COUNT < 0: no list there now (a knob that isn't a choice): it doesn't open. PL_HEADING: a title for now into out
 * (left empty: the PickLists one). */
enum { PL_COUNT, PL_NAME, PL_CURRENT, PL_CHOOSE, PL_HEADING };
typedef int (*pl_fn)(AEffect *e, int list, int op, int i, char *out, int max);
typedef struct {
    int n;
    const char *const *keys;     /* the steppers' parameter keys: "<key>__list" */
    const char *const *titles;   /* shown over the list */
    pl_fn fn;
    int midi;                    /* 1: MIDI from the MPC's inputs, on a MIDI Ch of ours */
} PickLists;

enum { PL_PREV = PL_ROWS, PL_NEXT, PL_CLOSE, PL_TITLE, PL_FIXED,   /* after the open flags: rows, then these */
       PL_MCH = PL_FIXED, PL_MCH_PREV, PL_MCH_NEXT, PL_FIXED_MIDI };   /* (and these with .midi) */
#define PL_TRAILER 16   /* "PLWMIDI NN" + padding, at the end of a chunk */

/* the MPC add-on's ring (extmidi.c), if it's loaded: found by the dynamic linker (weak: NULL without it) */
extern int mpc_ext_midi_read(unsigned *pos, unsigned char msg[3]) __attribute__((weak, visibility("default")));

typedef struct {
    AEffect fx;                  /* what MPC sees */
    AEffect *in;                 /* the plugin */
    audioMasterCallback master;
    const PickLists *pl;
    int base, open, page, nl;    /* nl: open flags (the plugin's lists, + the MIDI Ch one) */
    float norm[PL_MAX];
    volatile unsigned char report[PL_MAX];
    int ch;                      /* MIDI Ch: 0 = Omni, 1..16 */
    unsigned midi_pos;
    char *chunk;                 /* the plugin's chunk + our trailer */
    int *arrow;                  /* per plugin param: 0, or a "< " / " >" stepper arrow: +-(its partner's index + 1) */
    unsigned char *rest;         /* an arrow to report back at rest */
    float *alast;                /* per arrow (plugin's, then ours): the value the host last sent */
    unsigned char *rsent;        /* rest reported to the host since (it may or may not have taken it) */
} PickWrap;
/* The < > arrows and the data wheel: the page points the wheel at an arrow when it's touched (Q-Link). An arrow
 * rests at 0.45, just under the middle: the page's toggle takes it to 1 on a tap (from 0.5 it went to 0, and taps
 * were lost), and the wheel can move it either way: up steps on, down steps back, whichever arrow it is. 0 (a
 * button's release, or an old project's saved value) does nothing. After each, it goes back to rest (the host takes
 * the values we report: the list rows rest the same way). */
#define PL_REST 0.45f
/* a new value for arrow k: +1 / -1 a step that way (the wheel), 2 the arrow's own press, 0 nothing. The wheel's
 * direction is against the value before: rest, or (if the host hasn't taken rest yet) the last one it sent */
static int pl_arrow(PickWrap *w, int k, float v) {
    if (!w->alast || !w->rsent) return v >= 0.999f ? 2 : 0;
    float last = w->alast[k];
    if (w->rsent[k]) { if (fabsf(v - PL_REST) <= fabsf(v - last)) last = PL_REST; w->rsent[k] = 0; }
    int r = 0;
    if (v >= 0.999f) r = 2;
    else if (v > 0.001f && fabsf(v - last) > 0.0001f) r = v > last ? 1 : -1;
    w->alast[k] = v;
    w->rest[k] = 1;
    return r;
}

static PickWrap *pl_all[64];
static audioMasterCallback pl_real_master;

static int pl_nparams(const PickLists *pl) { return pl->n + (pl->midi ? 1 + PL_FIXED_MIDI : PL_FIXED); }
static void pl_copy(char *dst, const char *src, int max) { snprintf(dst, (size_t)max, "%s", src); }

/* the plugin's host calls, sent on as the wrapper's */
static void pl_sync(PickWrap *w) {
    w->fx.numPrograms = w->in->numPrograms;
    w->fx.numInputs = w->in->numInputs;
    w->fx.numOutputs = w->in->numOutputs;
    w->fx.flags = w->in->flags;
    w->fx.initialDelay = w->in->initialDelay;
    w->fx.uniqueID = w->in->uniqueID;
    w->fx.version = w->in->version;
}
static intptr_t pl_host(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    for (int i = 0; i < 64; i++) {
        PickWrap *w = pl_all[i];
        if (w && w->in == e) {
            pl_sync(w);
            if (op == audioMasterAutomate && w->arrow && w->rsent && idx >= 0 && idx < w->base && w->arrow[idx]) {   /* (its own reports: rest) */
                opt = PL_REST;
                w->rsent[idx] = 1;
            }
            return w->master ? w->master(&w->fx, op, idx, val, p, opt) : 0;
        }
    }
    return pl_real_master ? pl_real_master(e, op, idx, val, p, opt) : 0;   /* (while the plugin is being made) */
}

/* a list's answers: the plugin's, or ours for the MIDI Ch list (the one after the plugin's) */
static void pl_set_ch(PickWrap *w, int ch);
static int pl_fn_of(PickWrap *w, int list, int op, int i, char *out, int max) {
    if (list < w->pl->n) return w->pl->fn(w->in, list, op, i, out, max);
    if (op == PL_COUNT) return 17;
    if (op == PL_CURRENT) return w->ch;
    if (op == PL_HEADING) { pl_copy(out, "MIDI Channel", max); return 0; }
    if (op == PL_NAME) { if (i) snprintf(out, (size_t)max, "Ch %d", i); else pl_copy(out, "Omni", max); }
    if (op == PL_CHOOSE && i >= 0 && i <= 16) pl_set_ch(w, i);
    return 0;
}
static int pl_count(PickWrap *w) {
    const int n = w->open < 0 ? 0 : pl_fn_of(w, w->open, PL_COUNT, 0, NULL, 0);
    return n > 0 ? n : 0;
}
static int pl_pages(PickWrap *w) { const int n = pl_count(w); return n > 0 ? (n + PL_ROWS - 1) / PL_ROWS : 1; }
static void pl_set(PickWrap *w, int j, float v) { w->norm[j] = v; w->report[j] = 1; }
static void pl_rows_changed(PickWrap *w) {   /* new text: reported as a change, so MPC repaints the rows */
    for (int r = 0; r < PL_ROWS; r++) { w->norm[w->nl + r] = 0; w->report[w->nl + r] = 2; }
    w->report[w->nl + PL_TITLE] = 2;
}
static void pl_set_ch(PickWrap *w, int ch) { w->ch = ch; pl_set(w, w->nl + PL_MCH, (float)ch / 16); }
static void pl_close(PickWrap *w) {
    if (w->open >= 0) pl_set(w, w->open, 0);
    w->open = -1;
    pl_rows_changed(w);
}

static void pl_setParameter(AEffect *e, int32_t i, float v) {
    PickWrap *w = (PickWrap *)e->object;
    if (i < w->base) {
        const int a = w->arrow ? w->arrow[i] : 0;
        if (a) {   /* an arrow: its press, or the wheel's step either way; then back to rest */
            const int r = pl_arrow(w, i, v), prev = a < 0, partner = (a < 0 ? -a : a) - 1;   /* (a < 0: this one is "<") */
            if (r) {
                const int target = r == 2 ? i : (r > 0) == prev ? partner : i;
                w->in->setParameter(w->in, target, 1);
                w->in->setParameter(w->in, target, 0);
            }
            return;
        }
        w->in->setParameter(w->in, i, v);
        return;
    }
    const int j = i - w->base, n = w->nl;
    if (j >= pl_nparams(w->pl)) return;
    const int press = v > 0.5f;
    if (j < n) {   /* a list's open flag (the stepper's field toggles it) */
        if (press && pl_fn_of(w, j, PL_COUNT, 0, NULL, 0) < 0) { pl_set(w, j, 0); return; }   /* (nothing to list) */
        if (press) {
            if (w->open >= 0 && w->open != j) pl_set(w, w->open, 0);
            w->open = j;
            w->norm[j] = 1;
            const int cur = pl_fn_of(w, j, PL_CURRENT, 0, NULL, 0);
            w->page = cur > 0 ? cur / PL_ROWS : 0;
            pl_rows_changed(w);
        } else if (w->open == j) {
            w->norm[j] = 0;
            w->open = -1;
            pl_rows_changed(w);
        } else w->norm[j] = 0;
        return;
    }
    const int r = j - n;
    if (r < PL_ROWS) {
        if (press && w->open >= 0) {
            const int list = w->open, idx = w->page * PL_ROWS + r;
            if (idx < pl_count(w)) {
                pl_close(w);
                pl_fn_of(w, list, PL_CHOOSE, idx, NULL, 0);
                return;
            }
        }
        pl_set(w, j, 0);   /* back to rest */
        return;
    }
    if (r == PL_PREV || r == PL_NEXT) {
        if (press && w->open >= 0) {
            const int np = pl_pages(w);
            w->page = (w->page + (r == PL_NEXT ? 1 : np - 1)) % np;
            pl_rows_changed(w);
        }
        pl_set(w, j, 0);
        return;
    }
    if (r == PL_CLOSE) {
        if (press) pl_close(w);
        pl_set(w, j, 0);
        return;
    }
    if (r == PL_MCH) {   /* on an entry: that one; between (a Q-Link nudge): one step */
        const float pos = v * 16;
        int ch = fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - (float)w->ch) < 1 ? w->ch + (pos > (float)w->ch ? 1 : -1) : (int)lrintf(pos);
        pl_set_ch(w, ch < 0 ? 0 : ch > 16 ? 16 : ch);
        return;
    }
    if (r == PL_MCH_PREV || r == PL_MCH_NEXT) {   /* (like the plugin's arrows) */
        const int s = pl_arrow(w, w->base + j, v), d = s == 2 ? (r == PL_MCH_NEXT ? 1 : -1) : s;
        w->rest[w->base + j] = 0;
        if (d) pl_set_ch(w, (w->ch + (d > 0 ? 1 : 16)) % 17);
        pl_set(w, j, PL_REST);
    }
}
static float pl_getParameter(AEffect *e, int32_t i) {
    PickWrap *w = (PickWrap *)e->object;
    if (i < w->base) {
        if (w->arrow && w->arrow[i] && w->alast) return w->rsent[i] ? PL_REST : w->alast[i];   /* (an arrow: as the host has it) */
        return w->in->getParameter(w->in, i);
    }
    return i - w->base < pl_nparams(w->pl) ? w->norm[i - w->base] : 0;
}

static void pl_name(PickWrap *w, int j, char *t, int max) {
    const int n = w->nl, r = j - n;
    if (j < n) snprintf(t, (size_t)max, "%s List", j < w->pl->n ? w->pl->titles[j] : "MIDI Ch");
    else if (r < PL_ROWS) snprintf(t, (size_t)max, "List %d", r + 1);
    else if (r >= PL_MCH) pl_copy(t, r == PL_MCH ? "MIDI Ch" : r == PL_MCH_PREV ? "MIDI Ch <" : "MIDI Ch >", max);
    else pl_copy(t, r == PL_PREV ? "List <" : r == PL_NEXT ? "List >" : r == PL_CLOSE ? "List Close" : "List Title", max);
}
static void pl_display(PickWrap *w, int j, char *t, int max) {
    const int n = w->nl, r = j - n;
    t[0] = 0;
    if (j < n) { pl_copy(t, w->norm[j] > 0.5f ? "Open" : "Closed", max); return; }
    if (r == PL_MCH) { if (w->ch) snprintf(t, (size_t)max, "MIDI Ch %d", w->ch); else pl_copy(t, "MIDI Omni", max); return; }
    if (r == PL_MCH_PREV || r == PL_MCH_NEXT) { pl_copy(t, r == PL_MCH_PREV ? "<" : ">", max); return; }
    if (r < PL_ROWS) {
        const int idx = w->page * PL_ROWS + r;
        if (w->open < 0 || idx >= pl_count(w)) { pl_copy(t, " ", max); return; }
        char name[96] = {0};
        pl_fn_of(w, w->open, PL_NAME, idx, name, sizeof name);
        const int cur = pl_fn_of(w, w->open, PL_CURRENT, 0, NULL, 0);
        snprintf(t, (size_t)max, "%s%s", idx == cur ? "> " : "", name[0] ? name : "-");
        return;
    }
    if (r == PL_PREV) { pl_copy(t, "< Page", max); return; }
    if (r == PL_NEXT) { pl_copy(t, "Page >", max); return; }
    if (r == PL_CLOSE) { pl_copy(t, "Close", max); return; }
    if (w->open < 0) { pl_copy(t, " ", max); return; }
    const int np = pl_pages(w);
    char title[64] = {0};
    pl_fn_of(w, w->open, PL_HEADING, 0, title, sizeof title);
    const char *ti = title[0] ? title : w->open < w->pl->n ? w->pl->titles[w->open] : "MIDI Channel";
    if (pl_count(w) == 0) snprintf(t, (size_t)max, "%s: none", ti);
    else if (np > 1) snprintf(t, (size_t)max, "%s  %d/%d", ti, w->page + 1, np);
    else pl_copy(t, ti, max);
}

static intptr_t pl_dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    PickWrap *w = (PickWrap *)e->object;
    if (idx >= w->base && (op == effGetParamName || op == effGetParamLabel || op == effGetParamDisplay || op == effCanBeAutomated)) {
        const int j = idx - w->base;
        if (j >= pl_nparams(w->pl)) return 0;
        if (op == effCanBeAutomated) return 1;
        if (!p) return 0;
        if (op == effGetParamLabel) { ((char *)p)[0] = 0; return 1; }
        char t[96];
        if (op == effGetParamName) { pl_name(w, j, t, 32); pl_copy((char *)p, t, 32); }
        else { pl_display(w, j, t, sizeof t); pl_copy((char *)p, t, 32); }
        return 1;
    }
    if (op == effClose) {
        const intptr_t r = w->in->dispatcher(w->in, op, idx, v, p, o);
        for (int i = 0; i < 64; i++) if (pl_all[i] == w) pl_all[i] = NULL;
        free(w->chunk);
        free(w->arrow);
        free(w->rest);
        free(w->alast);
        free(w->rsent);
        free(w);
        return r;
    }
    if (w->pl->midi && (w->in->flags & effFlagsProgramChunks) && op == effGetChunk && p) {   /* + our MIDI Ch */
        void *c = NULL;
        const intptr_t n = w->in->dispatcher(w->in, op, idx, v, &c, o);
        if (n < 0 || (n > 0 && !c)) return n;
        char *b = (char *)realloc(w->chunk, (size_t)n + PL_TRAILER);
        if (!b) { *(void **)p = c; return n; }
        w->chunk = b;
        if (n) memcpy(b, c, (size_t)n);
        memset(b + n, 0, PL_TRAILER);
        snprintf(b + n, PL_TRAILER, "PLWMIDI %02d", w->ch);
        *(void **)p = b;
        return n + PL_TRAILER;
    }
    if (w->pl->midi && (w->in->flags & effFlagsProgramChunks) && op == effSetChunk && p && v >= PL_TRAILER &&
        !memcmp((const char *)p + v - PL_TRAILER, "PLWMIDI ", 8)) {
        const int ch = atoi((const char *)p + v - PL_TRAILER + 8);
        pl_set_ch(w, ch < 0 ? 0 : ch > 16 ? 16 : ch);
        v -= PL_TRAILER;
    }
    const intptr_t r = w->in->dispatcher(w->in, op, idx, v, p, o);
    pl_sync(w);
    return r;
}

static void pl_flush(PickWrap *w) {
    if (!w->master) return;
    int any = 0;
    if (w->rest) for (int i = 0; i < w->base; i++)
        if (w->rest[i]) { w->rest[i] = 0; w->rsent[i] = 1; w->master(&w->fx, audioMasterAutomate, i, 0, NULL, PL_REST); any = 1; }
    for (int j = 0; j < pl_nparams(w->pl); j++)
        if (w->report[j]) {
            if (w->rsent && w->norm[j] == PL_REST) w->rsent[w->base + j] = 1;
            if (w->report[j] == 2) w->master(&w->fx, audioMasterAutomate, w->base + j, 0, NULL, 0.001f);   /* (a change) */
            w->report[j] = 0;
            any = 1;
            w->master(&w->fx, audioMasterAutomate, w->base + j, 0, NULL, w->norm[j]);
        }
    if (any) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, NULL, 0);
}
/* MIDI from the MPC's inputs on our channel, to the plugin as a host would send it */
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4];
                 char detune, noteOffVelocity, reserved1, reserved2; } PlMidiEvent;
static void pl_midi(PickWrap *w) {
    if (!w->pl->midi || !mpc_ext_midi_read) return;
    enum { MAXEV = 32 };
    struct { int32_t numEvents; intptr_t reserved; PlMidiEvent *events[MAXEV]; } evs;
    PlMidiEvent ev[MAXEV];
    unsigned char m[3];
    int k = 0;
    while (k < MAXEV && mpc_ext_midi_read(&w->midi_pos, m)) {
        if (w->ch && (m[0] & 15) != w->ch - 1) continue;
        memset(&ev[k], 0, sizeof ev[k]);
        ev[k].type = 1;
        ev[k].byteSize = (int32_t)sizeof ev[k];
        memcpy(ev[k].midiData, m, 3);
        evs.events[k] = &ev[k];
        k++;
    }
    if (!k) return;
    evs.numEvents = k;
    evs.reserved = 0;
    w->in->dispatcher(w->in, 25 /* effProcessEvents */, 0, 0, &evs, 0);
}
static void pl_processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    PickWrap *w = (PickWrap *)e->object;
    pl_flush(w);
    pl_midi(w);
    w->in->processReplacing(w->in, in, out, n);
}
static void pl_process(AEffect *e, float **in, float **out, int32_t n) {
    PickWrap *w = (PickWrap *)e->object;
    pl_flush(w);
    pl_midi(w);
    if (w->in->process) w->in->process(w->in, in, out, n);
    else w->in->processReplacing(w->in, in, out, n);
}

/* VSTPluginMain: the plugin made by make(), behind the wrapper */
static AEffect *pl_wrap(audioMasterCallback master, AEffect *(*make)(audioMasterCallback), const PickLists *pl) {
    pl_real_master = master;
    AEffect *in = make(pl_host);
    if (!in || pl_nparams(pl) > PL_MAX) return in;
    PickWrap *w = (PickWrap *)calloc(1, sizeof *w);
    if (!w) return in;
    w->in = in;
    w->master = master;
    w->pl = pl;
    w->base = in->numParams;
    w->open = -1;
    w->nl = pl->n + (pl->midi ? 1 : 0);
    w->midi_pos = 0xffffffffu;   /* (from now on) */
    w->arrow = (int *)calloc((size_t)(in->numParams > 0 ? in->numParams : 1), sizeof *w->arrow);
    const int nall = in->numParams + pl_nparams(pl);
    w->rest = (unsigned char *)calloc((size_t)nall, 1);
    w->alast = (float *)calloc((size_t)nall, sizeof *w->alast);
    w->rsent = (unsigned char *)calloc((size_t)nall, 1);
    for (int k = 0; w->alast && k < nall; k++) w->alast[k] = PL_REST;
    if (pl->midi) w->norm[w->nl + PL_MCH_PREV] = w->norm[w->nl + PL_MCH_NEXT] = PL_REST;
    if (w->arrow && w->rest && w->alast && w->rsent) {   /* the arrows: "<name> <" with its "<name> >" */
        static char nm[1024][40];
        const int np = in->numParams < 1024 ? in->numParams : 1024;
        for (int i = 0; i < np; i++) { nm[i][0] = 0; in->dispatcher(in, effGetParamName, i, 0, nm[i], 0); nm[i][39] = 0; }
        for (int i = 0; i < np; i++) {
            const size_t l = strlen(nm[i]);
            if (l < 3 || nm[i][l - 2] != ' ' || nm[i][l - 1] != '<') continue;
            for (int k = 0; k < np; k++)
                if (k != i && strlen(nm[k]) == l && !strncmp(nm[k], nm[i], l - 1) && nm[k][l - 1] == '>') {
                    w->arrow[i] = -(k + 1); w->arrow[k] = i + 1;
                    break;
                }
        }
    }
    w->fx = *in;
    w->fx.object = w;
    w->fx.user = NULL;
    w->fx.numParams = in->numParams + pl_nparams(pl);
    w->fx.dispatcher = pl_dispatcher;
    w->fx.setParameter = pl_setParameter;
    w->fx.getParameter = pl_getParameter;
    w->fx.processReplacing = pl_processReplacing;
    w->fx.process = pl_process;
    w->fx.processDoubleReplacing = NULL;
    w->fx.flags &= ~(1 << 12);   /* (effFlagsCanDoubleReplacing: we only pass floats on) */
    for (int i = 0; i < 64; i++) if (!pl_all[i]) { pl_all[i] = w; return &w->fx; }
    free(w);   /* (no room: the plugin as it is) */
    return in;
}

/* params.json entries for the lists, to append after the plugin's own (each line starts with ",\n") */
__attribute__((unused)) static void pl_params_json(const PickLists *pl, FILE *f) {
    for (int j = 0; j < pl->n; j++)
        fprintf(f, ",\n    {\"key\": \"%s__list\", \"name\": \"%s List\", \"options\": [\"Closed\", \"Open\"]}", pl->keys[j], pl->titles[j]);
    if (pl->midi) fprintf(f, ",\n    {\"key\": \"midi_ch__list\", \"name\": \"MIDI Ch List\", \"options\": [\"Closed\", \"Open\"]}");
    for (int r = 0; r < PL_ROWS; r++)
        fprintf(f, ",\n    {\"key\": \"pick_%d\", \"name\": \"List %d\", \"options\": [\"Off\", \"On\"], \"display\": \"string\"}", r + 1, r + 1);
    fprintf(f, ",\n    {\"key\": \"pick_prev\", \"name\": \"List <\", \"options\": [\"Off\", \"On\"], \"momentary\": true}");
    fprintf(f, ",\n    {\"key\": \"pick_next\", \"name\": \"List >\", \"options\": [\"Off\", \"On\"], \"momentary\": true}");
    fprintf(f, ",\n    {\"key\": \"pick_close\", \"name\": \"List Close\", \"options\": [\"Off\", \"On\"], \"momentary\": true}");
    fprintf(f, ",\n    {\"key\": \"pick_title\", \"name\": \"List Title\", \"options\": [\"Off\", \"On\"], \"display\": \"string\"}");
    if (!pl->midi) return;
    fprintf(f, ",\n    {\"key\": \"midi_ch\", \"name\": \"MIDI Ch\", \"min\": 0, \"max\": 1, \"display\": \"string\", \"type\": \"stepper\"}");
    fprintf(f, ",\n    {\"key\": \"midi_ch_prev\", \"name\": \"MIDI Ch <\", \"options\": [\"Off\", \"On\"], \"momentary\": true}");
    fprintf(f, ",\n    {\"key\": \"midi_ch_next\", \"name\": \"MIDI Ch >\", \"options\": [\"Off\", \"On\"], \"momentary\": true}");
}
