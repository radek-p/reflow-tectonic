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
 * Copyright 2026 Radosław Piórkowski. MIT licence, like Tectonic.
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
}

int rtx_set_variable(const char *name, int value) {
    if (!strcmp(name, "rtx_capture")) { rtx_capture_flags = value; nsamples = 0; reset(); return 0; }
    if (!strcmp(name, "rtx_sample")) { if (nsamples < MAX_SAMPLES) samples[nsamples++] = value; return 0; }
    return 1;
}

static void start(void) {
    if (started) return;
    started = true;
    rec_begin("hdr");
    jint("v", 1);
    jint("flags", rtx_capture_flags);
    jint("hsize", DIMENPAR(hsize));
    jkey("samples"); jopen('[');
    for (int i = 0; i < nsamples; i++) jnum(samples[i]);
    jclose(']');
    rec_end();
}

void rtx_input_opened(void) {
    if (!RTX_ON || child) return;
    start();
    rec_begin("in");
    jint("tag", cur_input.synctex_tag);
    jkey("name");
    if (in_open > 0 && full_source_filename_stack[in_open]) jtexstr(full_source_filename_stack[in_open]);
    else jtexstr(cur_name);
    rec_end();
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
    jclose('}');
}

static void ser_list(int32_t p, int mode, hyctx *hy) {
    jopen('[');
    for (; p != TEX_NULL; p = LLIST_link(p)) ser_node(p, mode, hy);
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
    rec_end();
    flush_out();
    if (out_h) ttstub_output_close(out_h);
    out_h = 0;
}
