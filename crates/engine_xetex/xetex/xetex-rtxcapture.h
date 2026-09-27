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

#define RTX_CAPTURE   1  /* paragraphs, flow, displays, fonts */
#define RTX_PAGELESS  2  /* zero-height originals for the page builder */
#define RTX_MATHLISTS 4  /* noads before mlist_to_hlist */
#define RTX_SAMPLES   8  /* displays re-typeset at other widths (fork) */

extern int rtx_capture_flags;
extern unsigned char *rtx_nb;     /* one byte per mem word; NULL when off */
extern int rtx_fork_pending;      /* a display asks for width samples */

#define RTX_NB_SPACE  0x02
#define RTX_NB_ITALIC 0x04

#define RTX_ON (rtx_capture_flags != 0)
#define RTX_FREE(p) do { if (rtx_nb) rtx_nb[p] = 0; } while (0)
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
