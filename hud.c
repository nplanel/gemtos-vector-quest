/*
 * hud.c — HUD overlay: "VECTOR QUEST" title + subtitle.
 *
 * Font: single-stroke vector, 5-column × 8-row unit grid (x:0-4, y:0-7),
 * 4px per unit → 16×28px per cell.  Retro-futurist style: geometric,
 * 45°/90° strokes only, chamfered corners on enclosed shapes.
 *
 * Layout (top 40 rows):
 *   Title    — rows TITLE_Y0 .. TITLE_Y0+CELL_H-1  (centred)
 *   Subtitle — row  SUBTITLE_Y0
 */

#include <stddef.h>
#include "backend.h"
#include "vquest.h"   /* LINK_* */

/* ── geometry constants ──────────────────────────────────────────────── */

#define CELL_GAP  4      /* gap between character cells */
#define SPACE_W   8      /* width of space character */
#define TITLE_Y0  3      /* screen row of top of font */

#define SUB_SP_W   2      /* small space width */
#define SUB_GAP    1      /* small cell gap */
#define SUBTITLE_Y0 34    /* top of subtitle (title ends at ~30, 3px gap) */

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

/* Status row, left-aligned under the title's left edge:
 *   "2 PLAYERS F1 [link]   50HZ F2"   ... "ADN 2026 EDITION"
 * The subtitle starts at x=185.  backend_hud_clear_rect needs 16-px aligned
 * x/w, which is why the row starts at x=32 rather than the title's x=46.
 *
 * Mode label + link icon share one box (x 32..128) and redraw together: the
 * icon sits 5 px after the longest text so it reads as part of "2 PLAYERS",
 * and link transitions are rare enough that redrawing the text is free.
 * The icon is a 2-player thing only: absent in 1 player, and always present
 * in 2 players — bare end caps while no peer is heard, a broken chain for a
 * poor link, a solid one for a good link. */
#define MODE_TEXT_X   32
#define MODE_TEXT_Y   SUBTITLE_Y0
#define MODE_BOX_X    32
#define MODE_BOX_W    96
#define MODE_BOX_H     8
#define LINK_X0      109    /* "2 PLAYERS F1" ends at x=103 */
#define LINK_X1      122
#define LINK_Y        37

static void draw_small_text(const char *s, int16_t x, int16_t y) {
    for (; *s; s++) {
        if (*s == ' ') { x = (int16_t)(x + SUB_SP_W + SUB_GAP); continue; }
        draw_char(glyph_for(*s), x, y, FONT_SML_SX, FONT_SML_SY);
        x = (int16_t)(x + FONT_SML_STEP);
    }
}

static void hud_draw_mode(bool one_player, uint8_t link_state) {
    backend_hud_clear_rect(MODE_BOX_X, MODE_TEXT_Y, MODE_BOX_W, MODE_BOX_H);
    draw_small_text(one_player ? "1 PLAYER F1" : "2 PLAYERS F1", MODE_TEXT_X, MODE_TEXT_Y);
    if (one_player) return;
    backend_hud_line(LINK_X0, LINK_Y - 2, LINK_X0, LINK_Y + 2);   /* end caps */
    backend_hud_line(LINK_X1, LINK_Y - 2, LINK_X1, LINK_Y + 2);
    if (link_state == LINK_NONE) { backend_hud_note("none"); return; }
    if (link_state == LINK_OK) {
        backend_hud_line(LINK_X0, LINK_Y, LINK_X1, LINK_Y);           /* unbroken chain */
        backend_hud_note("ok");
    } else {
        backend_hud_line(LINK_X0, LINK_Y, LINK_X0 + 4, LINK_Y);       /* broken chain */
        backend_hud_line(LINK_X1 - 4, LINK_Y, LINK_X1, LINK_Y);
        backend_hud_note("bad");
    }
}

/* "50HZ F2" / "60HZ F2" refresh-rate indicator, after the mode box and
 * clear of the subtitle (text ends at x=173).  Redrawn only on the F2 edge
 * (see vquest.c): the toggle fires inside the IKBD interrupt handler, not
 * through gKeyState. */
#define HZ_TEXT_X   136
#define HZ_TEXT_Y   SUBTITLE_Y0
#define HZ_BOX_X    128
#define HZ_BOX_W     48
#define HZ_BOX_H      8

static void hud_draw_hz(uint8_t hz) {
    backend_hud_clear_rect(HZ_BOX_X, HZ_TEXT_Y, HZ_BOX_W, HZ_BOX_H);
    char s[8];
    s[0] = (char)('0' + hz / 10);
    s[1] = (char)('0' + hz % 10);
    s[2] = 'H'; s[3] = 'Z'; s[4] = ' '; s[5] = 'F'; s[6] = '2'; s[7] = 0;
    draw_small_text(s, HZ_TEXT_X, HZ_TEXT_Y);
}

