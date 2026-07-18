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

static const char kTitle[] = "VECTOR QUEST";
#define HUD_NCHARS ((int)(sizeof(kTitle) - 1))

/* "ADN 2026 EDITION" — subtitle, 16 characters.  Kept as a glyph-pointer
 * array (not draw_text/glyph_for): the "0" in "2026" deliberately draws
 * seg_O (the letter, chamfered) rather than seg_0 (the digit, a plain
 * rectangle) to match this font's rounded style — glyph_for's digit mapping
 * would silently swap that glyph back. */
static const Seg * const kSubSegs[] = {
    seg_A, seg_D, seg_N,                             /* A D N       */
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
        ox += (kTitle[j] == ' ') ? (SPACE_W + CELL_GAP) : FONT_BIG_STEP;
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
    if (kTitle[i] != ' ') {
        draw_char(glyph_for(kTitle[i]), letter_ox(i), TITLE_Y0, FONT_BIG_SX, FONT_BIG_SY);
        return 1;
    }
    return 0;
}

static int hud_draw_subletter(int8_t i) {
    if (i >= HUD_NSUB || kSubSegs[i] == NULL) return 0;
    draw_char(kSubSegs[i], subletter_ox(i), SUBTITLE_Y0, FONT_SML_SX, FONT_SML_SY);
    return 1;
}

/* Marks that fit on the row: SegmentedLine does no clipping, and past x=319
 * its address math wraps into the next screen row. */
#define TALLY_MAX ((SCREEN_WIDTH - 1 - TALLY_X0) / TALLY_GAP + 1)

static __attribute__((noinline)) void hud_draw_tally(int round) {
    int16_t i;                       /* int16: round is uncapped, int8_t wraps past 128 */
    int16_t n = (int16_t)(round - 1);
    int16_t x = TALLY_X0;
    if (n > TALLY_MAX) n = TALLY_MAX;
    for (i = 0; i < n; i++, x += TALLY_GAP)
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
