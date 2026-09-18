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

/* Peer link-health indicator: a 16x8 box in the empty strip left of the
 * subtitle (rows 34-41, x 46-184 are free of both title and subtitle).
 * x and w must be 16-px aligned (backend_hud_clear_rect's contract). */
#define LINK_BOX_X 48
#define LINK_BOX_Y 34
#define LINK_BOX_W 16
#define LINK_BOX_H  8

static void hud_draw_link(uint8_t state) {
    backend_hud_clear_rect(LINK_BOX_X, LINK_BOX_Y, LINK_BOX_W, LINK_BOX_H);
    if (state == LINK_NONE) { backend_hud_note("none"); return; }
    backend_hud_line(49, 35, 49, 39);          /* end caps */
    backend_hud_line(62, 35, 62, 39);
    if (state == LINK_OK) {
        backend_hud_line(49, 37, 62, 37);      /* unbroken chain */
        backend_hud_note("ok");
    } else {
        backend_hud_line(49, 37, 53, 37);      /* broken chain */
        backend_hud_line(58, 37, 62, 37);
        backend_hud_note("bad");
    }
}

/* "1 PLAYER" / "2 PLAYERS" mode label, next to the link box (which ends at
 * x=64); redrawn whenever a real peer is detected/lost, same as
 * hud_draw_link().  Cleared with backend_hud_clear_rect first since
 * "2 PLAYERS" is wider than "1 PLAYER" and would otherwise leave stale
 * glyphs; x/w rounded to the 16-px alignment that call requires. */
#define MODE_TEXT_X   72
#define MODE_TEXT_Y   SUBTITLE_Y0
#define MODE_BOX_X    64
#define MODE_BOX_W    64
#define MODE_BOX_H     8

static void hud_draw_mode(bool bot_enabled) {
    backend_hud_clear_rect(MODE_BOX_X, MODE_TEXT_Y, MODE_BOX_W, MODE_BOX_H);
    const char *s = bot_enabled ? "1 PLAYER" : "2 PLAYERS";
    int16_t x = MODE_TEXT_X;
    for (; *s; s++) {
        if (*s == ' ') { x = (int16_t)(x + SUB_SP_W + SUB_GAP); continue; }
        draw_char(glyph_for(*s), x, MODE_TEXT_Y, FONT_SML_SX, FONT_SML_SY);
        x = (int16_t)(x + FONT_SML_STEP);
    }
}

/* "50HZ F1" / "60HZ F1" refresh-rate indicator, in the gap between the mode
 * label (ends at x=128) and the right-aligned subtitle (starts at x=185).
 * Polled once per frame (see vquest.c) since the F1 toggle fires inside the
 * IKBD interrupt handler, not through gKeyState. */
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
    s[2] = 'H'; s[3] = 'Z'; s[4] = ' '; s[5] = 'F'; s[6] = '1'; s[7] = 0;
    int16_t x = HZ_TEXT_X;
    for (const char *p = s; *p; p++) {
        if (*p == ' ') { x = (int16_t)(x + SUB_SP_W + SUB_GAP); continue; }
        draw_char(glyph_for(*p), x, HZ_TEXT_Y, FONT_SML_SX, FONT_SML_SY);
        x = (int16_t)(x + FONT_SML_STEP);
    }
}

