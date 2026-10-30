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
 * MPC knows. */
#pragma once
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../nam-mpc/src/vst2.h"

#define PL_ROWS 24
#define PL_MAX 64
enum { PL_COUNT, PL_NAME, PL_CURRENT, PL_CHOOSE };
typedef int (*pl_fn)(AEffect *e, int list, int op, int i, char *out, int max);
typedef struct {
    int n;
    const char *const *keys;     /* the steppers' parameter keys: "<key>__list" */
    const char *const *titles;   /* shown over the list */
    pl_fn fn;
} PickLists;

enum { PL_PREV = PL_ROWS, PL_NEXT, PL_CLOSE, PL_TITLE, PL_FIXED };   /* after the open flags: rows, then these */

typedef struct {
    AEffect fx;                  /* what MPC sees */
    AEffect *in;                 /* the plugin */
    audioMasterCallback master;
    const PickLists *pl;
    int base, open, page;
    float norm[PL_MAX];
    volatile unsigned char report[PL_MAX];
} PickWrap;

static PickWrap *pl_all[64];
static audioMasterCallback pl_real_master;

static int pl_nparams(const PickLists *pl) { return pl->n + PL_FIXED; }
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
        if (w && w->in == e) { pl_sync(w); return w->master ? w->master(&w->fx, op, idx, val, p, opt) : 0; }
    }
    return pl_real_master ? pl_real_master(e, op, idx, val, p, opt) : 0;   /* (while the plugin is being made) */
}

static int pl_count(PickWrap *w) { return w->open < 0 ? 0 : w->pl->fn(w->in, w->open, PL_COUNT, 0, NULL, 0); }
static int pl_pages(PickWrap *w) { const int n = pl_count(w); return n > 0 ? (n + PL_ROWS - 1) / PL_ROWS : 1; }
static void pl_set(PickWrap *w, int j, float v) { w->norm[j] = v; w->report[j] = 1; }
static void pl_rows_changed(PickWrap *w) {   /* new text: reported as a change, so MPC repaints the rows */
    for (int r = 0; r < PL_ROWS; r++) { w->norm[w->pl->n + r] = 0; w->report[w->pl->n + r] = 2; }
    w->report[w->pl->n + PL_TITLE] = 2;
}
static void pl_close(PickWrap *w) {
    if (w->open >= 0) pl_set(w, w->open, 0);
    w->open = -1;
    pl_rows_changed(w);
}

static void pl_setParameter(AEffect *e, int32_t i, float v) {
    PickWrap *w = (PickWrap *)e->object;
    if (i < w->base) { w->in->setParameter(w->in, i, v); return; }
    const int j = i - w->base, n = w->pl->n;
    if (j >= pl_nparams(w->pl)) return;
    const int press = v > 0.5f;
    if (j < n) {   /* a list's open flag (the stepper's field toggles it) */
        if (press) {
            if (w->open >= 0 && w->open != j) pl_set(w, w->open, 0);
            w->open = j;
            w->norm[j] = 1;
            const int cur = w->pl->fn(w->in, j, PL_CURRENT, 0, NULL, 0);
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
                w->pl->fn(w->in, list, PL_CHOOSE, idx, NULL, 0);
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
    }
}
static float pl_getParameter(AEffect *e, int32_t i) {
    PickWrap *w = (PickWrap *)e->object;
    if (i < w->base) return w->in->getParameter(w->in, i);
    return i - w->base < pl_nparams(w->pl) ? w->norm[i - w->base] : 0;
}

static void pl_name(PickWrap *w, int j, char *t, int max) {
    const int n = w->pl->n, r = j - n;
    if (j < n) snprintf(t, (size_t)max, "%s List", w->pl->titles[j]);
    else if (r < PL_ROWS) snprintf(t, (size_t)max, "List %d", r + 1);
    else pl_copy(t, r == PL_PREV ? "List <" : r == PL_NEXT ? "List >" : r == PL_CLOSE ? "List Close" : "List Title", max);
}
static void pl_display(PickWrap *w, int j, char *t, int max) {
    const int n = w->pl->n, r = j - n;
    t[0] = 0;
    if (j < n) { pl_copy(t, w->norm[j] > 0.5f ? "Open" : "Closed", max); return; }
    if (r < PL_ROWS) {
        const int idx = w->page * PL_ROWS + r;
        if (w->open < 0 || idx >= pl_count(w)) { pl_copy(t, " ", max); return; }
        char name[96] = {0};
        w->pl->fn(w->in, w->open, PL_NAME, idx, name, sizeof name);
        const int cur = w->pl->fn(w->in, w->open, PL_CURRENT, 0, NULL, 0);
        snprintf(t, (size_t)max, "%s%s", idx == cur ? "> " : "", name[0] ? name : "-");
        return;
    }
    if (r == PL_PREV) { pl_copy(t, "< Page", max); return; }
    if (r == PL_NEXT) { pl_copy(t, "Page >", max); return; }
    if (r == PL_CLOSE) { pl_copy(t, "Close", max); return; }
    if (w->open < 0) { pl_copy(t, " ", max); return; }
    const int np = pl_pages(w);
    if (pl_count(w) == 0) snprintf(t, (size_t)max, "%s: none", w->pl->titles[w->open]);
    else if (np > 1) snprintf(t, (size_t)max, "%s  %d/%d", w->pl->titles[w->open], w->page + 1, np);
    else pl_copy(t, w->pl->titles[w->open], max);
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
        free(w);
        return r;
    }
    const intptr_t r = w->in->dispatcher(w->in, op, idx, v, p, o);
    pl_sync(w);
    return r;
}

static void pl_flush(PickWrap *w) {
    if (!w->master) return;
    int any = 0;
    for (int j = 0; j < pl_nparams(w->pl); j++)
        if (w->report[j]) {
            if (w->report[j] == 2) w->master(&w->fx, audioMasterAutomate, w->base + j, 0, NULL, 0.001f);   /* (a change) */
            w->report[j] = 0;
            any = 1;
            w->master(&w->fx, audioMasterAutomate, w->base + j, 0, NULL, w->norm[j]);
        }
    if (any) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, NULL, 0);
}
static void pl_processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    PickWrap *w = (PickWrap *)e->object;
    pl_flush(w);
    w->in->processReplacing(w->in, in, out, n);
}
static void pl_process(AEffect *e, float **in, float **out, int32_t n) {
    PickWrap *w = (PickWrap *)e->object;
    pl_flush(w);
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
    for (int r = 0; r < PL_ROWS; r++)
        fprintf(f, ",\n    {\"key\": \"pick_%d\", \"name\": \"List %d\", \"options\": [\"Off\", \"On\"], \"display\": \"string\"}", r + 1, r + 1);
    fprintf(f, ",\n    {\"key\": \"pick_prev\", \"name\": \"List <\", \"options\": [\"Off\", \"On\"], \"momentary\": true}");
    fprintf(f, ",\n    {\"key\": \"pick_next\", \"name\": \"List >\", \"options\": [\"Off\", \"On\"], \"momentary\": true}");
    fprintf(f, ",\n    {\"key\": \"pick_close\", \"name\": \"List Close\", \"options\": [\"Off\", \"On\"], \"momentary\": true}");
    fprintf(f, ",\n    {\"key\": \"pick_title\", \"name\": \"List Title\", \"options\": [\"Off\", \"On\"], \"display\": \"string\"}");
}
