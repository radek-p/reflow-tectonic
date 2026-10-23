/* xetex-rtxcapture.c: the reflowtex capture API.
 *
 * Writes <jobname>.rtxc.jsonl through Tectonic's I/O: every paragraph as
 * line_break receives it, TeX's own breaks, the main vertical list in
 * document order (build_page), displays as TeX appended them (and, with
 * RTX_SAMPLES, re-typeset at other widths in forked copies of the engine),
 * math lists, fonts and glyph metrics. Nothing here changes what TeX
 * typesets, except RTX_PAGELESS, which hands the page builder zero-height
 * originals (as reflowtex's LuaTeX capture does).
 *
 * Copyright 2026 Radosław Piórkowski. SPDX-License-Identifier: AGPL-3.0-or-later (reflow/LICENSE).
 */

#include "xetex-core.h"
#include "xetex-xetexd.h"
#include "xetex-ext.h"
#include "xetex-rtxcapture.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unicode/ubidi.h>

#ifndef __EMSCRIPTEN__
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#define RTX_CAN_FORK 1
#endif

/* xetex_layout additions (patches/): the raw HarfBuzz buffer, font units */
extern int32_t rtx_layout_raw(XeTeXLayoutEngine engine, int32_t max, uint32_t *clusters,
                              int32_t *x_advance, int32_t *y_advance, int32_t *x_offset, int32_t *y_offset);
extern uint32_t rtx_font_units_per_em(XeTeXLayoutEngine engine);
extern int32_t rtx_glyph_advance_units(XeTeXLayoutEngine engine, uint32_t gid);

int rtx_capture_flags = 0;
unsigned char *rtx_nb = NULL;
int rtx_fork_pending = 0;

#define NB_CONTRIB 0x01
#define NB_LINE    0x08
#define NB_DISP    0x10
#define NB_PAR     0x20
#define NB_MATH    0x40
#define NB_INS     0x80

#define MAX_SAMPLES 16
static int32_t samples[MAX_SAMPLES];
static int nsamples = 0;

/* ── growable byte buffer ─────────────────────────────────────────────── */

typedef struct { char *b; size_t n, cap; } sbuf;

static void sb_grow(sbuf *s, size_t more) {
    if (s->n + more <= s->cap) return;
    size_t c = s->cap ? s->cap : 1 << 16;
    while (c < s->n + more) c *= 2;
    s->b = xrealloc(s->b, c);
    s->cap = c;
}
static void sb_putc(sbuf *s, char c) { sb_grow(s, 1); s->b[s->n++] = c; }
static void sb_write(sbuf *s, const char *p, size_t n) { sb_grow(s, n); memcpy(s->b + s->n, p, n); s->n += n; }
static void sb_puts(sbuf *s, const char *p) { sb_write(s, p, strlen(p)); }

static sbuf out;                 /* records not yet written */
static rust_output_handle_t out_h = 0;
static bool opened = false;

/* ── JSON writer ──────────────────────────────────────────────────────── */

static int jd = 0;
static char jfirst[512];
static bool after_key = false;

static void jcomma(void) {
    if (after_key) { after_key = false; return; }
    if (jd > 0) {
        if (!jfirst[jd]) sb_putc(&out, ',');
        jfirst[jd] = 0;
    }
}
static void jopen(char c) { jcomma(); sb_putc(&out, c); jfirst[++jd] = 1; }
static void jclose(char c) { sb_putc(&out, c); jd--; }
static void jkey(const char *k) { jcomma(); sb_putc(&out, '"'); sb_puts(&out, k); sb_puts(&out, "\":"); after_key = true; }
static void jnum(long long v) { char t[32]; jcomma(); snprintf(t, sizeof t, "%lld", v); sb_puts(&out, t); }
static void jdbl(double v) {
    char t[40]; jcomma();
    if (v != v) v = 0;
    snprintf(t, sizeof t, "%.17g", v);
    sb_puts(&out, t);
}
static void jint(const char *k, long long v) { jkey(k); jnum(v); }
static void jbool(const char *k, bool v) { jkey(k); jcomma(); sb_puts(&out, v ? "true" : "false"); }

static void put_utf8_cp(uint32_t c) {
    char t[8];
    if (c == '"' || c == '\\') { sb_putc(&out, '\\'); sb_putc(&out, (char) c); return; }
    if (c < 0x20) { snprintf(t, sizeof t, "\\u%04x", c); sb_puts(&out, t); return; }
    if (c < 0x80) { sb_putc(&out, (char) c); return; }
    if (c < 0x800) { sb_putc(&out, 0xC0 | (c >> 6)); sb_putc(&out, 0x80 | (c & 63)); return; }
    if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
    if (c < 0x10000) { sb_putc(&out, 0xE0 | (c >> 12)); sb_putc(&out, 0x80 | ((c >> 6) & 63)); sb_putc(&out, 0x80 | (c & 63)); return; }
    sb_putc(&out, 0xF0 | (c >> 18)); sb_putc(&out, 0x80 | ((c >> 12) & 63));
    sb_putc(&out, 0x80 | ((c >> 6) & 63)); sb_putc(&out, 0x80 | (c & 63));
}

/* a string of bytes that should be UTF-8; invalid bytes are taken as Latin-1 */
static void jbytes(const unsigned char *s, size_t n) {
    jcomma();
    sb_putc(&out, '"');
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0 && i + len <= n;
        for (int k = 1; ok && k < len; k++) ok = (s[i + k] & 0xC0) == 0x80;
        if (!ok) { put_utf8_cp(c); i++; continue; }
        uint32_t cp = len == 1 ? c : len == 2 ? (c & 31) : len == 3 ? (c & 15) : (c & 7);
        for (int k = 1; k < len; k++) cp = (cp << 6) | (s[i + k] & 63);
        put_utf8_cp(cp);
        i += len;
    }
    sb_putc(&out, '"');
}
static void jcstr(const char *k, const char *s) { jkey(k); jbytes((const unsigned char *) s, s ? strlen(s) : 0); }

static void jutf16(const uint16_t *t, int n) {
    jcomma();
    sb_putc(&out, '"');
    for (int i = 0; i < n; i++) {
        uint32_t c = t[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && t[i + 1] >= 0xDC00 && t[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (t[i + 1] - 0xDC00);
            i++;
        }
        put_utf8_cp(c);
    }
    sb_putc(&out, '"');
}

/* a TeX string (str_number): UTF-16 in the pool */
static void jtexstr(str_number s) {
    if (s < 65536) { char t[2] = { (char) s, 0 }; jbytes((unsigned char *) t, s < 128 ? 1 : 0); return; }
    pool_pointer a = str_start[s - 65536], b = str_start[s + 1 - 65536];
    jutf16(&str_pool[a], b - a);
}

/* the expanded text of a token list, as special_out prints it (UTF-8 bytes) */
static void jtokens(int32_t ref) {
    unsigned char old = selector;
    pool_pointer start = pool_ptr;
    bool olds = doing_special;
    doing_special = true;
    selector = SELECTOR_NEW_STRING;
    if (ref != TEX_NULL)
        show_token_list(LLIST_link(ref), TEX_NULL, pool_size - pool_ptr);
    selector = old;
    doing_special = olds;
    size_t n = pool_ptr - start;
    unsigned char *tmp = xmalloc(n + 1);
    for (size_t i = 0; i < n; i++) tmp[i] = (unsigned char) str_pool[start + i];
    pool_ptr = start;
    jbytes(tmp, n);
    free(tmp);
}

static void open_output(void) {
    if (opened) return;
    opened = true;
    char *job = gettexstring(job_name);
    size_t n = strlen(job) + 16;
    char *name = xmalloc(n);
    snprintf(name, n, "%s.rtxc.jsonl", job);
    out_h = ttstub_output_open(name, 0);
    free(name);
    free(job);
}

static bool child = false;       /* a forked sample run */
static bool child_done = false;  /* (wasm) the sample child has stashed its sample */
static int child_fd = -1;
static int child_disp = 0;
static int32_t child_delta = 0;

static void flush_out(void) {
    if (child || !out.n) return;
    open_output();
    if (out_h) ttstub_output_write(out_h, out.b, out.n);
    out.n = 0;
}

static void rec_begin(const char *t) {
    jd = 0; after_key = false;
    jopen('{');
    jcstr("t", t);
}
static void rec_end(void) {
    jclose('}');
    sb_putc(&out, '\n');
    if (out.n > (1 << 20)) flush_out();
}

/* ── pointer maps (open addressing, no deletion: validity is the node byte) */

typedef struct { int32_t *k; int64_t *v; uint32_t cap, n; } rmap;

static void rmap_put(rmap *m, int32_t k, int64_t v);
static void rmap_rehash(rmap *m) {
    rmap o = *m;
    m->cap = o.cap ? o.cap * 2 : 4096;
    m->n = 0;
    m->k = xmalloc(sizeof(int32_t) * m->cap);
    m->v = xmalloc(sizeof(int64_t) * m->cap);
    for (uint32_t i = 0; i < m->cap; i++) m->k[i] = TEX_NULL;
    for (uint32_t i = 0; i < o.cap; i++) if (o.k[i] != TEX_NULL) rmap_put(m, o.k[i], o.v[i]);
    free(o.k); free(o.v);
}
static void rmap_put(rmap *m, int32_t k, int64_t v) {
    if ((m->n + 1) * 2 > m->cap) rmap_rehash(m);
    uint32_t h = ((uint32_t) k * 2654435761u) & (m->cap - 1);
    while (m->k[h] != TEX_NULL && m->k[h] != k) h = (h + 1) & (m->cap - 1);
    if (m->k[h] == TEX_NULL) m->n++;
    m->k[h] = k; m->v[h] = v;
}
static bool rmap_get(rmap *m, int32_t k, int64_t *v) {
    if (!m->cap) return false;
    uint32_t h = ((uint32_t) k * 2654435761u) & (m->cap - 1);
    while (m->k[h] != TEX_NULL) {
        if (m->k[h] == k) { *v = m->v[h]; return true; }
        h = (h + 1) & (m->cap - 1);
    }
    return false;
}
static void rmap_clear(rmap *m) {
    for (uint32_t i = 0; i < m->cap; i++) m->k[i] = TEX_NULL;
    m->n = 0;
}

static rmap line_map, disp_map, par_map, math_map, ins_map;

/* ── fonts and glyphs used ────────────────────────────────────────────── */

typedef struct { unsigned char *bits; } fontuse;
static fontuse *fonts_used = NULL;
static int fonts_cap = 0;

static void note_glyph(int f, uint32_t g) {
    if (f < 0) return;
    if (f >= fonts_cap) {
        int c = fonts_cap ? fonts_cap : 64;
        while (c <= f) c *= 2;
        fonts_used = xrealloc(fonts_used, sizeof(fontuse) * c);
        memset(fonts_used + fonts_cap, 0, sizeof(fontuse) * (c - fonts_cap));
        fonts_cap = c;
    }
    if (!fonts_used[f].bits) fonts_used[f].bits = xcalloc(1, 1 << 18); /* 2^21 code points */
    if (g < (1u << 21)) fonts_used[f].bits[g >> 3] |= 1 << (g & 7);
}

static bool is_native(int f) { return font_area[f] == OTGR_FONT_FLAG || font_area[f] == AAT_FONT_FLAG; }

/* ── setup ────────────────────────────────────────────────────────────── */

static int par_count = 0, cur_par = 0, disp_count = 0, mlist_count = 0, ins_count = 0;
static int32_t par_saved_tail = TEX_NULL;
static bool started = false;
static int mlist_depth = 0;
static int disp_mlist_reset(void);

static void srcpos_reset(void);
static void attrs_reset(void);
static void rmap_free(rmap *m) { free(m->k); free(m->v); m->k = NULL; m->v = NULL; m->cap = m->n = 0; }

/* Everything back to "no run yet": the driver may run the engine several
 * times in one process (TeX passes, a wasm page compiling again). */
static void reset(void) {
    free(rtx_nb); rtx_nb = NULL;
    rmap_free(&line_map); rmap_free(&disp_map); rmap_free(&par_map);
    rmap_free(&math_map); rmap_free(&ins_map);
    for (int f = 0; f < fonts_cap; f++) free(fonts_used[f].bits);
    free(fonts_used); fonts_used = NULL; fonts_cap = 0;
    par_count = cur_par = disp_count = mlist_count = ins_count = 0;
    par_saved_tail = TEX_NULL;
    started = false; opened = false; out_h = 0; out.n = 0;
    child = false; child_fd = -1; rtx_fork_pending = 0; mlist_depth = 0; child_done = false;
    disp_mlist_reset();
    if (rtx_capture_flags) rtx_nb = xcalloc(MEM_TOP + 1, 1);
    attrs_reset();
    srcpos_reset();
}

int rtx_set_variable(const char *name, int value) {
    if (!strcmp(name, "rtx_capture")) { rtx_capture_flags = value; nsamples = 0; reset(); return 0; }
    if (!strcmp(name, "rtx_sample")) { if (nsamples < MAX_SAMPLES) samples[nsamples++] = value; return 0; }
    return 1;
}

static void start(void) {
    if (started) return;
    started = true;
    /* attributes from here on: the format is loaded, nothing typeset yet */
    if ((rtx_capture_flags & RTX_ATTRS) && !rtx_ab) rtx_ab = xcalloc(MEM_TOP + 1, sizeof(uint16_t));
    rec_begin("hdr");
    jint("v", 1);
    jint("flags", rtx_capture_flags);
    jint("hsize", DIMENPAR(hsize));
    jkey("samples"); jopen('[');
    for (int i = 0; i < nsamples; i++) jnum(samples[i]);
    jclose(']');
    rec_end();
}

static void note_user_tag(int tag, str_number name);

void rtx_input_opened(void) {
    if (RTX_ON) note_user_tag(cur_input.synctex_tag, in_open > 0 && full_source_filename_stack[in_open] ? full_source_filename_stack[in_open] : cur_name);
    if (!RTX_ON || child) return;
    start();
    rec_begin("in");
    jint("tag", cur_input.synctex_tag);
    jkey("name");
    if (in_open > 0 && full_source_filename_stack[in_open]) jtexstr(full_source_filename_stack[in_open]);
    else jtexstr(cur_name);
    rec_end();
}

/* ── source positions (RTX_SRCPOS) ────────────────────────────────────── */
/* Where each typeset character came from: the file (synctex tag), line and
 * column of its token – or, for a character a macro produced, where the
 * outermost macro was called in a file ("via"). A position is packed into
 * 64 bits: tag 16 | line 20 | column 16 | span 11 | via 1.
 *
 * Tokens carry no positions in TeX. What the capture keeps:
 *  - get_next notes the token it returns: a column of a file line, or the
 *    token-list node it came from (RTX_TOK_FILE, RTX_TOK_NODE);
 *  - a token stored as a macro argument, or backed up, remembers the position
 *    of the token it copies (rtx_tokpos), so text in \emph{…} keeps its own
 *    positions when it is read back from the argument;
 *  - a macro called from a file remembers where (rtx_macro_enter/push): a
 *    character from its body gets that call site, flagged via;
 *  - a native word gets a position per UTF-16 unit of its text (collected
 *    per character, aligned through the font's mapping and XeTeX's merging of
 *    words), a math character field one position, and the char and glyph
 *    nodes mlist_to_hlist makes from a noad the noad's.
 * A byte per mem word (rtx_pb) says a word has a position; free_node and
 * get_avail clear it, so a reused address never inherits one. */

unsigned char *rtx_pb = NULL;
int32_t rtx_tok_node = TEX_NULL, rtx_tok_col = 0, rtx_tok_line = 0, rtx_tok_tag = 0;

#define PB_POS 1
#define POS_VIA 1ull
static uint64_t mkpos(int tag, int line, int col, bool via) {
    if (tag < 0 || tag > 0xFFFF) tag = 0;     /* beyond 16 bits: not known (no file, rather than another) */
    if (line < 0 || line > 0xFFFFF) line = 0;
    if (col < 0) col = 0;
    if (col > 0xFFFF) col = 0xFFFF;
    return ((uint64_t) (tag & 0xFFFF) << 48) | ((uint64_t) (line & 0xFFFFF) << 28) | ((uint64_t) col << 12) | (via ? POS_VIA : 0);
}
#define POS_TAG(p) ((int) ((p) >> 48))
#define POS_LINE(p) ((int) (((p) >> 28) & 0xFFFFF))
#define POS_COL(p) ((int) (((p) >> 12) & 0xFFFF))
#define POS_VIA_OF(p) ((int) ((p) & 1))
/* bits 1-11, the span: for a native word's unit, how many source characters
 * it stands for when the font's mapping made one of several (`` → “,
 * --- → —); for a via position, the extent of the macro call (\name and the
 * arguments read from that line); 0 = one, or not known */
#define POS_SPAN_MAX 0x7FF
#define POS_SPAN(p) ((int) (((p) >> 1) & POS_SPAN_MAX))
#define POS_BASE(p) ((p) & ~((uint64_t) POS_SPAN_MAX << 1))
static uint64_t with_span(uint64_t p, int n) {
    if (n < 0 || n > POS_SPAN_MAX) n = 0;      /* longer than a span can say: not known */
    return POS_BASE(p) | ((uint64_t) n << 1);
}

/* positions by mem address, with the word's content for token nodes (a
 * token node can be reused by an inline allocation that bypasses get_avail) */
typedef struct { int32_t *k; uint64_t *v; int32_t *c; uint32_t cap, n; } pmap;
static pmap posm;
#define NOCHECK INT32_MIN
static void pmap_put(pmap *m, int32_t k, uint64_t v, int32_t c);
static void pmap_grow(pmap *m) {
    pmap o = *m;
    m->cap = o.cap ? o.cap * 2 : 1 << 16;
    m->n = 0;
    m->k = xmalloc(sizeof(int32_t) * m->cap);
    m->v = xmalloc(sizeof(uint64_t) * m->cap);
    m->c = xmalloc(sizeof(int32_t) * m->cap);
    for (uint32_t i = 0; i < m->cap; i++) m->k[i] = TEX_NULL;
    for (uint32_t i = 0; i < o.cap; i++) if (o.k[i] != TEX_NULL) pmap_put(m, o.k[i], o.v[i], o.c[i]);
    free(o.k); free(o.v); free(o.c);
}
static void pmap_put(pmap *m, int32_t k, uint64_t v, int32_t c) {
    if ((m->n + 1) * 2 > m->cap) pmap_grow(m);
    uint32_t h = ((uint32_t) k * 2654435761u) & (m->cap - 1);
    while (m->k[h] != TEX_NULL && m->k[h] != k) h = (h + 1) & (m->cap - 1);
    if (m->k[h] == TEX_NULL) m->n++;
    m->k[h] = k; m->v[h] = v; m->c[h] = c;
}
static bool pmap_get(pmap *m, int32_t k, uint64_t *v, int32_t *c) {
    if (!m->cap) return false;
    uint32_t h = ((uint32_t) k * 2654435761u) & (m->cap - 1);
    while (m->k[h] != TEX_NULL) {
        if (m->k[h] == k) { *v = m->v[h]; *c = m->c[h]; return true; }
        h = (h + 1) & (m->cap - 1);
    }
    return false;
}

static void setpos(int32_t addr, uint64_t pos, int32_t check) {
    rtx_pb[addr] |= PB_POS;
    pmap_put(&posm, addr, pos, check);
}
static bool getpos(int32_t addr, uint64_t *pos) {
    if (!rtx_pb || addr == TEX_NULL || addr < 0 || addr > MEM_TOP || !(rtx_pb[addr] & PB_POS)) return false;
    int32_t c;
    if (!pmap_get(&posm, addr, pos, &c)) return false;
    return c == NOCHECK || c == mem[addr].b32.s0;
}

/* native words: a position per UTF-16 unit, by node */
typedef struct { uint64_t *pos; int n; } wpos;
static wpos *words = NULL;
static int nwords = 0, capwords = 0;
static rmap word_map;           /* node → index into words */

static void set_word_pos(int32_t node, const uint64_t *pos, int n) {
    if (nwords == capwords) { capwords = capwords ? capwords * 2 : 1024; words = xrealloc(words, sizeof(wpos) * capwords); }
    words[nwords].pos = xmalloc(sizeof(uint64_t) * (n ? n : 1));
    memcpy(words[nwords].pos, pos, sizeof(uint64_t) * n);
    words[nwords].n = n;
    rtx_pb[node] |= PB_POS;
    rmap_put(&word_map, node, nwords);
    nwords++;
}
static wpos *get_word_pos(int32_t node) {
    int64_t v;
    if (!rtx_pb || !(rtx_pb[node] & PB_POS) || !rmap_get(&word_map, node, &v)) return NULL;
    if (words[v].n != NATIVE_NODE_length(node)) return NULL;
    return &words[v];
}

/* macro call sites, by input-stack level */
typedef struct { uint64_t pos; int32_t start; bool set; bool exact; bool own; } origin;
static origin *origins = NULL;
static int norigins = 0;
static uint64_t macro_cs_pos = 0;
static bool macro_cs_known = false, macro_cs_own = false;
/* (active characters) the origin via_origin and tok_pos found last is an "own" one: see rtx_macro_enter */
static bool via_own = false, tok_own = false;

static unsigned char *user_tags = NULL;
static int n_user_tags = 0;

static uint64_t ended_call;           /* a peek after a call (rtx_list_end) */
static int ended_tag, ended_line, ended_col;
static bool ended_known = false;

static void srcpos_reset(void) {
    ended_known = false;
    if (user_tags) memset(user_tags, 0, n_user_tags);
    free(rtx_pb); rtx_pb = NULL;
    free(posm.k); free(posm.v); free(posm.c); memset(&posm, 0, sizeof posm);
    for (int i = 0; i < nwords; i++) free(words[i].pos);
    free(words); words = NULL; nwords = capwords = 0;
    rmap_free(&word_map);
    free(origins); origins = NULL; norigins = 0;
    rtx_tok_node = TEX_NULL;
    if (rtx_capture_flags & RTX_SRCPOS) rtx_pb = xcalloc(MEM_TOP + 1, 1);
}

static input_state_t *level(int k) { return k == input_ptr ? &cur_input : &input_stack[k]; }

static bool user_tag(int tag);

/* the outermost macro called from a file, on the input stack now; its span
 * the widest extent the levels made by that call know (an inner macro of
 * the call that read more arguments from the line knows more) */
static bool via_origin(uint64_t *pos) {
    bool found = false;
    int at = -1;
    via_own = false;
    for (int k = input_ptr; k >= 0; k--) {
        input_state_t *l = level(k);
        if (l->state != TOKEN_LIST) {
            /* no call site known: where the document's file is being read now */
            if (!found && user_tag(l->synctex_tag) && l->loc > l->start) {
                *pos = mkpos(l->synctex_tag, line, l->loc - l->start - 1, true);
                found = true;
            }
            break;
        }
        if (k < norigins && origins[k].set && origins[k].start == l->start) {
            *pos = origins[k].pos; found = true; at = k;
            if (origins[k].exact || origins[k].own) break;      /* the innermost call whose own token has a position (or that stands for itself: an active character) */
        }
    }
    if (at >= 0 && origins[at].own) { via_own = true; return true; }     /* (an active character) */
    if (at >= 0) {
        int span = POS_SPAN(*pos);
        for (int k = input_ptr; k >= at; k--)
            if (k < norigins && origins[k].set && origins[k].start == level(k)->start
                && POS_BASE(origins[k].pos) == POS_BASE(*pos) && POS_SPAN(origins[k].pos) > span) span = POS_SPAN(origins[k].pos);
        *pos = with_span(*pos, span);
    }
    return found;
}

/* Files whose text counts as the document's: .tex (not pgf's .code.tex)
 * and .bbl. A character from a package, a class, the .aux (a \ref's
 * number) is placed at the document's macro call that brought it instead. */
static void note_user_tag(int tag, str_number name) {
    if (tag < 0 || tag > (1 << 20)) return;
    if (tag >= n_user_tags) {
        int n = tag * 2 + 16;
        user_tags = xrealloc(user_tags, n);
        memset(user_tags + n_user_tags, 0, n - n_user_tags);
        n_user_tags = n;
    }
    char *s = gettexstring(name);
    size_t l = strlen(s);
    bool tex = l > 4 && !strcmp(s + l - 4, ".tex") && !(l > 9 && !strcmp(s + l - 9, ".code.tex"));
    bool bbl = l > 4 && !strcmp(s + l - 4, ".bbl");
    user_tags[tag] = tex || bbl;
    free(s);
}
static bool user_tag(int tag) { return tag > 0 && tag < n_user_tags && user_tags[tag]; }

/* \char: its number is scanned with a token of lookahead; the character's
 * position is the \char's, noted before */
static uint64_t char_num_pos;
static int char_num_state = 0;          /* 0 none, 1 known, 2 unknown */
static bool tok_pos(uint64_t *pos);
void rtx_char_num(void) {
    char_num_state = tok_pos(&char_num_pos) ? 1 : 2;
}
static bool take_char_num(uint64_t *pos, bool *known) {
    if (!char_num_state) return false;
    *known = char_num_state == 1; *pos = char_num_pos; char_num_state = 0;
    return true;
}

/* the position of the token get_next returned last */
static bool tok_pos(uint64_t *pos) {
    if (rtx_tok_node == TEX_NULL) {
        if (rtx_tok_tag <= 0 || rtx_tok_tag > 0xFFFF) return false;
        if (!user_tag(rtx_tok_tag)) return false;
        *pos = mkpos(rtx_tok_tag, rtx_tok_line, rtx_tok_col, false);
        return true;
    }
    tok_own = false;
    if (getpos(rtx_tok_node, pos)) return true;
    if (via_origin(pos)) {
        if (via_own) { tok_own = true; *pos = POS_BASE(*pos) & ~POS_VIA; return true; }   /* (an active character) */
        *pos |= POS_VIA; return true;
    }
    return false;
}

/* only a token's own position is kept with it: a macro call site ("via")
 * depends on where the token is used, and is found then */
static bool tok_own_pos(int32_t t, uint64_t *pos);
void rtx_tokpos(int32_t q) {
    uint64_t p;
    if (tok_own_pos(mem[q].b32.s0, &p)) setpos(q, p, mem[q].b32.s0);
    else rtx_pb[q] &= ~PB_POS;      /* a word from the free list keeps no stale position (hardening's 0006) */
}

/* A delimited argument's partial match of its delimiter failed (TeX §397): TeX
 * puts the matched tokens into the argument as copies of the delimiter's, not
 * the ones read (verbatim's '\' before anything but \end{verbatim}). When
 * they were characters just read from a file line, they are the characters
 * before the current token, one column each. */
static int partial_len = 0, partial_k = 0;
static bool partial_ok = false;
static uint64_t partial_base;
void rtx_partial_begin(int32_t s, int32_t r) {
    partial_len = 0; partial_k = 0;
    for (int32_t t = s; t != r && t != TEX_NULL; t = LLIST_link(t)) partial_len++;
    partial_ok = rtx_tok_node == TEX_NULL && tok_pos(&partial_base);
}
void rtx_partial_copy(int32_t q) {
    int k = partial_k++;
    if (!partial_ok || mem[q].b32.s0 >= CS_TOKEN_FLAG) return;
    int col = POS_COL(partial_base) - (partial_len - k);
    if (col >= 0) setpos(q, mkpos(POS_TAG(partial_base), POS_LINE(partial_base), col, false), mem[q].b32.s0);
}

/* (active characters) An active character above 127 read from the document stands for itself: what its
 * definition typesets is placed at the character, not "via" a call (rtx-tfm-unicode.sty makes "ń" a
 * command that gives T1's ń in an 8-bit font; inputenc's characters on pdfTeX are the same). Its
 * origin is "own": glyphs made under it get its position as their own, length one. */
void rtx_macro_enter(void) {
    tok_own = false;
    macro_cs_known = tok_pos(&macro_cs_pos);
    macro_cs_own = macro_cs_known && (tok_own || (!(macro_cs_pos & POS_VIA) && cur_cs >= ACTIVE_BASE + 128 && cur_cs < SINGLE_BASE));
}

static void origins_fit(void) {
    if (input_ptr >= norigins) {
        int n = stack_size + 1;
        origins = xrealloc(origins, sizeof(origin) * n);
        for (int i = norigins; i < n; i++) origins[i].set = false;
        norigins = n;
    }
}

/* the source characters of a token: a control sequence's backslash and name */
static int tok_len(int32_t t) {
    if (t < CS_TOKEN_FLAG) return 1;
    int32_t p = t - CS_TOKEN_FLAG;
    if (p < SINGLE_BASE) return 1;                     /* an active character */
    if (p < NULL_CS) return 2;                         /* \x */
    if (p >= HASH_BASE && (p < UNDEFINED_CONTROL_SEQUENCE || p > EQTB_SIZE) && p <= hash_top && hash[p].s1 > 0)
        return 1 + length(hash[p].s1);              /* (e-TeX's extra hash is above eqtb) */
    return 1;
}

/* How far base's line has been read: the column after the token read last,
 * when that token is on base's line (from the file, or a token of an
 * argument with its own position); else -1. A call's extent: \name and the
 * arguments it has read so far. */
static int read_end(uint64_t base) {
    if (rtx_tok_node == TEX_NULL) {
        if (rtx_tok_tag != POS_TAG(base) || rtx_tok_line != POS_LINE(base)) return -1;
        for (int k = input_ptr; k >= 0; k--) {
            input_state_t *l = level(k);
            if (l->state != TOKEN_LIST) return l->synctex_tag == rtx_tok_tag ? l->loc - l->start : -1;
        }
        return -1;
    }
    uint64_t q;
    if (!getpos(rtx_tok_node, &q) || POS_VIA_OF(q) || POS_TAG(q) != POS_TAG(base) || POS_LINE(q) != POS_LINE(base)) return -1;
    return POS_COL(q) + tok_len(mem[rtx_tok_node].b32.s0);
}
static uint64_t extend(uint64_t pos) {
    int span = read_end(pos) - POS_COL(pos);
    return span > POS_SPAN(pos) ? with_span(pos, span) : pos;
}

/* any token list begins: what it produces is due to the token read last (a
 * \the, an \everypar's paragraph, a backed-up token) – unless a macro, below */
void rtx_list_push(void) {
    origins_fit();
    origins[input_ptr].set = false;
    uint64_t p;
    origins[input_ptr].own = false;
    if (tok_pos(&p)) { origins[input_ptr].set = true; origins[input_ptr].pos = extend(p | POS_VIA); origins[input_ptr].start = cur_input.start; origins[input_ptr].exact = !(p & POS_VIA); origins[input_ptr].own = tok_own; }
}

void rtx_macro_push(void) {
    origins_fit();
    origins[input_ptr].set = macro_cs_known;
    origins[input_ptr].exact = macro_cs_known && !(macro_cs_pos & POS_VIA);
    origins[input_ptr].pos = macro_cs_known ? (macro_cs_own ? macro_cs_pos | POS_VIA : extend(macro_cs_pos | POS_VIA)) : 0;
    origins[input_ptr].own = macro_cs_own;
    origins[input_ptr].start = cur_input.start;
}

/* back_input: TeX first ends the token lists that are used up – the macro
 * that made the token, often (\csname…\endcsname backs up the name it made:
 * a T1 accent's composite, \ss) – so the token's origin is taken before, and
 * given to the level of the backed-up token after */
/* The own position of token t, read last or a few tokens before (back_input
 * often backs up another than the last: \futurelet's second, \expandafter's
 * first): the newest of the tokens read that is t. A file token is checked
 * against the line still in the buffer; one from an earlier line has none. */
rtx_tokrec rtx_ring[RTX_RING];
unsigned rtx_ring_i = 0;
static bool rec_is(const rtx_tokrec *e, int32_t t, uint64_t *pos) {
    if (e->node != TEX_NULL) {
        if (e->node < 0 || e->node > MEM_TOP || mem[e->node].b32.s0 != t) return false;
        if (!getpos(e->node, pos)) *pos = 0;
        return true;
    }
    if (e->line != line || e->tag <= 0) return false;
    for (int k = input_ptr; k >= 0; k--) {
        input_state_t *l = level(k);
        if (l->state == TOKEN_LIST) continue;
        if (l->synctex_tag != e->tag || l->start + e->col > l->limit) return false;
        UnicodeScalar c = buffer[l->start + e->col];
        bool same;
        if (t < CS_TOKEN_FLAG) same = t % MAX_CHAR_VAL == c;
        else if (t - CS_TOKEN_FLAG < SINGLE_BASE) same = t - CS_TOKEN_FLAG - ACTIVE_BASE == c;   /* an active character */
        else same = CAT_CODE(c) == ESCAPE;
        if (!same) return false;
        *pos = user_tag(e->tag) ? mkpos(e->tag, e->line, e->col, false) : 0;
        return true;
    }
    return false;
}
static bool tok_own_pos(int32_t t, uint64_t *pos) {
    rtx_tokrec cur = { rtx_tok_node, rtx_tok_col, rtx_tok_line, rtx_tok_tag };
    if (rec_is(&cur, t, pos)) return *pos != 0;
    for (unsigned k = 0; k < RTX_RING; k++)
        if (rec_is(&rtx_ring[(rtx_ring_i - k) & (RTX_RING - 1)], t, pos)) return *pos != 0;
    return false;
}

/* A peek after a call: a macro's last tokens read the next token from the file
 * (\futurelet in \@ifnextchar, \@ifstar, xparse's optional arguments) and back it
 * up. By then the call's token lists have ended (get_next ends a used-up list
 * before it reads below), so nothing on the input stack says which call is still
 * going on: what the backed-up token leads to (\sqrt's \sqrtsign, a starred
 * form's body) was placed at the peeked character. The call is remembered when
 * its outermost list ends, and the next token read from the file after it (on
 * the same line, only blanks between) that is backed up belongs to it. */
void rtx_list_end(void) {
    if (input_ptr < 1 || input_stack[input_ptr - 1].state == TOKEN_LIST) return;    /* not the outermost list */
    ended_known = input_ptr < norigins && origins[input_ptr].set && origins[input_ptr].start == cur_input.start;
    if (ended_known) {
        input_state_t *f = &input_stack[input_ptr - 1];
        ended_call = origins[input_ptr].pos;
        ended_tag = f->synctex_tag; ended_line = line; ended_col = f->loc - f->start;
    }
}

/* the token just read from the file is the first after the ended call: on its
 * line, nothing but blanks between (TeX skips a space after a control word) */
static bool first_after_end(void) {
    if (rtx_tok_tag != ended_tag || rtx_tok_line != ended_line || rtx_tok_col < ended_col) return false;
    for (int c = ended_col; c < rtx_tok_col; c++)
        if (CAT_CODE(buffer[cur_input.start + c]) != SPACER) return false;
    return true;
}

static uint64_t back_pos, back_org;
static bool back_known = false, back_org_known = false, back_org_own = false;
void rtx_back_begin(void) {
    /* the token's own position (from the file, or its node's) */
    back_known = tok_own_pos(cur_tok, &back_pos);
    /* and the call it is read for: the level's origin, never "exact" – the
     * token's own position is not the cause of what is made after it (a
     * peeked `{` of \sqrt{y} is not where \sqrt's sign comes from) */
    if (cur_input.state != TOKEN_LIST && rtx_tok_node == TEX_NULL && ended_known && first_after_end()) {
        back_org = ended_call;          /* a peek after a call: the call's (above) */
        back_org_known = true;
        back_org_own = false;
    } else { back_org_known = via_origin(&back_org); back_org_own = back_org_known && via_own; }   /* own: an active character's */
}
/* the backed-up token keeps its own position (a via one is found where it is used) */
void rtx_back_tok(int32_t q) { if (back_known && !(back_pos & POS_VIA)) setpos(q, back_pos, mem[q].b32.s0); }
void rtx_back_push(void) {
    origins_fit();
    origins[input_ptr].set = back_org_known;
    origins[input_ptr].exact = false;
    origins[input_ptr].pos = back_org_known ? back_org | POS_VIA : 0;
    origins[input_ptr].own = back_org_own;     /* a token backed up after an own call ended (\\ss: \\csname's result) */
    origins[input_ptr].start = cur_input.start;
}

/* a math character field (a noad's nucleus, a script) set from the token just read */
void rtx_fieldpos(int32_t field) {
    uint64_t p;
    bool k = false;
    if ((take_char_num(&p, &k) ? k : tok_pos(&p))) setpos(field, p, NOCHECK);
}

/* a token list copied (\the of a token register): each copy keeps its token's
 * position; a word reused without get_avail loses a stale one */
void rtx_copytok(int32_t from, int32_t to) {
    uint64_t p;
    if (getpos(from, &p)) setpos(to, p, mem[to].b32.s0);
    else rtx_pb[to] &= ~PB_POS;
}

void rtx_copypos(int32_t from, int32_t to) {
    uint64_t p;
    if (getpos(from, &p)) setpos(to, p, NOCHECK);
}

/* mlist_to_hlist made the noad q into new_hlist(q): its characters (and
 * glyphs) that have no position of their own get the noad's */
static void mark_list(int32_t p, uint64_t pos) {
    for (; p != TEX_NULL; p = LLIST_link(p)) {
        if (is_char_node(p)) { if (!(rtx_pb[p] & PB_POS)) setpos(p, pos, mem[p].b32.s0); continue; }
        switch (NODE_type(p)) {
        case HLIST_NODE: case VLIST_NODE: mark_list(BOX_list_ptr(p), pos); break;
        case WHATSIT_NODE:
            if (NODE_subtype(p) == GLYPH_NODE && !(rtx_pb[p] & PB_POS)) setpos(p, pos, NOCHECK);
            break;
        default: break;
        }
    }
}
void rtx_noad_done(int32_t q) {
    uint64_t pos;
    /* (a \left or \right noad's field is its delimiter until the second pass) */
    if (NODE_type(q) == LEFT_NOAD || NODE_type(q) == RIGHT_NOAD) return;
    if (getpos(q + 1, &pos)) mark_list(mem[q + 1].b32.s1, pos);
}
/* make_left_right has put the delimiter's box where the field was */
void rtx_lr_done(int32_t q) {
    uint64_t pos;
    if (getpos(q + 1, &pos)) mark_list(mem[q + 1].b32.s1, pos);
}
/* a radical's sign, a math accent: made from the noad's delimiter or accent
 * field (its command's position), before the noad's nucleus marks the rest */
void rtx_mark_from(int32_t field, int32_t list) {
    uint64_t pos;
    if (list != TEX_NULL && getpos(field, &pos)) mark_list(list, pos);
}

/* ── attributes (RTX_ATTRS, patch 0005) ───────────────────────────────── */
/* \attribute909..917 are \count32000..32008 (rtx-xetex.sty: \count(31091+N),
 * the top of the range, clear of LaTeX's allocator), e-TeX sparse registers:
 * read with find_sa_element only when one may have changed (sa_w_def, gsa_w_def, sa_restore say so), else
 * the last tuple's id is reused. Tuples are interned; values <= 0 are "unset"
 * (LuaTeX's unset attribute), stored as 0. */
#define ATTR_FIRST 32000   /* \attribute909 */
#define ATTR_N 9           /* 909-916: reflowtex.sty; 917: \addvspace (rtx-xetex.sty) */
#define ATTR_MAX 0x7FFF
uint16_t *rtx_ab = NULL;
bool rtx_attr_dirty = true;
uint16_t rtx_attr_cur = 0;
static int32_t (*asets)[ATTR_N] = NULL;   /* id -> tuple; id 0 = all unset */
static int nasets = 0, capasets = 0, aoverflow = 0;
static uint16_t *ahash = NULL;            /* open addressing over tuple ids */
static uint32_t ahcap = 0;
typedef struct { uint16_t *a; int n; } wattr;
static wattr *wattrs = NULL;              /* per-character ids of mixed words */
static int nwattrs = 0, capwattrs = 0;
static rmap wattr_map;                    /* node -> index into wattrs */
static bool is_word_node(int32_t p) {
    return p != TEX_NULL && !is_char_node(p) && NODE_type(p) == WHATSIT_NODE
        && (NODE_subtype(p) == NATIVE_WORD_NODE || NODE_subtype(p) == NATIVE_WORD_NODE_AT);
}

static uint32_t ahash_of(const int32_t *v) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < ATTR_N; i++) h = (h ^ (uint32_t) v[i]) * 16777619u;
    return h;
}
static uint16_t aset_intern(const int32_t *v) {
    bool any = false;
    for (int i = 0; i < ATTR_N; i++) if (v[i]) any = true;
    if (!any) return 0;
    if ((uint32_t) (nasets + 1) * 2 > ahcap) {
        ahcap = ahcap ? ahcap * 2 : 1024;
        free(ahash); ahash = xcalloc(ahcap, sizeof(uint16_t));
        for (int id = 1; id < nasets; id++) {
            uint32_t h = ahash_of(asets[id]) & (ahcap - 1);
            while (ahash[h]) h = (h + 1) & (ahcap - 1);
            ahash[h] = id;
        }
    }
    uint32_t h = ahash_of(v) & (ahcap - 1);
    while (ahash[h]) {
        if (!memcmp(asets[ahash[h]], v, sizeof(int32_t) * ATTR_N)) return ahash[h];
        h = (h + 1) & (ahcap - 1);
    }
    if (nasets > ATTR_MAX) { aoverflow++; return 0; }
    if (nasets == capasets) { capasets = capasets ? capasets * 2 : 256; asets = xrealloc(asets, sizeof *asets * capasets); }
    memcpy(asets[nasets], v, sizeof(int32_t) * ATTR_N);
    ahash[h] = nasets;
    return nasets++;
}
uint16_t rtx_attr_now(void) {
    int32_t v[ATTR_N], saved = cur_ptr;
    for (int i = 0; i < ATTR_N; i++) {
        find_sa_element(INT_VAL, ATTR_FIRST + i, false);
        v[i] = cur_ptr == TEX_NULL ? 0 : mem[cur_ptr + 2].b32.s1;
        if (v[i] < 0) v[i] = 0;
    }
    cur_ptr = saved;
    rtx_attr_dirty = false;
    return rtx_attr_cur = aset_intern(v);
}
static void attrs_reset(void) {
    free(rtx_ab); rtx_ab = NULL;
    free(ahash); ahash = NULL; ahcap = 0;
    free(asets); asets = NULL; capasets = 0;
    for (int i = 0; i < nwattrs; i++) free(wattrs[i].a);
    free(wattrs); wattrs = NULL; nwattrs = capwattrs = 0;
    rmap_free(&wattr_map);
    aoverflow = 0; rtx_attr_cur = 0; rtx_attr_dirty = true;
    if (rtx_capture_flags & RTX_ATTRS) {
        int32_t zero[ATTR_N] = { 0 };
        capasets = 256; asets = xmalloc(sizeof *asets * capasets);
        memcpy(asets[0], zero, sizeof zero); nasets = 1;
    }
}
static wattr *unit_attrs(int32_t node) {
    int64_t v;
    if (!(rtx_ab[node] & 0x8000) || !rmap_get(&wattr_map, node, &v)) return NULL;
    return wattrs[v].n == NATIVE_NODE_length(node) ? &wattrs[v] : NULL;
}
static uint16_t unit_attr(int32_t node, int i) {
    wattr *w = unit_attrs(node);
    return w && i < w->n ? w->a[i] : rtx_ab[node] & ATTR_MAX;
}
/* a native word's ids per character: one id when they agree */
static void set_unit_attrs(int32_t node, const uint16_t *a, int n) {
    bool one = true;
    for (int i = 1; i < n; i++) if (a[i] != a[0]) one = false;
    if (one) { rtx_ab[node] = n ? a[0] : 0; return; }
    if (nwattrs == capwattrs) { capwattrs = capwattrs ? capwattrs * 2 : 256; wattrs = xrealloc(wattrs, sizeof(wattr) * capwattrs); }
    wattrs[nwattrs].a = xmalloc(sizeof(uint16_t) * n);
    memcpy(wattrs[nwattrs].a, a, sizeof(uint16_t) * n);
    wattrs[nwattrs].n = n;
    rmap_put(&wattr_map, node, nwattrs++);
    rtx_ab[node] = 0x8000 | a[0];
}
/* copy_node_list, hpack's joined words: the copy keeps the original's */
void rtx_attr_copy(int32_t from, int32_t to) {
    wattr *w = rtx_ab[from] & 0x8000 ? unit_attrs(from) : NULL;
    if (w) set_unit_attrs(to, w->a, w->n);
    else rtx_ab[to] = rtx_ab[from] & ATTR_MAX;
    /* and what the capture knows of it: an interword space stays one when
     * \usebox copies it (the flag is on whenever the editor captures) */
    if (rtx_nb) rtx_nb[to] |= rtx_nb[from] & (RTX_NB_SPACE | RTX_NB_ITALIC);
}
/* the words from `start` on, the first `pre` characters of which were the
 * word XeTeX's main loop merged this run into (its ids in old) */
static void attrs_after_merge(int32_t start, const uint16_t *old, int pre, uint16_t now) {
    int total = 0, k = 0;
    for (int32_t p = start; p != TEX_NULL; p = LLIST_link(p)) if (is_word_node(p)) total += NATIVE_NODE_length(p);
    uint16_t *all = xmalloc(sizeof(uint16_t) * (total + 1));
    for (int i = 0; i < total; i++) all[i] = i < pre ? old[i] : now;
    for (int32_t p = start; p != TEX_NULL; p = LLIST_link(p)) if (is_word_node(p)) {
        set_unit_attrs(p, all + k, NATIVE_NODE_length(p));
        k += NATIVE_NODE_length(p);
    }
    free(all);
}
/* mlist_to_hlist: what a noad became gets the noad's tuple, as LuaTeX's
 * glyphs get their noad's attributes (made now, they carry the tuple in
 * force where the formula ends); nested noads have set theirs already */
static void noad_list(int32_t p, uint16_t from, uint16_t to) {
    for (; p != TEX_NULL; p = LLIST_link(p)) {
        if ((rtx_ab[p] & ATTR_MAX) == from && !(rtx_ab[p] & 0x8000)) rtx_ab[p] = to;
        if (!is_char_node(p) && (NODE_type(p) == HLIST_NODE || NODE_type(p) == VLIST_NODE)) noad_list(BOX_list_ptr(p), from, to);
    }
}
void rtx_noad_attrs(int32_t q) {
    uint16_t now = rtx_attr_dirty ? rtx_attr_now() : rtx_attr_cur, a = rtx_ab[q] & ATTR_MAX;
    if (a != now) noad_list(mem[q + 1].b32.s1, now, a);
}
static void jaset(uint16_t id) {
    jopen('[');
    for (int i = 0; i < ATTR_N; i++) if (asets[id][i]) { jnum(909 + i); jnum(asets[id][i]); }
    jclose(']');
}
/* "a": the attributes set on the node as pairs [909, value, …] (values > 0);
 * "au": a mixed word's, as runs [[chars, pairs], …] */
static void jattrs(int32_t p) {
    uint16_t v = rtx_ab[p];
    wattr *w = v & 0x8000 ? unit_attrs(p) : NULL;
    if (!w) { if (v & ATTR_MAX) { jkey("a"); jaset(v & ATTR_MAX); } return; }
    jkey("au"); jopen('[');
    for (int i = 0; i < w->n; ) {
        int n = 1;
        while (i + n < w->n && w->a[i + n] == w->a[i]) n++;
        jopen('['); jnum(n); jaset(w->a[i]); jclose(']');
        i += n;
    }
    jclose(']');
}

/* ── native words: per character, through mapping and merging ── */
static uint64_t *npos = NULL;              /* parallel to native_text while collecting */
static int npos_cap = 0;
static uint16_t *raw_txt = NULL;
static uint64_t *raw_pos = NULL;
static int raw_n = 0, raw_cap = 0;
static int32_t saved_tail = TEX_NULL, saved_prev = TEX_NULL;
static uint64_t *old_pos = NULL;
static int old_n = 0;
static bool collecting = false;
static uint16_t *old_ua = NULL;           /* RTX_ATTRS: that word's ids, per character */
static int old_un = 0;
static uint16_t run_aid = 0;

void rtx_native_char(void) {
    uint64_t p = 0;
    bool known = false;
    if (!take_char_num(&p, &known)) known = tok_pos(&p);
    int need = native_len + 2;
    if (need > npos_cap) { npos_cap = need * 2; npos = xrealloc(npos, sizeof(uint64_t) * npos_cap); }
    npos[native_len] = known ? p : 0;
    npos[native_len + 1] = known ? p : 0;
}

void rtx_native_collected(void) {
    raw_n = native_len;
    if (raw_n > raw_cap) { raw_cap = raw_n * 2; raw_txt = xrealloc(raw_txt, 2 * raw_cap); raw_pos = xrealloc(raw_pos, 8 * raw_cap); }
    for (int i = 0; i < raw_n; i++) { raw_txt[i] = native_text[i]; raw_pos[i] = i < npos_cap ? npos[i] : 0; }
    saved_tail = cur_list.tail;
    saved_prev = TEX_NULL;
    old_n = 0;
    old_un = 0;
    run_aid = rtx_ab ? (rtx_attr_dirty ? rtx_attr_now() : rtx_attr_cur) : 0;
    /* XeTeX may merge this word into the native word before it (same font,
     * nothing but the word between): remember that word and its positions */
    int32_t t = saved_tail;
    if (t != TEX_NULL && !is_char_node(t) && NODE_type(t) == WHATSIT_NODE
        && (NODE_subtype(t) == NATIVE_WORD_NODE || NODE_subtype(t) == NATIVE_WORD_NODE_AT)) {
        wpos *w = get_word_pos(t);
        if (w) { old_pos = xrealloc(old_pos, 8 * (w->n + 1)); memcpy(old_pos, w->pos, 8 * w->n); old_n = w->n; }
        else { old_pos = xrealloc(old_pos, 8 * (NATIVE_NODE_length(t) + 1)); memset(old_pos, 0, 8 * NATIVE_NODE_length(t)); old_n = NATIVE_NODE_length(t); }
        if (rtx_ab) {
            old_un = NATIVE_NODE_length(t);
            old_ua = xrealloc(old_ua, sizeof(uint16_t) * (old_un + 1));
            for (int i = 0; i < old_un; i++) old_ua[i] = unit_attr(t, i);
        }
        int32_t q = cur_list.head;
        while (q != TEX_NULL && LLIST_link(q) != t) q = LLIST_link(q);
        saved_prev = q;
    }
    rtx_pb[saved_tail] |= 2;                     /* still there at the end: not merged */
    collecting = true;
}

static bool is_word(int32_t p) {
    return p != TEX_NULL && !is_char_node(p) && NODE_type(p) == WHATSIT_NODE
        && (NODE_subtype(p) == NATIVE_WORD_NODE || NODE_subtype(p) == NATIVE_WORD_NODE_AT);
}

void rtx_native_done(void) {
    if (!collecting) return;
    collecting = false;
    bool merged = !(rtx_pb[saved_tail] & 2);
    rtx_pb[saved_tail] &= ~2;
    int32_t start = merged ? (saved_prev == TEX_NULL ? TEX_NULL : LLIST_link(saved_prev)) : LLIST_link(saved_tail);
    if (start == TEX_NULL) return;
    if (rtx_ab && merged && old_un > 0) attrs_after_merge(start, old_ua, old_un, run_aid);
    /* the text of the words made from this run */
    int total = 0;
    for (int32_t p = start; p != TEX_NULL; p = LLIST_link(p)) if (is_word(p)) total += NATIVE_NODE_length(p);
    int pre = merged ? old_n : 0;
    int mapped_n = total - pre;
    if (mapped_n < 0) return;
    uint16_t *mt = xmalloc(2 * (mapped_n + 1));
    uint64_t *all = xmalloc(8 * (total + 1));
    int k = 0;
    for (int32_t p = start; p != TEX_NULL; p = LLIST_link(p)) if (is_word(p))
        for (int i = 0; i < NATIVE_NODE_length(p); i++, k++) if (k >= pre) mt[k - pre] = NATIVE_NODE_text(p)[i];
    for (int i = 0; i < pre; i++) all[i] = old_pos[i];
    /* raw (as typed) → mapped (after the font's mapping: -- → –, `` → “):
     * equal units pair up; a mapped unit that differs takes the raw units up
     * to the next that equals the next mapped unit, and the first one's
     * position (its extent is recovered by the converter from the source) */
    int i = 0, j = 0;
    while (j < mapped_n) {
        if (i < raw_n && raw_txt[i] == mt[j]) { all[pre + j] = raw_pos[i]; i++; j++; continue; }
        all[pre + j] = i < raw_n ? raw_pos[i] : (i > 0 ? raw_pos[i - 1] : 0);
        int i2 = i + 1;
        if (j + 1 < mapped_n) while (i2 < raw_n && raw_txt[i2] != mt[j + 1]) i2++;
        else i2 = raw_n;
        if (i < raw_n && i2 - i > 1 && !(all[pre + j] & POS_VIA)) all[pre + j] = with_span(all[pre + j], i2 - i);
        i = i2 > i ? i2 : i + 1;
        j++;
    }
    /* hand each word its slice */
    k = 0;
    for (int32_t p = start; p != TEX_NULL; p = LLIST_link(p)) if (is_word(p)) {
        set_word_pos(p, all + k, NATIVE_NODE_length(p));
        k += NATIVE_NODE_length(p);
    }
    free(mt); free(all);
}

/* hpack joins adjacent words of one font (and words split by an unused
 * discretionary) into one node: its positions are theirs, in order */
void rtx_merge_words(int32_t merged, int32_t chain) {
    int n = NATIVE_NODE_length(merged), k = 0;
    uint64_t *all = xcalloc(n + 1, sizeof(uint64_t));
    bool any = false;
    for (int32_t p = chain; p != TEX_NULL && k < n; p = LLIST_link(p)) {
        if (!is_word(p)) continue;
        wpos *w = get_word_pos(p);
        for (int i = 0; i < NATIVE_NODE_length(p) && k < n; i++, k++) if (w) { all[k] = w->pos[i]; any = true; }
    }
    if (any) set_word_pos(merged, all, n);
    free(all);
    if (rtx_ab) {
        uint16_t *ua = xmalloc(sizeof(uint16_t) * (n + 1));
        k = 0;
        for (int32_t p = chain; p != TEX_NULL && k < n; p = LLIST_link(p))
            if (is_word(p)) for (int i = 0; i < NATIVE_NODE_length(p) && k < n; i++, k++) ua[k] = unit_attr(p, i);
        set_unit_attrs(merged, ua, k);
        free(ua);
    }
}

/* ── 8-bit (TFM) text: per character node ── */
/* The main loop's character (lig_stack, made at main_loop and at its
 * lookahead) is the token just read: its position goes on the node, which the
 * lig/kern program then appends to the list or to a ligature's components
 * (a ligature's position is its components', jlig_src). A word the line
 * breaker hyphenates is rebuilt by reconstitute() from hu[]: the positions of
 * the old nodes are kept by index (hu_pos) and handed to the new nodes. */
#define PB_BREAK 4          /* a char node that is a discretionary's hyphen: a break point */
static void rtx_tfm_set(int32_t p, bool known, uint64_t pos) {
    rtx_pb[p] &= ~(PB_POS | PB_BREAK);     /* the node may be a reused word (avail, inline) */
    if (known) setpos(p, pos, mem[p].b32.s0);
}
void rtx_tfm_char(int32_t p) {
    uint64_t pos = 0;
    bool known = false;
    if (!take_char_num(&pos, &known)) known = tok_pos(&pos);
    rtx_tfm_set(p, known, pos);
}

/* a character node made outside the main loop from the token just read
 * (\accent's accent and accentee): a char node, or a native word of one
 * character (new_character in a native font) */
void rtx_made_char(int32_t p) {
    uint64_t pos = 0;
    bool known = false;
    if (!take_char_num(&pos, &known)) known = tok_pos(&pos);
    if (p == TEX_NULL) return;
    if (is_char_node(p)) { rtx_tfm_set(p, known, pos); return; }
    if (known && is_word_node(p)) {
        int n = NATIVE_NODE_length(p);
        uint64_t a[4] = { pos, pos, pos, pos };
        if (n >= 1 && n <= 4) set_word_pos(p, a, n);
    }
}

static uint64_t hu_pos[4097];
static bool hu_has[4097];
static int hu_hyph = -1;                   /* hu[hu_hyph] is the hyphen character for now */
void rtx_hu_from(int i, int32_t node) {
    if (i < 0 || i > 4096) return;
    if (i == 1) hu_has[0] = false;         /* hu[0] comes from the node before the word (init_list), not by index */
    hu_has[i] = getpos(node, &hu_pos[i]);
}
void rtx_hu_hyph(int i) { hu_hyph = i; }
void rtx_hu_char(int32_t p, int i) {
    rtx_pb[p] &= ~(PB_POS | PB_BREAK);
    if (i < 0 || i > 4096 || !hu_has[i]) return;
    setpos(p, hu_pos[i], mem[p].b32.s0);
    if (i == hu_hyph) rtx_pb[p] |= PB_BREAK;  /* the hyphen of a break before hu[i]: placed at hu[i], length 0 */
}
void rtx_char_copy(int32_t from, int32_t to) {
    uint64_t pos;
    rtx_pb[to] &= ~(PB_POS | PB_BREAK);
    if (getpos(from, &pos)) setpos(to, pos, mem[to].b32.s0);
}

/* JSON: a char node's position, [tag, line, col, via, length]; a ligature's:
 * its first component's, and the length to the end of its last when they
 * are on one line in order (ﬁ: 2; “ for ``: 2) */
static void jchar_src(int32_t p) {
    uint64_t pos;
    if (!getpos(p, &pos)) return;
    jkey("src"); jopen('['); jnum(POS_TAG(pos)); jnum(POS_LINE(pos)); jnum(POS_COL(pos)); jnum(POS_VIA_OF(pos));
    jnum(POS_VIA_OF(pos) ? POS_SPAN(pos) : (rtx_pb[p] & PB_BREAK) ? 0 : 1);   /* via: the call's extent */
    jclose(']');
}
static void jlig_src(int32_t lig) {
    uint64_t a, b;
    int32_t q = LIGATURE_NODE_lig_ptr(lig);
    if (q == TEX_NULL || !getpos(q, &a)) return;
    int len = POS_VIA_OF(a) ? POS_SPAN(a) : 1;
    bool brk = (rtx_pb[q] & PB_BREAK) != 0;
    if (!POS_VIA_OF(a)) {
        int last = POS_COL(a);
        for (int32_t r = LLIST_link(q); r != TEX_NULL; r = LLIST_link(r)) {
            if (!getpos(r, &b) || POS_VIA_OF(b) || POS_TAG(b) != POS_TAG(a) || POS_LINE(b) != POS_LINE(a) || POS_COL(b) <= last) break;
            if (rtx_pb[r] & PB_BREAK) continue;
            last = POS_COL(b);
        }
        len = last - POS_COL(a) + 1;
    }
    jkey("src"); jopen('['); jnum(POS_TAG(a)); jnum(POS_LINE(a)); jnum(POS_COL(a)); jnum(POS_VIA_OF(a)); jnum(brk ? 0 : len); jclose(']');
}

/* JSON: a native word's positions as runs of units at consecutive columns:
 * [[n, tag, line, col, via], …] */
static void jword_src(int32_t p) {
    wpos *w = get_word_pos(p);
    if (!w) return;
    jkey("src"); jopen('[');
    int i = 0;
    while (i < w->n) {
        uint64_t a = w->pos[i];
        int n = 1, cp = 0;
        while (i + n < w->n) {
            uint64_t b = w->pos[i + n];
            if (POS_TAG(b) != POS_TAG(a) || POS_LINE(b) != POS_LINE(a) || POS_VIA_OF(b) != POS_VIA_OF(a)) break;
            if ((POS_SPAN(a) || POS_SPAN(b)) && !(POS_VIA_OF(a) && POS_SPAN(a) == POS_SPAN(b))) break;   /* a via run: one call, one span */
            /* columns count code points; a low surrogate shares its pair's */
            int cp2 = cp + ((NATIVE_NODE_text(p)[i + n] & 0xFC00) == 0xDC00 ? 0 : 1);
            if (POS_COL(b) - POS_COL(a) != (POS_VIA_OF(a) ? 0 : cp2)) break;
            cp = cp2;
            n++;
        }
        jopen('['); jnum(n); jnum(POS_TAG(a)); jnum(POS_LINE(a)); jnum(POS_COL(a)); jnum(POS_VIA_OF(a));
        if (POS_SPAN(a)) jnum(POS_SPAN(a));
        jclose(']');
        i += n;
    }
    jclose(']');
}
static void jpos(const char *k, uint64_t p) {
    jkey(k); jopen('['); jnum(POS_TAG(p)); jnum(POS_LINE(p)); jnum(POS_COL(p)); jnum(POS_VIA_OF(p)); jclose(']');
}
static void jnode_src(int32_t p) {
    uint64_t pos;
    if (getpos(p, &pos)) jpos("src", pos);
}

/* ── node serializer ──────────────────────────────────────────────────── */

#define M_FLOW 1   /* line boxes become references (main flow, vlists in it, ins) */

typedef struct {
    bool on;           /* top level of a paragraph: hyphenate words */
    int lang, lhm, rhm;
    rmap *t82;         /* words TeX82's second pass would try */
} hyctx;

static void ser_list(int32_t p, int mode, hyctx *hy);
static void ser_node(int32_t p, int mode, hyctx *hy);

static void jspec(const char *k, int32_t g) {
    jkey(k); jopen('[');
    jnum(BOX_width(g)); jnum(GLUE_SPEC_stretch(g)); jnum(GLUE_SPEC_stretch_order(g));
    jnum(GLUE_SPEC_shrink(g)); jnum(GLUE_SPEC_shrink_order(g));
    jclose(']');
}

static int32_t hyph_code(int32_t c, int32_t hyph_idx) {
    if (hyph_idx == 0 || c > 255) return LC_CODE(c);
    if (trie_trc[hyph_idx + c] != c) return 0;
    return trie_tro[hyph_idx + c];
}
static int32_t hyph_index_for(int lang) {
    if (trie_trc[hyph_start + lang] != lang) return 0;
    return trie_trl[hyph_start + lang];
}

/* TeX §923–§965 (the pattern and exception part of hyphenate()), on a copy:
 * codes[1..hn] are hyphenation codes; returns hyf[] in out[0..hn]. */
static void rtx_hyphenate(const int32_t *codes, int hn, int lang, int lhm, int rhm, unsigned char *hyf) {
    static int32_t hc_[HYPHENATABLE_LENGTH_LIMIT + 4];
    int j, l, i;
    for (j = 0; j <= hn; j++) hyf[j] = 0;
    for (j = 1; j <= hn; j++) hc_[j] = codes[j];
    /* exceptions (§934 ff.) */
    int32_t h = hc_[1];
    hn++;
    hc_[hn] = lang;
    for (j = 2; j <= hn; j++) h = (h + h + hc_[j]) % HYPH_PRIME;
    while (true) {
        str_number k = hyph_word[h];
        if (k == 0) break;
        if (length(k) == hn) {
            pool_pointer u = str_start[k - 65536];
            for (j = 1; j <= hn; j++, u++) if (str_pool[u] != hc_[j]) break;
            if (j > hn) {
                int32_t s = hyph_list[h];
                while (s != TEX_NULL) { hyf[mem[s].b32.s0] = 1; s = LLIST_link(s); }
                hn--;
                goto found;
            }
        }
        h = hyph_link[h];
        if (h == 0) break;
        h--;
    }
    hn--;
    if (trie_trc[lang + 1] != lang) return;
    hc_[0] = 0; hc_[hn + 1] = 0; hc_[hn + 2] = max_hyph_char;
    for (j = 0; j <= hn - rhm + 1; j++) {
        trie_pointer z = trie_trl[lang + 1] + hc_[j];
        l = j;
        while (hc_[l] == trie_trc[z]) {
            if (trie_tro[z] != MIN_TRIE_OP) {
                int32_t v = trie_tro[z];
                do {
                    v = v + op_start[lang];
                    i = l - hyf_distance[v];
                    if (hyf_num[v] > hyf[i]) hyf[i] = hyf_num[v];
                    v = hyf_next[v];
                } while (v != MIN_TRIE_OP);
            }
            l++;
            z = trie_trl[z] + hc_[l];
        }
    }
found:
    for (j = 0; j <= lhm - 1 && j <= hn; j++) hyf[j] = 0;
    for (j = 0; j <= rhm - 1 && hn - j >= 0; j++) hyf[hn - j] = 0;
}

/* The hyphenation points of a native word's first run of letters (XeTeX
 * §927: leading non-letters are split off, the word ends at the first
 * non-letter), as offsets into the node's UTF-16 text. */
static void word_points(int32_t p, hyctx *hy) {
    int f = NATIVE_NODE_font(p);
    int n = NATIVE_NODE_length(p);
    uint16_t *t = NATIVE_NODE_text(p);
    int32_t hyf_ch = hyphen_char[f];
    if (hyf_ch < 0 || hyf_ch > BIGGEST_CHAR || trie_not_ready) return;
    if (hy->lhm + hy->rhm > max_hyphenatable_length()) return;
    int32_t hidx = hyph_index_for(hy->lang);
    static int32_t codes[HYPHENATABLE_LENGTH_LIMIT + 4];
    static unsigned char hyf[HYPHENATABLE_LENGTH_LIMIT + 4];
    int hn = 0, first = -1, end = -1;
    for (int l = 0; l < n; l++) {
        int32_t c = t[l];
        int units = 1;
        if (c >= 0xD800 && c < 0xDC00 && l + 1 < n) { c = 0x10000 + (c - 0xD800) * 0x400 + t[l + 1] - 0xDC00; units = 2; }
        int32_t code = hyph_code(c, hidx);
        if (code == 0) { if (hn > 0) break; l += units - 1; continue; }
        if (hn == 0) {
            first = l;
            if (!(code == c || INTPAR(uc_hyph) > 0)) return;
        }
        if (hn + units > max_hyphenatable_length()) break;
        if (units == 1) codes[++hn] = code;
        else {
            codes[++hn] = (code - 65536) / 1024 + 0xD800;
            codes[++hn] = code % 1024 + 0xDC00;
        }
        l += units - 1;
        end = l + 1;
    }
    if (hn < hy->lhm + hy->rhm) return;
    rtx_hyphenate(codes, hn, hy->lang, hy->lhm, hy->rhm, hyf);
    bool any = false;
    for (int j = hy->lhm; j <= hn - hy->rhm; j++) {
        if (hyf[j] & 1) {
            if (!any) { jkey("hy"); jopen('['); any = true; }
            jnum(first + j);
        }
    }
    if (any) { jclose(']'); jkey("hr"); jopen('['); jnum(first); jnum(end); jclose(']'); }
}

/* ── 8-bit (TFM) words: hyphenation points ─────────────────────────────
 * TeX inserts discretionaries only in line_break's second pass, after this
 * capture has written the paragraph: a native word gets its points from
 * word_points, and an 8-bit word got none, so a T1/OT1 paragraph never
 * hyphenated in the reader. A TFM word here: a run of character nodes,
 * ligatures and font kerns in one font, at the paragraph's top level; its
 * letters are the characters (a ligature's original ones); points as for a
 * native word (§923ff on its first run of letters, leading non-letters
 * skipped), written on the run's first node as "hy": letter indices (from the
 * run's first letter, 0-based) a hyphen may go before. The converter makes
 * the discretionaries. */
static int32_t tfm_hy_node = TEX_NULL, tfm_run_end = TEX_NULL;
static int tfm_hy_n = 0;
static int tfm_hy[HYPHENATABLE_LENGTH_LIMIT + 4];
static bool tfm_run_node(int32_t q, int f) {
    if (q == TEX_NULL) return false;
    if (is_char_node(q)) return CHAR_NODE_font(q) == f;
    if (NODE_type(q) == LIGATURE_NODE) return LIGATURE_NODE_lig_font(q) == f;
    return NODE_type(q) == KERN_NODE && NODE_subtype(q) == NORMAL;
}
static void tfm_word_points(int32_t p, hyctx *hy) {
    if (tfm_run_end != TEX_NULL) { if (p != tfm_run_end) return; tfm_run_end = TEX_NULL; }   /* inside a run already scanned */
    if (!(is_char_node(p) || NODE_type(p) == LIGATURE_NODE)) return;
    int f = is_char_node(p) ? CHAR_NODE_font(p) : LIGATURE_NODE_lig_font(p);
    int32_t end = p;
    while (tfm_run_node(end, f)) end = LLIST_link(end);
    tfm_run_end = end;
    tfm_hy_node = TEX_NULL; tfm_hy_n = 0;
    int32_t hyf_ch = hyphen_char[f];
    if (hyf_ch < 0 || hyf_ch > 255 || trie_not_ready) return;
    if (hy->lhm + hy->rhm > max_hyphenatable_length()) return;
    int32_t hidx = hyph_index_for(hy->lang);
    static int32_t codes[HYPHENATABLE_LENGTH_LIMIT + 4];
    static unsigned char hyf[HYPHENATABLE_LENGTH_LIMIT + 4];
    int hn = 0, first = -1, k = 0;
    bool done = false;
    for (int32_t q = p; q != end && !done; q = LLIST_link(q)) {
        int32_t one[1], *cs = one, nc = 0;
        static int32_t lig[HYPHENATABLE_LENGTH_LIMIT + 4];
        if (is_char_node(q)) { one[0] = CHAR_NODE_character(q); nc = 1; }
        else if (NODE_type(q) == LIGATURE_NODE) {
            cs = lig;
            for (int32_t r = LIGATURE_NODE_lig_ptr(q); r != TEX_NULL && nc < HYPHENATABLE_LENGTH_LIMIT; r = LLIST_link(r)) lig[nc++] = CHAR_NODE_character(r);
        }
        for (int i = 0; i < nc; i++, k++) {
            int32_t code = hyph_code(cs[i], hidx);
            if (code == 0) { if (hn > 0) { done = true; break; } continue; }
            if (hn == 0) { first = k; if (!(code == cs[i] || INTPAR(uc_hyph) > 0)) return; }
            if (hn + 1 > max_hyphenatable_length()) { done = true; break; }
            codes[++hn] = code;
        }
    }
    if (hn < hy->lhm + hy->rhm) return;
    rtx_hyphenate(codes, hn, hy->lang, hy->lhm, hy->rhm, hyf);
    for (int j = hy->lhm; j <= hn - hy->rhm; j++) if (hyf[j] & 1) tfm_hy[tfm_hy_n++] = first + j;
    if (tfm_hy_n) tfm_hy_node = p;
}
static void jtfm_points(int32_t p) {
    if (p != tfm_hy_node || !tfm_hy_n) return;
    jkey("hy"); jopen('['); for (int i = 0; i < tfm_hy_n; i++) jnum(tfm_hy[i]); jclose(']');
}

static uint16_t *glyph_ids(int32_t p) {
    FixedPoint *loc = (FixedPoint *) NATIVE_NODE_glyph_info_ptr(p);
    return (uint16_t *) (loc + NATIVE_NODE_glyph_count(p));
}

static void ser_native_word(int32_t p, hyctx *hy) {
    int f = NATIVE_NODE_font(p);
    int n = NATIVE_NODE_length(p);
    int g = NATIVE_NODE_glyph_count(p);
    uint16_t *text = NATIVE_NODE_text(p);
    FixedPoint *loc = (FixedPoint *) NATIVE_NODE_glyph_info_ptr(p);
    uint16_t *ids = glyph_ids(p);
    jcstr("t", "w");
    if (NODE_subtype(p) == NATIVE_WORD_NODE_AT) jint("at", 1);
    jint("f", f);
    jkey("s"); jutf16(text, n);
    if (rtx_pb) jword_src(p);
    jint("w", BOX_width(p)); jint("h", BOX_height(p)); jint("d", BOX_depth(p));
    jkey("gi"); jopen('['); for (int i = 0; i < g; i++) { jnum(ids[i]); note_glyph(f, ids[i]); } jclose(']');
    jkey("gx"); jopen('['); for (int i = 0; i < g; i++) jnum(loc[i].x); jclose(']');
    bool ys = false;
    for (int i = 0; i < g; i++) if (loc[i].y) ys = true;
    if (ys) { jkey("gy"); jopen('['); for (int i = 0; i < g; i++) jnum(loc[i].y); jclose(']'); }
    if (font_letter_space[f]) jint("ls", font_letter_space[f]);
    /* The same HarfBuzz call again, for what the node does not keep: the
     * clusters and the positions in font units. Only for one-direction
     * words whose glyphs come out the same. */
    if (font_area[f] == OTGR_FONT_FLAG && g > 0 && n > 0) {
        XeTeXLayoutEngine engine = (XeTeXLayoutEngine) font_layout_engine[f];
        UErrorCode err = U_ZERO_ERROR;
        UBiDi *bidi = ubidi_open();
        ubidi_setPara(bidi, (const UChar *) text, n, getDefaultDirection(engine), NULL, &err);
        UBiDiDirection dir = ubidi_getDirection(bidi);
        ubidi_close(bidi);
        if (dir != UBIDI_MIXED) {
            int m = layoutChars(engine, text, 0, n, n, dir == UBIDI_RTL);
            if (m == g) {
                uint32_t *gl = xmalloc(sizeof(uint32_t) * m), *cl = xmalloc(sizeof(uint32_t) * m);
                int32_t *xa = xmalloc(sizeof(int32_t) * m * 4);
                int32_t *ya = xa + m, *xo = xa + 2 * m, *yo = xa + 3 * m;
                getGlyphs(engine, gl);
                rtx_layout_raw(engine, m, cl, xa, ya, xo, yo);
                bool same = true;
                for (int i = 0; i < m; i++) if (gl[i] != ids[i]) same = false;
                if (same) {
                    bool any;
                    jkey("cl"); jopen('['); for (int i = 0; i < m; i++) jnum(cl[i]); jclose(']');
                    jkey("xa"); jopen('['); for (int i = 0; i < m; i++) jnum(xa[i]); jclose(']');
                    any = false; for (int i = 0; i < m; i++) if (ya[i]) any = true;
                    if (any) { jkey("ya"); jopen('['); for (int i = 0; i < m; i++) jnum(ya[i]); jclose(']'); }
                    any = false; for (int i = 0; i < m; i++) if (xo[i]) any = true;
                    if (any) { jkey("xo"); jopen('['); for (int i = 0; i < m; i++) jnum(xo[i]); jclose(']'); }
                    any = false; for (int i = 0; i < m; i++) if (yo[i]) any = true;
                    if (any) { jkey("yo"); jopen('['); for (int i = 0; i < m; i++) jnum(yo[i]); jclose(']'); }
                }
                free(gl); free(cl); free(xa);
            }
        }
    }
    if (hy && hy->on) {
        word_points(p, hy);
        int64_t v;
        if (hy->t82 && rmap_get(hy->t82, p, &v)) jint("t82", 1);
    }
}

static void ser_box_fields(int32_t p) {
    jint("w", BOX_width(p)); jint("h", BOX_height(p)); jint("d", BOX_depth(p));
    if (BOX_shift_amount(p)) jint("sh", BOX_shift_amount(p));
    if (BOX_glue_sign(p)) { jint("gsg", BOX_glue_sign(p)); jint("go", BOX_glue_order(p)); }
    if (BOX_glue_set(p) != 0.0) { jkey("gs"); jdbl(BOX_glue_set(p)); }   /* 1.0 on an overfull box */
    if (BOX_lr_mode(p)) jint("lr", BOX_lr_mode(p));
}

/* A line referred to without its contents: which glyphs give it its height
 * and depth. XeTeX measured them from FreeType's control box through floats;
 * with the glyph named, a consumer can use the outline's exact extent (what
 * LuaTeX has) and redo the interline glue the same way. "hb"/"db": a box,
 * rule or 8-bit character reaches it too (exact already). */
static void line_extents(int32_t box) {
    scaled_t H = BOX_height(box), D = BOX_depth(box);
    int nh = 0, nd = 0;
    bool hb = false, db = false;
    int32_t hs[16][3], ds[16][3];
    for (int32_t q = BOX_list_ptr(box); q != TEX_NULL; q = LLIST_link(q)) {
        if (is_char_node(q)) {
            int f = CHAR_NODE_font(q);
            b16x4 ci = FONT_CHARACTER_INFO(f, CHAR_NODE_character(q));
            if (FONT_CHARINFO_HEIGHT(f, ci) == H) hb = true;
            if (FONT_CHARINFO_DEPTH(f, ci) == D) db = true;
            continue;
        }
        switch (NODE_type(q)) {
        case HLIST_NODE: case VLIST_NODE: case RULE_NODE:
            if (BOX_height(q) - (NODE_type(q) == RULE_NODE ? 0 : BOX_shift_amount(q)) == H) hb = true;
            if (BOX_depth(q) + (NODE_type(q) == RULE_NODE ? 0 : BOX_shift_amount(q)) == D) db = true;
            break;
        case WHATSIT_NODE:
            if (NODE_subtype(q) == NATIVE_WORD_NODE || NODE_subtype(q) == NATIVE_WORD_NODE_AT) {
                int f = NATIVE_NODE_font(q);
                if (font_area[f] != OTGR_FONT_FLAG || INTPAR(xetex_use_glyph_metrics) <= 0) {
                    if (BOX_height(q) == H) hb = true;
                    if (BOX_depth(q) == D) db = true;
                    break;
                }
                FixedPoint *loc = (FixedPoint *) NATIVE_NODE_glyph_info_ptr(q);
                uint16_t *ids = glyph_ids(q);
                for (int i = 0; i < NATIVE_NODE_glyph_count(q); i++) {
                    GlyphBBox bb;
                    if (getCachedGlyphBBox(f, ids[i], &bb) == 0) getGlyphBounds((XeTeXLayoutEngine) font_layout_engine[f], ids[i], &bb);  /* never fill the cache: it outlives the run */
                    float y = Fix2D(-loc[i].y);
                    if (BOX_height(q) == H && D2Fix(y + bb.yMax) == H && nh < 16) { hs[nh][0] = f; hs[nh][1] = ids[i]; hs[nh][2] = loc[i].y; nh++; }
                    if (BOX_depth(q) == D && -D2Fix(y + bb.yMin) == D && nd < 16) { ds[nd][0] = f; ds[nd][1] = ids[i]; ds[nd][2] = loc[i].y; nd++; }
                }
            } else if (NODE_subtype(q) == GLYPH_NODE || NODE_subtype(q) == PIC_NODE || NODE_subtype(q) == PDF_NODE) {
                if (BOX_height(q) == H) hb = true;
                if (BOX_depth(q) == D) db = true;
            }
            break;
        default: break;
        }
    }
    if (nh) { jkey("hx"); jopen('['); for (int i = 0; i < nh; i++) { jopen('['); jnum(hs[i][0]); jnum(hs[i][1]); jnum(hs[i][2]); jclose(']'); } jclose(']'); }
    if (nd) { jkey("dx"); jopen('['); for (int i = 0; i < nd; i++) { jopen('['); jnum(ds[i][0]); jnum(ds[i][1]); jnum(ds[i][2]); jclose(']'); } jclose(']'); }
    if (hb) jint("hb", 1);
    if (db) jint("db", 1);
}

static void ser_node(int32_t p, int mode, hyctx *hy) {
    int64_t v;
    jopen('{');
    if (is_char_node(p)) {
        jcstr("t", "c");
        jint("f", CHAR_NODE_font(p));
        jint("c", CHAR_NODE_character(p));
        note_glyph(CHAR_NODE_font(p), CHAR_NODE_character(p));
        if (rtx_pb) jchar_src(p);
        if (rtx_ab) jattrs(p);
        if (hy && hy->on) jtfm_points(p);
        jclose('}');
        return;
    }
    switch (NODE_type(p)) {
    case HLIST_NODE:
    case VLIST_NODE: {
        bool line = rtx_nb[p] & NB_LINE && rmap_get(&line_map, p, &v);
        jcstr("t", NODE_type(p) == HLIST_NODE ? "h" : "v");
        ser_box_fields(p);
        if (line) { jkey("line"); jopen('['); jnum(v >> 20); jnum(v & 0xFFFFF); jclose(']'); }
        if (rtx_nb[p] & NB_DISP && rmap_get(&disp_map, p, &v)) jint("disp", v);
        if (line && (mode & M_FLOW)) { line_extents(p); break; }
        jkey("c");
        ser_list(BOX_list_ptr(p), NODE_type(p) == VLIST_NODE ? (mode & M_FLOW) : 0, NULL);
        break;
    }
    case UNSET_NODE:
        jcstr("t", "u");
        jint("w", BOX_width(p)); jint("h", BOX_height(p)); jint("d", BOX_depth(p));
        jkey("c"); ser_list(BOX_list_ptr(p), 0, NULL);
        break;
    case RULE_NODE:
        jcstr("t", "r");
        jint("w", BOX_width(p)); jint("h", BOX_height(p)); jint("d", BOX_depth(p));
        break;
    case INS_NODE:
        jcstr("t", "i");
        jint("n", NODE_subtype(p));
        jint("h", BOX_height(p)); jint("d", BOX_depth(p));
        jint("fc", INSERTION_NODE_float_cost(p));
        jspec("stp", INSERTION_NODE_split_top_ptr(p));
        if (rtx_nb[p] & NB_INS && rmap_get(&ins_map, p, &v)) jint("iid", v);
        jkey("c"); ser_list(INSERTION_NODE_ins_ptr(p), M_FLOW, NULL);
        break;
    case MARK_NODE:
        jcstr("t", "M");
        if (MARK_NODE_class(p)) jint("cl", MARK_NODE_class(p));
        jkey("s"); jtokens(MARK_NODE_ptr(p));
        break;
    case ADJUST_NODE:
        jcstr("t", "a");
        if (NODE_subtype(p)) jint("pre", 1);
        jkey("c"); ser_list(mem[p + 1].b32.s1, 0, NULL);
        break;
    case LIGATURE_NODE: {
        jcstr("t", "L");
        jint("f", LIGATURE_NODE_lig_font(p));
        jint("c", LIGATURE_NODE_lig_char(p));
        if (NODE_subtype(p)) jint("sub", NODE_subtype(p));
        note_glyph(LIGATURE_NODE_lig_font(p), LIGATURE_NODE_lig_char(p));
        jkey("o"); jopen('[');
        for (int32_t q = LIGATURE_NODE_lig_ptr(p); q != TEX_NULL; q = LLIST_link(q)) jnum(CHAR_NODE_character(q));
        jclose(']');
        if (rtx_pb) jlig_src(p);
        if (hy && hy->on) jtfm_points(p);
        break;
    }
    case DISC_NODE:
        jcstr("t", "d");
        jkey("pre"); ser_list(DISCRETIONARY_NODE_pre_break(p), 0, NULL);
        jkey("post"); ser_list(DISCRETIONARY_NODE_post_break(p), 0, NULL);
        if (DISCRETIONARY_NODE_replace_count(p)) jint("rc", DISCRETIONARY_NODE_replace_count(p));
        break;
    case WHATSIT_NODE:
        switch (NODE_subtype(p)) {
        case OPEN_NODE: case WRITE_NODE: case CLOSE_NODE: case PDF_SAVE_POS_NODE:
            jcstr("t", "W"); jint("s", NODE_subtype(p));
            break;
        case SPECIAL_NODE:
            jcstr("t", "x"); jkey("s"); jtokens(WRITE_NODE_tokens(p));
            break;
        case LANGUAGE_NODE:
            jcstr("t", "lang");
            jint("l", LANGUAGE_NODE_what_lang(p));
            jint("lhm", LANGUAGE_NODE_what_lhm(p)); jint("rhm", LANGUAGE_NODE_what_rhm(p));
            if (hy) { hy->lang = LANGUAGE_NODE_what_lang(p); hy->lhm = LANGUAGE_NODE_what_lhm(p); hy->rhm = LANGUAGE_NODE_what_rhm(p); }
            break;
        case NATIVE_WORD_NODE: case NATIVE_WORD_NODE_AT:
            ser_native_word(p, hy);
            break;
        case GLYPH_NODE:
            jcstr("t", "G");
            jint("f", NATIVE_NODE_font(p)); jint("g", NATIVE_NODE_glyph(p));
            if (rtx_pb) jchar_src(p);
            note_glyph(NATIVE_NODE_font(p), NATIVE_NODE_glyph(p));
            jint("w", BOX_width(p)); jint("h", BOX_height(p)); jint("d", BOX_depth(p));
            break;
        case PIC_NODE: case PDF_NODE:
            jcstr("t", "pic");
            jint("k", NODE_subtype(p));
            jint("w", BOX_width(p)); jint("h", BOX_height(p)); jint("d", BOX_depth(p));
            jint("page", mem[p + 4].b16.s0);
            jint("box", mem[p + 8].b16.s1);
            jkey("m"); jopen('[');
            jnum(mem[p + 5].b32.s0); jnum(mem[p + 5].b32.s1); jnum(mem[p + 6].b32.s0);
            jnum(mem[p + 6].b32.s1); jnum(mem[p + 7].b32.s0); jnum(mem[p + 7].b32.s1);
            jclose(']');
            jkey("path"); jbytes(PIC_NODE_path(p), PIC_NODE_path_len(p));
            break;
        default:
            jcstr("t", "W"); jint("s", NODE_subtype(p));
        }
        break;
    case MATH_NODE:
        jcstr("t", "m");
        jint("sub", NODE_subtype(p));
        if (BOX_width(p)) jint("w", BOX_width(p));
        if (rtx_nb[p] & NB_MATH && rmap_get(&math_map, p, &v)) jint("ml", v);
        break;
    case GLUE_NODE: {
        int32_t g = GLUE_NODE_glue_ptr(p);
        jcstr("t", "g");
        if (NODE_subtype(p)) jint("sub", NODE_subtype(p));
        jint("w", BOX_width(g));
        if (GLUE_SPEC_stretch(g)) { jint("st", GLUE_SPEC_stretch(g)); if (GLUE_SPEC_stretch_order(g)) jint("sto", GLUE_SPEC_stretch_order(g)); }
        if (GLUE_SPEC_shrink(g)) { jint("sh", GLUE_SPEC_shrink(g)); if (GLUE_SPEC_shrink_order(g)) jint("sho", GLUE_SPEC_shrink_order(g)); }
        if (rtx_nb[p] & RTX_NB_SPACE) jint("sp", 1);
        jint("sl", SYNCTEX_line(p, MEDIUM_NODE_SIZE));
        if (GLUE_NODE_leader_ptr(p) != TEX_NULL) { jkey("l"); ser_node(GLUE_NODE_leader_ptr(p), 0, NULL); }
        break;
    }
    case KERN_NODE:
        jcstr("t", "k");
        if (NODE_subtype(p)) jint("sub", NODE_subtype(p));
        jint("w", BOX_width(p));
        if (rtx_nb[p] & RTX_NB_ITALIC) jint("it", 1);
        break;
    case MARGIN_KERN_NODE:
        jcstr("t", "mk"); jint("sub", NODE_subtype(p)); jint("w", BOX_width(p));
        break;
    case PENALTY_NODE:
        jcstr("t", "p"); jint("v", PENALTY_NODE_penalty(p));
        break;
    default:
        jcstr("t", "?"); jint("type", NODE_type(p));
    }
    if (rtx_ab) jattrs(p);
    jclose('}');
}

static void ser_list(int32_t p, int mode, hyctx *hy) {
    jopen('[');
    if (hy && hy->on) tfm_run_end = TEX_NULL;
    for (; p != TEX_NULL; p = LLIST_link(p)) { if (hy && hy->on) tfm_word_points(p, hy); ser_node(p, mode, hy); }
    jclose(']');
}

/* ── topology fingerprint (display samples) ───────────────────────────── */

static uint64_t fp_mix(uint64_t h, uint64_t x) { h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h; }
static uint64_t fp_list(int32_t p);
static uint64_t fp_node(int32_t p) {
    uint64_t h = 1469598103934665603ull;
    if (is_char_node(p)) return fp_mix(fp_mix(h, CHAR_NODE_font(p)), 0x10000 + CHAR_NODE_character(p));
    h = fp_mix(h, NODE_type(p));
    switch (NODE_type(p)) {
    case HLIST_NODE: case VLIST_NODE: h = fp_mix(h, fp_list(BOX_list_ptr(p))); break;
    case GLUE_NODE:
        h = fp_mix(h, NODE_subtype(p));
        h = fp_mix(h, GLUE_SPEC_stretch_order(GLUE_NODE_glue_ptr(p)) * 8 + GLUE_SPEC_shrink_order(GLUE_NODE_glue_ptr(p)));
        if (GLUE_NODE_leader_ptr(p) != TEX_NULL) h = fp_mix(h, fp_node(GLUE_NODE_leader_ptr(p)));
        break;
    case KERN_NODE: case MATH_NODE: h = fp_mix(h, NODE_subtype(p)); break;
    case PENALTY_NODE: h = fp_mix(h, PENALTY_NODE_penalty(p)); break;
    case DISC_NODE:
        h = fp_mix(h, fp_list(DISCRETIONARY_NODE_pre_break(p)));
        h = fp_mix(h, fp_list(DISCRETIONARY_NODE_post_break(p)));
        h = fp_mix(h, DISCRETIONARY_NODE_replace_count(p));
        break;
    case LIGATURE_NODE: h = fp_mix(h, LIGATURE_NODE_lig_char(p)); break;
    case WHATSIT_NODE:
        h = fp_mix(h, NODE_subtype(p));
        if (NODE_subtype(p) == NATIVE_WORD_NODE || NODE_subtype(p) == NATIVE_WORD_NODE_AT) {
            uint16_t *ids = glyph_ids(p);
            for (int i = 0; i < NATIVE_NODE_glyph_count(p); i++) h = fp_mix(h, ids[i]);
        } else if (NODE_subtype(p) == GLYPH_NODE) h = fp_mix(h, NATIVE_NODE_glyph(p));
        break;
    default: break;
    }
    return h;
}
static uint64_t fp_list(int32_t p) {
    uint64_t h = 7;
    int n = 0;
    for (; p != TEX_NULL; p = LLIST_link(p), n++) h = fp_mix(h, fp_node(p));
    return fp_mix(h, n);
}

/* ── paragraphs ───────────────────────────────────────────────────────── */

static rmap t82_set;

/* TeX §894–§899 / §930, §926: the native word TeX82's second pass would try
 * after the glue at g, or TEX_NULL. */
static int32_t t82_word_after(int32_t g, int lang) {
    int32_t s = LLIST_link(g), ha = TEX_NULL;
    int32_t hidx = hyph_index_for(lang);
    while (s != TEX_NULL) {
        if (is_char_node(s)) return TEX_NULL;           /* a TFM word: not ours */
        switch (NODE_type(s)) {
        case LIGATURE_NODE:
            if (LIGATURE_NODE_lig_ptr(s) == TEX_NULL) break;
            return TEX_NULL;
        case KERN_NODE:
            if (NODE_subtype(s) == NORMAL) break;
            return TEX_NULL;
        case MATH_NODE:
            if (NODE_subtype(s) >= L_CODE) break;
            return TEX_NULL;
        case WHATSIT_NODE:
            if (NODE_subtype(s) == NATIVE_WORD_NODE || NODE_subtype(s) == NATIVE_WORD_NODE_AT) {
                for (int l = 0; l < NATIVE_NODE_length(s); l++) {
                    int32_t c = NATIVE_NODE_text(s)[l];
                    if (c >= 0xD800 && c < 0xDC00) { c = 0x10000 + (c - 0xD800) * 0x400 + NATIVE_NODE_text(s)[l + 1] - 0xDC00; l++; }
                    if (hyph_code(c, hidx) != 0 || LC_CODE(c) != 0) {
                        if (LC_CODE(c) == c || INTPAR(uc_hyph) > 0) { ha = s; goto found; }
                        return TEX_NULL;
                    }
                }
            } else if (NODE_subtype(s) == LANGUAGE_NODE) {
                lang = LANGUAGE_NODE_what_lang(s);
                hidx = hyph_index_for(lang);
            }
            break;
        default:
            return TEX_NULL;
        }
        s = LLIST_link(s);
    }
    return TEX_NULL;
found:
    if (hyphen_char[NATIVE_NODE_font(ha)] < 0 || hyphen_char[NATIVE_NODE_font(ha)] > BIGGEST_CHAR) return TEX_NULL;
    for (s = LLIST_link(ha); s != TEX_NULL; s = LLIST_link(s)) {
        if (is_char_node(s)) continue;
        switch (NODE_type(s)) {
        case LIGATURE_NODE: continue;
        case KERN_NODE: if (NODE_subtype(s) != NORMAL) return ha; continue;
        case WHATSIT_NODE: case GLUE_NODE: case PENALTY_NODE: case INS_NODE: case ADJUST_NODE: case MARK_NODE:
            return ha;
        default: return TEX_NULL;
        }
    }
    return TEX_NULL;
}

void rtx_par_begin(bool display, int lang, int lhm, int rhm) {
    if (child) return;
    start();
    int32_t head = LLIST_link(TEMP_HEAD);
    cur_par = ++par_count;
    par_saved_tail = cur_list.tail;
    if (!par_map.cap) rmap_rehash(&par_map);
    rmap_clear(&par_map);
    int idx = 0;
    for (int32_t p = head; p != TEX_NULL; p = LLIST_link(p), idx++) {
        if (is_char_node(p)) continue;
        rtx_nb[p] |= NB_PAR;
        rmap_put(&par_map, p, idx);
        if (NODE_type(p) == INS_NODE) {
            rtx_nb[p] |= NB_INS;
            rmap_put(&ins_map, p, ++ins_count);
        }
    }
    /* which words TeX82 would try */
    if (!t82_set.cap) rmap_rehash(&t82_set);
    rmap_clear(&t82_set);
    {
        int l = lang;
        for (int32_t p = head; p != TEX_NULL; p = LLIST_link(p)) {
            if (is_char_node(p)) continue;
            if (NODE_type(p) == WHATSIT_NODE && NODE_subtype(p) == LANGUAGE_NODE) l = LANGUAGE_NODE_what_lang(p);
            if (NODE_type(p) == GLUE_NODE) {
                int32_t w = t82_word_after(p, l);
                if (w != TEX_NULL) rmap_put(&t82_set, w, 1);
            }
        }
    }
    rec_begin("par");
    jint("id", cur_par);
    if (display) jbool("display", true);
    jint("nest", nest_ptr);
    jint("mode", cur_list.mode);
    jint("prevgraf", cur_list.prev_graf);
    jint("hsize", DIMENPAR(hsize));
    if (LOCAL(par_shape) != TEX_NULL) {
        int32_t ps = LOCAL(par_shape);
        int n = LLIST_info(ps);
        jkey("parshape"); jopen('[');
        for (int k = 1; k <= n; k++) { jnum(mem[ps + 2 * k - 1].b32.s1); jnum(mem[ps + 2 * k].b32.s1); }
        jclose(']');
    }
    if (DIMENPAR(hang_indent)) { jint("hangindent", DIMENPAR(hang_indent)); jint("hangafter", INTPAR(hang_after)); }
    jspec("leftskip", GLUEPAR(left_skip));
    jspec("rightskip", GLUEPAR(right_skip));
    jspec("parfillskip", GLUEPAR(par_fill_skip));
    jint("parindent", DIMENPAR(par_indent));
    jspec("baselineskip", GLUEPAR(baseline_skip));
    jspec("lineskip", GLUEPAR(line_skip));
    jint("lineskiplimit", DIMENPAR(line_skip_limit));
    jint("pretolerance", INTPAR(pretolerance));
    jint("tolerance", INTPAR(tolerance));
    jint("emergencystretch", DIMENPAR(emergency_stretch));
    jint("looseness", INTPAR(looseness));
    jint("linepenalty", INTPAR(line_penalty));
    jint("hyphenpenalty", INTPAR(hyphen_penalty));
    jint("exhyphenpenalty", INTPAR(ex_hyphen_penalty));
    jint("adjdemerits", INTPAR(adj_demerits));
    jint("doublehyphendemerits", INTPAR(double_hyphen_demerits));
    jint("finalhyphendemerits", INTPAR(final_hyphen_demerits));
    jint("clubpenalty", INTPAR(club_penalty));
    jint("widowpenalty", INTPAR(widow_penalty));
    jint("brokenpenalty", INTPAR(broken_penalty));
    jint("interlinepenalty", INTPAR(inter_line_penalty));
    jint("lastlinefit", INTPAR(last_line_fit));
    jint("protrude", INTPAR(xetex_protrude_chars));
    jint("lang", lang); jint("lhm", lhm); jint("rhm", rhm);
    jint("uchyph", INTPAR(uc_hyph));
    jint("font", eqtb[CUR_FONT_LOC].b32.s1);
    jint("tag", cur_input.synctex_tag);
    jint("line", abs(pack_begin_line));
    jint("endline", line);
    hyctx hy = { true, lang, lhm, rhm, &t82_set };
    jkey("n"); ser_list(head, 0, &hy);
    rec_end();
}

/* TeX's decisions: where each line ends. A break is named by the index of
 * the node in the captured list, or – at a node TeX made while hyphenating
 * – by the index of the last captured node before it and the characters
 * that follow it up to the break. */
void rtx_par_breaks(int32_t best_bet, int32_t best_line, int32_t demerits, int32_t looseness,
                    bool second_pass, bool final_pass) {
    if (child) return;
    int nb = 0, cap = 64;
    int32_t *br = xmalloc(sizeof(int32_t) * cap);
    for (int32_t q = ACTIVE_NODE_break_node(best_bet); q != TEX_NULL; q = PASSIVE_NODE_prev_break(q)) {
        if (nb == cap) { cap *= 2; br = xrealloc(br, sizeof(int32_t) * cap); }
        br[nb++] = PASSIVE_NODE_cur_break(q);
    }
    rmap pos = { 0 };
    int last = -1, chars = 0;
    for (int32_t p = LLIST_link(TEMP_HEAD); p != TEX_NULL; p = LLIST_link(p)) {
        int64_t v;
        if (!is_char_node(p) && (rtx_nb[p] & NB_PAR) && rmap_get(&par_map, p, &v)) {
            last = (int) v; chars = 0;
            rmap_put(&pos, p, ((int64_t) last << 21) | 0x1FFFFF);
            if (NODE_type(p) == WHATSIT_NODE && (NODE_subtype(p) == NATIVE_WORD_NODE || NODE_subtype(p) == NATIVE_WORD_NODE_AT))
                chars = NATIVE_NODE_length(p);
            continue;
        }
        rmap_put(&pos, p, ((int64_t) last << 21) | chars);
        if (is_char_node(p)) chars++;
        else if (NODE_type(p) == LIGATURE_NODE) {
            for (int32_t q = LIGATURE_NODE_lig_ptr(p); q != TEX_NULL; q = LLIST_link(q)) chars++;
        } else if (NODE_type(p) == WHATSIT_NODE && (NODE_subtype(p) == NATIVE_WORD_NODE || NODE_subtype(p) == NATIVE_WORD_NODE_AT))
            chars += NATIVE_NODE_length(p);
    }
    rec_begin("brk");
    jint("id", cur_par);
    jint("lines", best_line - 1 - cur_list.prev_graf);
    jint("demerits", demerits);
    if (looseness) jint("looseness", looseness);
    jint("pass", final_pass ? 3 : second_pass ? 2 : 1);
    jkey("b"); jopen('[');
    for (int i = nb - 1; i >= 0; i--) {
        int32_t p = br[i];
        int64_t v;
        jopen('[');
        if (p == TEX_NULL) { jnum(-1); }
        else if (rmap_get(&pos, p, &v)) {
            int a = (int) (v >> 21), c = (int) (v & 0x1FFFFF);
            jnum(a);
            if (c != 0x1FFFFF) jnum(c);
        } else jnum(-2);
        jclose(']');
    }
    jclose(']');
    rec_end();
    free(br);
    free(pos.k); free(pos.v);
}

void rtx_line(int32_t box, int32_t line_no) {
    if (child) return;
    rtx_nb[box] |= NB_LINE;
    rmap_put(&line_map, box, ((int64_t) cur_par << 20) | (line_no & 0xFFFFF));
}

void rtx_par_end(void) { }

/* ── the main vertical list (build_page) ──────────────────────────────── */

void rtx_build_page(void) {
    if (child) return;
    start();
    int32_t head = LLIST_link(CONTRIB_HEAD);
    if (head == TEX_NULL) return;
    /* the trailing run of glue/kern/penalty (\lastskip can read it back) */
    int32_t tail = TEX_NULL, prev = CONTRIB_HEAD, tail_prev = TEX_NULL;
    for (int32_t p = head; p != TEX_NULL; prev = p, p = LLIST_link(p)) {
        int t = NODE_type(p);
        if (t == GLUE_NODE || t == KERN_NODE || t == PENALTY_NODE) {
            if (tail == TEX_NULL) { tail = p; tail_prev = prev; }
        } else tail = TEX_NULL;
    }
    bool pageless = rtx_capture_flags & RTX_PAGELESS;
    bool in_tail = false;
    scaled_t trailing = 0;
    for (int32_t p = head; p != TEX_NULL; p = LLIST_link(p)) {
        if (!(rtx_nb[p] & NB_CONTRIB)) {
            rec_begin("flow");
            jkey("n"); ser_node(p, M_FLOW, NULL);
            rec_end();
            rtx_nb[p] |= NB_CONTRIB;
        }
        if (!pageless) continue;
        if (p == tail) in_tail = true;
        int t = NODE_type(p);
        if (in_tail) {
            if (t == GLUE_NODE) trailing += BOX_width(GLUE_NODE_glue_ptr(p));
            else if (t == KERN_NODE) trailing += BOX_width(p);
        } else if ((t == HLIST_NODE || t == VLIST_NODE || t == RULE_NODE
                    || (t == WHATSIT_NODE && (NODE_subtype(p) == PIC_NODE || NODE_subtype(p) == PDF_NODE)))
                   && BOX_height(p) + BOX_depth(p) >= 65536) {
            BOX_height(p) = 0;
            BOX_depth(p) = 0;
        } else if (t == GLUE_NODE) {
            delete_glue_ref(GLUE_NODE_glue_ptr(p));
            GLUE_NODE_glue_ptr(p) = 0;           /* zero_glue */
            GLUE_SPEC_ref_count(0)++;
        } else if (t == KERN_NODE) {
            BOX_width(p) = 0;
        } else if (t == INS_NODE) {
            BOX_height(p) = 0;
        }
    }
    if (pageless && trailing != 0) {
        int32_t k = new_kern(-trailing);
        rtx_nb[k] |= NB_CONTRIB;
        LLIST_link(k) = tail;
        LLIST_link(tail_prev) = k;
        if (tail_prev == CONTRIB_HEAD) {}  /* head replaced through CONTRIB_HEAD's link */
    }
}

/* ── displays ─────────────────────────────────────────────────────────── */

static int32_t disp_tail = TEX_NULL;
static int disp_cur = 0;
static int32_t disp_quad = 0;
static int disp_mlist = 0;
static int disp_mlist_reset(void) { disp_tail = TEX_NULL; disp_cur = 0; disp_mlist = 0; return 0; }

void rtx_display_begin(void) {
    disp_cur = ++disp_count;
    if (child) return;
    start();
    disp_quad = font_info[QUAD_CODE + param_base[eqtb[CUR_FONT_LOC].b32.s1]].b32.s1;
    if ((rtx_capture_flags & RTX_SAMPLES) && nsamples > 0 && nest_ptr == 1)
        rtx_fork_pending = disp_cur;
}

void rtx_display_append_begin(void) {
    disp_tail = cur_list.tail;
}

static void display_note(bool align, int32_t e, int32_t q, int32_t d, bool l, int g1, int g2, bool danger, int32_t mq) {
    jint("id", disp_cur);
    if (align) jbool("align", true);
    jint("pre_size", DIMENPAR(pre_display_size));
    jint("width", DIMENPAR(display_width));
    jint("indent", DIMENPAR(display_indent));
    jspec("above", GLUEPAR(above_display_skip));
    jspec("below", GLUEPAR(below_display_skip));
    jspec("above_short", GLUEPAR(above_display_short_skip));
    jspec("below_short", GLUEPAR(below_display_short_skip));
    jint("g1", g1); jint("g2", g2);
    jint("quad", disp_quad);
    jspec("baselineskip", GLUEPAR(baseline_skip));
    jspec("lineskip", GLUEPAR(line_skip));
    jint("lineskiplimit", DIMENPAR(line_skip_limit));
    jint("predisplaydirection", INTPAR(pre_display_direction));
    if (disp_mlist) jint("ml", disp_mlist);
    if (!align) {
        jkey("place"); jopen('{');
        jint("e", e); jint("q", q); jint("d", d); jbool("l", l);
        jint("mathquad", mq);
        jint("hfuzz", DIMENPAR(hfuzz));
        jint("overfullrule", DIMENPAR(overfull_rule));
        if (danger) jbool("danger", true);
        jclose('}');
    }
}

static void emit_sample(bool align, int32_t e, int32_t q, int32_t d, bool l, int g1, int g2, bool danger, int32_t mq);

static void display_end(bool align, int32_t e, int32_t q, int32_t d, bool l, int g1, int g2, bool danger, int32_t mq) {
    if (child) {
        if (disp_cur == child_disp) emit_sample(align, e, q, d, l, g1, g2, danger, mq);
        return;
    }
    int32_t first = disp_tail == TEX_NULL ? TEX_NULL : LLIST_link(disp_tail);
    int role = 0;
    for (int32_t p = first; p != TEX_NULL; p = LLIST_link(p)) {
        if (NODE_type(p) == HLIST_NODE || NODE_type(p) == VLIST_NODE) {
            rtx_nb[p] |= NB_DISP;
            rmap_put(&disp_map, p, disp_cur);
            role++;
        }
        if (p == cur_list.tail) break;
    }
    rec_begin("disp");
    display_note(align, e, q, d, l, g1, g2, danger, mq);
    jint("boxes", role);
    jkey("fp"); { char t[24]; snprintf(t, sizeof t, "%016llx", (unsigned long long) fp_list(first)); jbytes((const unsigned char *) t, strlen(t)); }
    rec_end();
    disp_mlist = 0;
}

void rtx_display_end(int32_t e, int32_t q, int32_t d, bool l, int g1, int g2, bool danger, int32_t mq) {
    display_end(false, e, q, d, l, g1, g2, danger, mq);
}

void rtx_display_align_end(void) {
    display_end(true, 0, 0, 0, false, GLUE_PAR__above_display_skip, GLUE_PAR__below_display_skip, false, 0);
}

/* ── display samples: re-typeset the display at other widths ──────────── */

/* A \dimen register by name (\linewidth, …), or NULL. */
static int32_t *dimen_register(const char *name) {
    int n = (int) strlen(name);
    int j = (last > first ? last : first) + 1;
    if (j + n >= buf_size) return NULL;
    for (int i = 0; i < n; i++) buffer[j + i] = (unsigned char) name[i];
    bool save = no_new_control_sequence;
    no_new_control_sequence = true;
    int32_t cs = id_lookup(j, n);
    no_new_control_sequence = save;
    if (cs == UNDEFINED_CONTROL_SEQUENCE) return NULL;
    if (eqtb[cs].b16.s1 != ASSIGN_DIMEN) return NULL;
    return &eqtb[eqtb[cs].b32.s1].b32.s1;
}

#ifdef __EMSCRIPTEN__
/* wasm has no fork: the host (web/library.js) snapshots linear memory at the
 * fork point and restores it after each sample. All engine state lives in
 * linear memory, and main_control's loop head, where both happen, has no
 * live C locals. */
extern void rtx_js_snapshot(void);
extern void rtx_js_restore(void);
extern int rtx_js_next(void);
extern void rtx_js_stash(const char *p, size_t n);
extern int rtx_js_stash_count(void);
extern size_t rtx_js_stash_size(int i);
extern void rtx_js_stash_copy(int i, char *p);
extern void rtx_js_drop(void);
#endif

/* The sample child's widths: \displaywidth and the registers a display's
 * macros may measure with. */
static void apply_widths(int32_t delta) {
    DIMENPAR(display_width) += delta;
    DIMENPAR(hsize) += delta;
    static const char *regs[] = { "linewidth", "columnwidth", "textwidth", NULL };
    for (int i = 0; regs[i]; i++) { int32_t *r = dimen_register(regs[i]); if (r) *r += delta; }
}

static void emit_sample(bool align, int32_t e, int32_t q, int32_t d, bool l, int g1, int g2, bool danger, int32_t mq) {
    int32_t first = disp_tail == TEX_NULL ? TEX_NULL : LLIST_link(disp_tail);
    out.n = 0;
    rec_begin("dsample");
    jint("delta", child_delta);
    display_note(align, e, q, d, l, g1, g2, danger, mq);
    jkey("fp"); { char t[24]; snprintf(t, sizeof t, "%016llx", (unsigned long long) fp_list(first)); jbytes((const unsigned char *) t, strlen(t)); }
    jkey("items"); jopen('[');
    for (int32_t p = first; p != TEX_NULL; p = LLIST_link(p)) {
        ser_node(p, 0, NULL);
        if (p == cur_list.tail) break;
    }
    jclose(']');
    rec_end();
#ifdef RTX_CAN_FORK
    size_t off = 0;
    while (off < out.n) {
        ssize_t w = write(child_fd, out.b + off, out.n - off);
        if (w <= 0) break;
        off += w;
    }
    _exit(0);
#elif defined(__EMSCRIPTEN__)
    rtx_js_stash(out.b, out.n);   /* kept by the host across the restore */
    out.n = 0;
    child_done = true;
    rtx_fork_pending = 1;          /* back to rtx_big_switch at the next loop head */
#endif
}

#ifdef RTX_CAN_FORK
/* Fork once for width Δ; the child returns 1 and runs on at that width, the
 * parent gets the sample line (or NULL) and 0. */
static int fork_sample(int id, int32_t delta, char **line, uint64_t *fp) {
    int fds[2];
    *line = NULL;
    if (pipe(fds) != 0) return 0;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 0; }
    if (pid == 0) {
        close(fds[0]);
        child = true; child_fd = fds[1]; child_disp = id; child_delta = delta;
        alarm(60);
        out.n = 0;
        apply_widths(delta);
        return 1;
    }
    close(fds[1]);
    sbuf got = { 0 };
    char tmp[65536];
    ssize_t n;
    while ((n = read(fds[0], tmp, sizeof tmp)) > 0) sb_write(&got, tmp, n);
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (got.n && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        sb_putc(&got, 0);
        *line = got.b;
        char *f = strstr(got.b, "\"fp\":\"");
        *fp = f ? strtoull(f + 6, NULL, 16) : 0;
    } else free(got.b);
    return 0;
}
#endif

#define WIDE_RESOLUTION (2 * 65536)

#ifdef __EMSCRIPTEN__
/* The delta and fingerprint of stashed sample i. */
static void stash_info(int i, int32_t *delta, uint64_t *fp) {
    size_t n = rtx_js_stash_size(i);
    char *b = xmalloc(n + 1);
    rtx_js_stash_copy(i, b);
    b[n] = 0;
    char *d = strstr(b, "\"delta\":"), *f = strstr(b, "\"fp\":\"");
    *delta = d ? (int32_t) strtol(d + 8, NULL, 10) : 0;
    *fp = f ? strtoull(f + 6, NULL, 16) : 0;
    free(b);
}

/* At the fork point, with memory as it was there: start the next sample
 * child (Δ = 0, the requested widths, then bisection as native), or put the
 * samples into the stream and go on as the parent. Reads only globals and
 * the host's stash: it runs right after a restore. */
static __attribute__((noinline)) void wasm_next(int id) {
    int k = rtx_js_next();
    int nbase = 1 + nsamples;
    int32_t delta = 0;
    bool go = false;
    if (k < nbase) { delta = k == 0 ? 0 : samples[k - 1]; go = true; }
    else if (nsamples > 0 && k < 64) {
        int32_t dd, lo = 0, hi = samples[0];
        uint64_t flo, fhi, fm;
        stash_info(0, &dd, &flo);
        stash_info(1, &dd, &fhi);
        for (int j = nbase; j < k; j++) {
            stash_info(j, &dd, &fm);
            if (fm == flo) lo = dd; else { hi = dd; fhi = fm; }
        }
        if (flo != fhi && hi - lo > WIDE_RESOLUTION) { delta = lo + (hi - lo) / 2; go = true; }
    }
    if (go) {
        child = true; child_done = false; child_disp = id; child_delta = delta;
        rtx_fork_pending = 0;
        out.n = 0;
        apply_widths(delta);
        return;
    }
    int n = rtx_js_stash_count();
    for (int i = 0; i < n; i++) {
        size_t s = rtx_js_stash_size(i);
        sb_grow(&out, s);
        rtx_js_stash_copy(i, out.b + out.n);
        out.n += s;
    }
    rtx_js_drop();
    rtx_fork_pending = 0;
    flush_out();
}
#endif

void rtx_big_switch(void) {
#ifdef __EMSCRIPTEN__
    if (child) {
        if (!child_done) { rtx_fork_pending = 0; return; }
        rtx_js_restore();                 /* memory as at the fork point */
        wasm_next(rtx_fork_pending);
        return;
    }
    if (!rtx_fork_pending) return;
    flush_out();
    rtx_js_snapshot();
    wasm_next(rtx_fork_pending);
    return;
#endif
    int id = rtx_fork_pending;
    rtx_fork_pending = 0;
    if (child || !id) return;
#ifdef RTX_CAN_FORK
    flush_out();
    /* Δ = 0 first (the display as the parent will set it: its fingerprint),
     * then the requested widths; where the topology at W differs from the
     * first requested sample, bisect for the switch (display-model.ts's
     * wideVariants, to its 2pt resolution). */
    int32_t deltas[64];
    uint64_t fps[64];
    char *lines[64];
    int nd = 0;
    deltas[nd++] = 0;
    for (int i = 0; i < nsamples && nd < 48; i++) deltas[nd++] = samples[i];
    for (int i = 0; i < nd; i++) {
        fps[i] = 0;
        if (fork_sample(id, deltas[i], &lines[i], &fps[i])) return;   /* the child */
    }
    int32_t lo = 0, hi = nd > 1 ? deltas[1] : 0;
    uint64_t flo = fps[0], fhi = nd > 1 ? fps[1] : fps[0];
    while (nd > 1 && flo != fhi && hi - lo > WIDE_RESOLUTION && nd < 64) {
        int32_t mid = lo + (hi - lo) / 2;
        uint64_t fm = 0;
        char *ln = NULL;
        if (fork_sample(id, mid, &ln, &fm)) return;
        deltas[nd] = mid; fps[nd] = fm; lines[nd] = ln; nd++;
        if (fm == flo) lo = mid; else { hi = mid; fhi = fm; }
    }
    for (int i = 0; i < nd; i++) {
        if (!lines[i]) continue;
        sb_puts(&out, lines[i]);
        free(lines[i]);
    }
    flush_out();
#endif
}

int rtx_capture_is_child(void) { return child ? 1 : 0; }

/* ── math lists ───────────────────────────────────────────────────────── */

static void ser_mlist(int32_t p);

static void ser_field(const char *k, int32_t x) {
    int mt = mem[x].b32.s1;
    if (mt == EMPTY) return;
    jkey(k); jopen('{');
    switch (mt) {
    case MATH_CHAR: case MATH_TEXT_CHAR:
        jint("fam", mem[x].b16.s1 % 256);
        jint("c", mem[x].b16.s0 + (mem[x].b16.s1 / 256) * 65536);
        if (mt == MATH_TEXT_CHAR) jint("text", 1);
        if (rtx_pb) jnode_src(x);
        break;
    case SUB_BOX:
        jkey("box"); ser_node(mem[x].b32.s0, 0, NULL);
        break;
    case SUB_MLIST:
        jkey("ml"); ser_mlist(mem[x].b32.s0);
        break;
    default:
        jint("mt", mt);
    }
    jclose('}');
}

static void ser_delim(const char *k, int32_t x) {
    jkey(k); jopen('[');
    jnum(mem[x].b16.s3 % 256); jnum(mem[x].b16.s2 + (mem[x].b16.s3 / 256) * 65536);
    jnum(mem[x].b16.s1 % 256); jnum(mem[x].b16.s0 + (mem[x].b16.s1 / 256) * 65536);
    jclose(']');
}

static const char *noad_name(int t) {
    static const char *n[] = { "ord", "op", "bin", "rel", "open", "close", "punct", "inner",
                               "radical", "fraction", "under", "over", "accent", "vcenter", "left", "right" };
    return t >= ORD_NOAD && t <= RIGHT_NOAD ? n[t - ORD_NOAD] : "?";
}

static void ser_mlist(int32_t p) {
    jopen('[');
    for (; p != TEX_NULL; p = LLIST_link(p)) {
        if (is_char_node(p)) { ser_node(p, 0, NULL); continue; }
        int t = NODE_type(p);
        if (t < STYLE_NODE && t != UNSET_NODE) { ser_node(p, 0, NULL); continue; }
        jopen('{');
        if (t == STYLE_NODE) { jcstr("t", "style"); jint("s", NODE_subtype(p)); }
        else if (t == CHOICE_NODE) {
            jcstr("t", "choice");
            jkey("D"); ser_mlist(mem[p + 1].b32.s0);
            jkey("T"); ser_mlist(mem[p + 1].b32.s1);
            jkey("S"); ser_mlist(mem[p + 2].b32.s0);
            jkey("SS"); ser_mlist(mem[p + 2].b32.s1);
        } else if (t >= ORD_NOAD && t <= RIGHT_NOAD) {
            jcstr("t", noad_name(t));
            if (NODE_subtype(p)) jint("sub", NODE_subtype(p));
            if (t == FRACTION_NOAD) {
                jint("thickness", mem[p + 1].b32.s1);
                ser_field("num", p + 2);
                ser_field("den", p + 3);
                ser_delim("left", p + 4);
                ser_delim("right", p + 5);
            } else if (t == LEFT_NOAD || t == RIGHT_NOAD) {
                ser_delim("delim", p + 1);
            } else {
                ser_field("nuc", p + 1);
                ser_field("sup", p + 2);
                ser_field("sub_", p + 3);
                if (t == RADICAL_NOAD) ser_delim("delim", p + 4);
                if (t == ACCENT_NOAD) {
                    jint("afam", mem[p + 4].b16.s1 % 256);
                    jint("ac", mem[p + 4].b16.s0 + (mem[p + 4].b16.s1 / 256) * 65536);
                }
            }
        } else { jcstr("t", "?"); jint("type", t); }
        jclose('}');
    }
    jclose(']');
}

void rtx_mlist_enter(int32_t cur_mlist, int cur_style) {
    if (mlist_depth++ != 0 || child || !(rtx_capture_flags & RTX_MATHLISTS)) return;
    start();
    int id = ++mlist_count;
    int32_t t = cur_list.tail;
    bool inline_ = cur_style == TEXT_STYLE && t != TEX_NULL && !is_char_node(t)
        && NODE_type(t) == MATH_NODE && NODE_subtype(t) == BEFORE;
    if (inline_) { rtx_nb[t] |= NB_MATH; rmap_put(&math_map, t, id); }
    else if (cur_style == DISPLAY_STYLE) disp_mlist = id;
    /* the flush out of the partial record under construction: none, records
     * are written whole */
    rec_begin("mlist");
    jint("id", id);
    jint("style", cur_style);
    jkey("n"); ser_mlist(cur_mlist);
    rec_end();
}

void rtx_mlist_exit(void) { mlist_depth--; }

/* ── the end: fonts, glyphs ───────────────────────────────────────────── */

void rtx_finish(void) {
    if (child || !rtx_nb) return;
    for (int f = 0; f < fonts_cap; f++) {
        if (!fonts_used[f].bits || f > font_ptr) continue;
        rec_begin("font");
        jint("id", f);
        jkey("name"); jtexstr(font_name[f]);
        jint("size", font_size[f]);
        jint("dsize", font_dsize[f]);
        jint("hyphenchar", hyphen_char[f]);
        jkey("params"); jopen('[');
        for (int k = 1; k <= font_params[f] && k <= 7; k++) jnum(font_info[k + param_base[f]].b32.s1);
        jclose(']');
        XeTeXLayoutEngine engine = NULL;
        if (font_area[f] == OTGR_FONT_FLAG) {
            engine = (XeTeXLayoutEngine) font_layout_engine[f];
            uint32_t index = 0;
            const char *fn = getFontFilename(engine, &index);
            jcstr("file", fn);
            if (index) jint("index", index);
            freeFontFilename(fn);
            jint("upem", rtx_font_units_per_em(engine));
            jkey("extend"); jdbl(getExtendFactor(engine));
            jkey("slant"); jdbl(getSlantFactor(engine));
            jkey("embolden"); jdbl(getEmboldenFactor(engine));
            jkey("ptsize"); jdbl(getPointSize(engine));
            jbool("native", true);
        } else if (font_area[f] == AAT_FONT_FLAG) {
            jbool("native", true); jbool("aat", true);
        } else {
            jkey("area"); jtexstr(font_area[f]);
        }
        /* the font's hyphen character too, typeset or not: a discretionary's pre-break hyphen hangs into the
           margin by its \rpcode (microtype), and the reader's breaker needs that code for every break at one */
        if (hyphen_char[f] > 0 && hyphen_char[f] < 0x110000) {
            int32_t hg = engine ? map_char_to_glyph(f, hyphen_char[f]) : hyphen_char[f];
            if (hg > 0 && hg < (1 << 21) && (engine || (!is_native(f) && hg >= font_bc[f] && hg <= font_ec[f])))
                fonts_used[f].bits[hg >> 3] |= 1 << (hg & 7);
        }
        /* glyphs (native: ids) or characters (TFM) used */
        jkey("g"); jopen('[');
        for (uint32_t g = 0; g < (1u << 21); g++) {
            if (!(fonts_used[f].bits[g >> 3] & (1 << (g & 7)))) continue;
            jopen('[');
            jnum(g);
            if (engine) {
                GlyphBBox bb;
                if (getCachedGlyphBBox(f, g, &bb) == 0) getGlyphBounds(engine, g, &bb);  /* (see line_extents) */
                jnum(D2Fix(getGlyphWidthFromEngine(engine, g)));
                jnum(D2Fix(bb.yMax)); jnum(D2Fix(-bb.yMin));
                jnum(rtx_glyph_advance_units(engine, g));
                jnum(get_cp_code(f, g, LEFT_SIDE)); jnum(get_cp_code(f, g, RIGHT_SIDE));
                jnum(D2Fix(getGlyphItalCorr(engine, g)));
            } else if (!is_native(f) && g >= font_bc[f] && g <= font_ec[f]) {
                b16x4 ci = FONT_CHARACTER_INFO(f, g);
                jnum(FONT_CHARINFO_WIDTH(f, ci)); jnum(FONT_CHARINFO_HEIGHT(f, ci));
                jnum(FONT_CHARINFO_DEPTH(f, ci));
                jnum(FONT_CHARINFO_ITALCORR(f, ci));
                jnum(get_cp_code(f, g, LEFT_SIDE)); jnum(get_cp_code(f, g, RIGHT_SIDE));
            }
            jclose(']');
        }
        jclose(']');
        rec_end();
    }
    rec_begin("end");
    jint("paragraphs", par_count);
    jint("displays", disp_count);
    { int32_t *tw = dimen_register("textwidth"); if (tw) jint("textwidth", *tw); }
    jint("hsize", DIMENPAR(hsize));
    if (rtx_ab) { jint("asets", nasets - 1); if (aoverflow) jint("aoverflow", aoverflow); }
    rec_end();
    flush_out();
    if (out_h) ttstub_output_close(out_h);
    out_h = 0;
}
