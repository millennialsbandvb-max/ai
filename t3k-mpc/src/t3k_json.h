/* t3k_json.h: just enough JSON for TONE3000's API answers (our own code). jp_parse() builds a flat table of nodes
 * over the text (no copies); jp_get() finds a key in an object, jp_at() an array item, jp_str() copies a string out
 * (escapes decoded, \u to UTF-8). Malformed input gives -1, never a crash. */
#ifndef T3K_JSON_H
#define T3K_JSON_H
#include <stdlib.h>
#include <string.h>

enum { JT_OBJ, JT_ARR, JT_STR, JT_NUM, JT_LIT };
typedef struct { int type, start, end, parent, next, first; } JNode;   /* start..end: the text (a string: inside quotes) */
typedef struct { const char *s; int len; JNode *n; int count, cap; } JDoc;

static int jp_ws(const JDoc *d, int i) { while (i < d->len && (d->s[i] == ' ' || d->s[i] == '\n' || d->s[i] == '\r' || d->s[i] == '\t')) i++; return i; }
static int jp_new(JDoc *d, int type, int start, int parent) {
    if (d->count == d->cap) {
        const int cap = d->cap ? d->cap * 2 : 256;
        JNode *n = realloc(d->n, (size_t)cap * sizeof *n);
        if (!n) return -1;
        d->n = n; d->cap = cap;
    }
    JNode *x = &d->n[d->count];
    x->type = type; x->start = start; x->end = start; x->parent = parent; x->next = -1; x->first = -1;
    return d->count++;
}
/* one value at i; returns the index after it, or -1 */
static int jp_value(JDoc *d, int i, int parent, int *out, int depth) {
    i = jp_ws(d, i);
    if (i >= d->len || depth > 64) return -1;
    const char c = d->s[i];
    if (c == '"') {
        const int k = jp_new(d, JT_STR, i + 1, parent);
        if (k < 0) return -1;
        i++;
        while (i < d->len && d->s[i] != '"') i += d->s[i] == '\\' ? 2 : 1;
        if (i >= d->len) return -1;
        d->n[k].end = i;
        *out = k;
        return i + 1;
    }
    if (c == '{' || c == '[') {
        const int k = jp_new(d, c == '{' ? JT_OBJ : JT_ARR, i, parent);
        if (k < 0) return -1;
        int last = -1;
        i = jp_ws(d, i + 1);
        if (i < d->len && d->s[i] == (c == '{' ? '}' : ']')) { d->n[k].end = i + 1; *out = k; return i + 1; }
        for (;;) {
            int item;
            if (c == '{') {   /* key (a string node, its value its first child... kept simple: key then value as siblings) */
                int key;
                i = jp_value(d, i, k, &key, depth + 1);
                if (i < 0 || d->n[key].type != JT_STR) return -1;
                i = jp_ws(d, i);
                if (i >= d->len || d->s[i] != ':') return -1;
                if (last >= 0) d->n[last].next = key; else d->n[k].first = key;
                last = key;
                i = jp_value(d, i + 1, k, &item, depth + 1);
            } else i = jp_value(d, i, k, &item, depth + 1);
            if (i < 0) return -1;
            if (last >= 0) d->n[last].next = item; else d->n[k].first = item;
            last = item;
            i = jp_ws(d, i);
            if (i < d->len && d->s[i] == ',') { i++; continue; }
            if (i < d->len && d->s[i] == (c == '{' ? '}' : ']')) { d->n[k].end = i + 1; *out = k; return i + 1; }
            return -1;
        }
    }
    const int k = jp_new(d, (c == '-' || (c >= '0' && c <= '9')) ? JT_NUM : JT_LIT, i, parent);
    if (k < 0) return -1;
    while (i < d->len && !strchr(",}] \n\r\t", d->s[i])) i++;
    d->n[k].end = i;
    *out = k;
    return i;
}
static int jp_parse(JDoc *d, const char *s, int len) {
    memset(d, 0, sizeof *d);
    d->s = s; d->len = len;
    int root;
    return jp_value(d, 0, -1, &root, 0) < 0 ? -1 : root;
}
static void jp_free(JDoc *d) { free(d->n); d->n = NULL; d->count = d->cap = 0; }
/* the value of key in object o (-1: none) */
static int jp_get(const JDoc *d, int o, const char *key) {
    if (o < 0 || d->n[o].type != JT_OBJ) return -1;
    const size_t kl = strlen(key);
    for (int k = d->n[o].first; k >= 0; k = d->n[d->n[k].next].next) {
        const int v = d->n[k].next;
        if (v < 0) return -1;
        if ((size_t)(d->n[k].end - d->n[k].start) == kl && !memcmp(d->s + d->n[k].start, key, kl)) return v;
    }
    return -1;
}
static int jp_len(const JDoc *d, int a) { int n = 0; if (a >= 0 && d->n[a].type == JT_ARR) for (int k = d->n[a].first; k >= 0; k = d->n[k].next) n++; return n; }
static int jp_at(const JDoc *d, int a, int i) {
    if (a < 0 || d->n[a].type != JT_ARR) return -1;
    for (int k = d->n[a].first; k >= 0; k = d->n[k].next) if (i-- == 0) return k;
    return -1;
}
/* a string (decoded) or a number / literal's text into out; "" if missing or null */
static void jp_str(const JDoc *d, int v, char *out, int max) {
    out[0] = 0;
    if (v < 0 || max < 1) return;
    const JNode *x = &d->n[v];
    if (x->type == JT_LIT && x->end - x->start == 4 && !memcmp(d->s + x->start, "null", 4)) return;
    int o = 0;
    for (int i = x->start; i < x->end && o < max - 1; i++) {
        char c = d->s[i];
        if (x->type == JT_STR && c == '\\' && i + 1 < x->end) {
            c = d->s[++i];
            if (c == 'n' || c == 'r' || c == 't') c = ' ';
            else if (c == 'u' && i + 4 < x->end) {
                unsigned u = (unsigned)strtoul((char[5]){d->s[i + 1], d->s[i + 2], d->s[i + 3], d->s[i + 4], 0}, NULL, 16);
                i += 4;
                if (u < 0x80) c = (char)u;
                else if (u < 0x800 && o < max - 2) { out[o++] = (char)(0xc0 | (u >> 6)); c = (char)(0x80 | (u & 0x3f)); }
                else if (o < max - 3) { out[o++] = (char)(0xe0 | (u >> 12)); out[o++] = (char)(0x80 | ((u >> 6) & 0x3f)); c = (char)(0x80 | (u & 0x3f)); }
                else break;
            }
        }
        out[o++] = c;
    }
    out[o] = 0;
}
static long jp_num(const JDoc *d, int v) { char t[32]; jp_str(d, v, t, sizeof t); return strtol(t, NULL, 10); }
#endif
