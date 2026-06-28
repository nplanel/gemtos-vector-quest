/*
 * hud.c — HUD overlay: "VECTOR QUEST" title + round tally marks.
 *
 * Font: single-stroke vector, 5-column × 8-row unit grid (x:0-4, y:0-7),
 * 4px per unit → 16×28px per cell.  Retro-futurist style: geometric,
 * 45°/90° strokes only, chamfered corners on enclosed shapes.
 *
 * Layout (top 40 rows):
 *   Title  — rows TITLE_Y0 .. TITLE_Y0+CELL_H-1  (centred)
 *   Rule   — row  RULE_Y
 *   Tally  — rows TALLY_Y0 .. TALLY_Y1  (right-aligned, grows left)
 */

#include <stddef.h>
#include "backend.h"

/* ── geometry constants ──────────────────────────────────────────────── */

#define CELL_GAP  4      /* gap between character cells */
#define SPACE_W   8      /* width of space character */
#define TITLE_Y0  3      /* screen row of top of font */

#define SUB_SP_W   2      /* small space width */
#define SUB_GAP    1      /* small cell gap */
#define SUBTITLE_Y0 34    /* top of subtitle (title ends at ~30, 3px gap) */

#define TALLY_Y0 35
#define TALLY_Y1 38
#define TALLY_X0   3     /* leftmost tally x (first mark) */
#define TALLY_GAP  6     /* pixels between tally marks */

/* ── character table ─────────────────────────────────────────────────── */

static const Seg * const kCharSegs[] = {
    seg_V, seg_E, seg_C, seg_T, seg_O, seg_R,  /* V E C T O R */
    NULL,                                        /* space       */
    seg_Q, seg_U, seg_E, seg_S, seg_T           /* Q U E S T   */
};

#define HUD_NCHARS ((int)(sizeof(kCharSegs) / sizeof(kCharSegs[0])))

/* "GEMTOS 2026 EDITION" — subtitle, 19 characters */
static const Seg * const kSubSegs[] = {
    seg_G, seg_E, seg_M, seg_T, seg_O, seg_S,       /* G E M T O S */
    NULL,                                             /* space       */
    seg_2, seg_O, seg_2, seg_6,                      /* 2 0 2 6     */
    NULL,                                             /* space       */
    seg_E, seg_D, seg_I, seg_T, seg_I, seg_O, seg_N /* E D I T I O N */
};
#define HUD_NSUB ((int)(sizeof(kSubSegs) / sizeof(kSubSegs[0])))

/* ── drawing helpers ─────────────────────────────────────────────────── */

static void draw_char(const Seg *segs, int16_t ox, int16_t oy, int8_t sx, int8_t sy) {
    const Seg *s;
    for (s = segs; s->x0 >= 0; s++)
        backend_hud_line(ox + s->x0 * sx, oy + s->y0 * sy,
                         ox + s->x1 * sx, oy + s->y1 * sy);
}

/* ── public API ──────────────────────────────────────────────────────── */

/* Title row layout: 11 big glyphs plus one inter-word space, centred.
 * (SPACE_W+CELL_GAP)-CELL_GAP collapses to SPACE_W. */
#define HUD_TITLE_W  (11 * FONT_BIG_STEP + SPACE_W)
#define HUD_TITLE_OX ((SCREEN_WIDTH - HUD_TITLE_W) / 2)

/* Pixel x of the i-th character's left edge. */
static int16_t letter_ox(int8_t idx) {
    int16_t ox = HUD_TITLE_OX;
    int8_t j;
    for (j = 0; j < idx; j++)
        ox += (kCharSegs[j] == NULL) ? (SPACE_W + CELL_GAP) : FONT_BIG_STEP;
    return ox;
}

/* Pixel x of the i-th subtitle character's left edge (right-aligned to title). */
static int16_t subletter_ox(int8_t idx) {
    const int16_t title_rx = HUD_TITLE_OX + HUD_TITLE_W;
    int16_t sub_w    = 0;
    int8_t  j;
    for (j = 0; j < HUD_NSUB; j++)
        sub_w += (kSubSegs[j] == NULL) ? (SUB_SP_W + SUB_GAP) : FONT_SML_STEP;
    sub_w -= SUB_GAP;
    int16_t ox = title_rx - sub_w;
    for (j = 0; j < idx; j++)
        ox += (kSubSegs[j] == NULL) ? (SUB_SP_W + SUB_GAP) : FONT_SML_STEP;
    return ox;
}

static void hud_begin(void) { backend_hud_begin(); }

static int hud_draw_letter(int8_t i) {
    if (kCharSegs[i]) {
        draw_char(kCharSegs[i], letter_ox(i), TITLE_Y0, FONT_BIG_SX, FONT_BIG_SY);
        return 1;
    }
    return 0;
}

static int hud_draw_subletter(int8_t i) {
    if (i >= HUD_NSUB || kSubSegs[i] == NULL) return 0;
    draw_char(kSubSegs[i], subletter_ox(i), SUBTITLE_Y0, FONT_SML_SX, FONT_SML_SY);
    return 1;
}

static void hud_draw_tally(int round) {
    int16_t i;                       /* int16: round is uncapped, int8_t wraps past 128 */
    int16_t x = TALLY_X0;
    for (i = 0; i < round - 1; i++, x += TALLY_GAP)
        backend_hud_line(x, TALLY_Y0, x, TALLY_Y1);
}

/* Draw the whole HUD at once (the animated reveal in vquest.c calls the same
 * hud_begin/hud_draw_letter/hud_draw_subletter/hud_draw_tally helpers a glyph
 * at a time).  Title-screen call, so letter_ox's O(n) re-walk is irrelevant. */
static void hud_draw(int round) {
    int8_t i;
    hud_begin();
    for (i = 0; i < HUD_NCHARS; i++) hud_draw_letter(i);
    for (i = 0; i < HUD_NSUB;   i++) hud_draw_subletter(i);
    hud_draw_tally(round);
}
