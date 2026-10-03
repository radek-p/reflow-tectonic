/* xetex-rtxcapture.h: the reflowtex capture API.
 *
 * Off unless the driver sets rtx_capture_flags (tt_xetex_set_int_variable
 * "rtx_capture"). Every call site in the engine is guarded by the flag, so a
 * run without it executes exactly the unpatched code.
 *
 */
#ifndef XETEX_RTXCAPTURE_H
#define XETEX_RTXCAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define RTX_CAPTURE   1  /* paragraphs, flow, displays, fonts */
#define RTX_PAGELESS  2  /* zero-height originals for the page builder */
#define RTX_MATHLISTS 4  /* noads before mlist_to_hlist */
#define RTX_SAMPLES   8  /* displays re-typeset at other widths (fork) */
#define RTX_SRCPOS   16  /* source positions of characters (file, line, column) */
#define RTX_ATTRS    32  /* nodes stamped with \attribute909-917 (\count32000-32008) */

/* Attributes (RTX_ATTRS). LuaTeX's \attributeN is a count register whose
 * value every new node keeps; rtx-xetex.sty makes \attributeN \count(31091+N)
 * and each node gets the id of the tuple (\count32000..\count32008) in force when it
 * is allocated, in a side table (0: none set). Bit 15: a native word whose
 * characters came from runs with different tuples (a table per character). */
extern uint16_t *rtx_ab;         /* a tuple id per mem word; NULL when off */
extern bool rtx_attr_dirty;      /* a sparse count may have changed */
extern uint16_t rtx_attr_cur;
uint16_t rtx_attr_now(void);
void rtx_attr_copy(int32_t from, int32_t to);
void rtx_noad_attrs(int32_t q);
#define RTX_NOAD(q) do { if (rtx_ab) rtx_noad_attrs(q); } while (0)
#define RTX_STAMP(p) do { if (rtx_ab) rtx_ab[p] = rtx_attr_dirty ? rtx_attr_now() : rtx_attr_cur; } while (0)
/* every word of a node: a noad's fields keep the tuple they were made with */
#define RTX_STAMP_N(p, s) do { if (rtx_ab) { uint16_t a_ = rtx_attr_dirty ? rtx_attr_now() : rtx_attr_cur; for (int32_t i_ = 0; i_ < (s); i_++) rtx_ab[(p) + i_] = a_; } } while (0)
#define RTX_ACOPY(f, t) do { if (rtx_ab) rtx_attr_copy(f, t); } while (0)
#define RTX_ADIRTY() do { if (rtx_ab) rtx_attr_dirty = true; } while (0)

extern int rtx_capture_flags;
extern unsigned char *rtx_nb;     /* one byte per mem word; NULL when off */
extern int rtx_fork_pending;      /* a display asks for width samples */

#define RTX_NB_SPACE  0x02
#define RTX_NB_ITALIC 0x04

#define RTX_ON (rtx_capture_flags != 0)
#define RTX_FREE(p) do { if (rtx_nb) rtx_nb[p] = 0; if (rtx_pb) rtx_pb[p] = 0; } while (0)
/* free_node: every word of the node (a noad's fields carry positions) */
#define RTX_FREE_N(p, s) do { if (rtx_nb) rtx_nb[p] = 0; if (rtx_pb) memset(rtx_pb + (p), 0, (s)); } while (0)

/* Source positions (RTX_SRCPOS). get_next notes where the token it returns
 * came from: a column of the current file line, or a token-list node. */
extern unsigned char *rtx_pb;    /* a position byte per mem word; NULL when off */
extern int32_t rtx_tok_node, rtx_tok_col, rtx_tok_line, rtx_tok_tag;
void rtx_list_end(void);
/* the tokens read before it, newest last (a token backed up or stored can be one
 * read a little earlier: \futurelet's second, \expandafter's first) */
#define RTX_RING 8
typedef struct { int32_t node, col, line, tag; } rtx_tokrec;
extern rtx_tokrec rtx_ring[RTX_RING];
extern unsigned rtx_ring_i;
#define RTX_TOK_SAVE() (rtx_ring_i = (rtx_ring_i + 1) & (RTX_RING - 1), \
    rtx_ring[rtx_ring_i].node = rtx_tok_node, rtx_ring[rtx_ring_i].col = rtx_tok_col, \
    rtx_ring[rtx_ring_i].line = rtx_tok_line, rtx_ring[rtx_ring_i].tag = rtx_tok_tag)
#define RTX_TOK_FILE() do { if (rtx_pb) { RTX_TOK_SAVE(); rtx_tok_node = TEX_NULL; rtx_tok_col = cur_input.loc - cur_input.start; \
    rtx_tok_line = line; rtx_tok_tag = cur_input.synctex_tag; } } while (0)
#define RTX_TOK_NODE() do { if (rtx_pb) { RTX_TOK_SAVE(); rtx_tok_node = cur_input.loc; } } while (0)
/* this token node (a macro argument, a backed-up token) is the token just read */
#define RTX_TOKPOS(q) do { if (rtx_pb) rtx_tokpos(q); } while (0)
#define RTX_SRC(call) do { if (rtx_pb) call; } while (0)
/* a word taken from the free list inline (not through get_avail): its old position is not its own */
#define RTX_NOPOS(p) do { if (rtx_pb) rtx_pb[p] &= ~1; } while (0)
void rtx_tokpos(int32_t q);
void rtx_fieldpos(int32_t field);
void rtx_copypos(int32_t from, int32_t to);
void rtx_copytok(int32_t from, int32_t to);
void rtx_macro_enter(void);
void rtx_macro_push(void);
void rtx_list_push(void);
void rtx_back_begin(void);
void rtx_back_push(void);
void rtx_back_tok(int32_t q);
void rtx_char_num(void);
void rtx_partial_begin(int32_t s, int32_t r);
void rtx_partial_copy(int32_t q);
void rtx_native_char(void);
void rtx_native_collected(void);
void rtx_native_done(void);
void rtx_tfm_char(int32_t p);
void rtx_made_char(int32_t p);
void rtx_hu_from(int i, int32_t node);
void rtx_hu_hyph(int i);
void rtx_hu_char(int32_t p, int i);
void rtx_char_copy(int32_t from, int32_t to);
void rtx_merge_words(int32_t merged, int32_t chain);
void rtx_noad_done(int32_t q);
void rtx_lr_done(int32_t q);
void rtx_mark_from(int32_t field, int32_t list);
#define RTX_MARK(p, bit) do { if (rtx_nb) rtx_nb[p] |= (bit); } while (0)

int rtx_set_variable(const char *name, int value);
void rtx_input_opened(void);
void rtx_par_begin(bool display, int lang, int lhm, int rhm);
void rtx_par_breaks(int32_t best_bet, int32_t best_line, int32_t demerits, int32_t looseness,
                    bool second_pass, bool final_pass);
void rtx_line(int32_t box, int32_t line_no);
void rtx_par_end(void);
void rtx_build_page(void);
void rtx_display_begin(void);
void rtx_display_append_begin(void);
void rtx_display_end(int32_t e, int32_t q, int32_t d, bool l, int g1, int g2, bool danger, int32_t math_quad);
void rtx_display_align_end(void);
void rtx_big_switch(void);
void rtx_mlist_enter(int32_t mlist, int style);
void rtx_mlist_exit(void);
void rtx_finish(void);
int rtx_capture_is_child(void);

#endif
