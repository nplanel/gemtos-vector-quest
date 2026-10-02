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
#include "vquest.h"   /* LINK_*, LAPS_PER_RACE */
#include "tuning.h"   /* MINES_PER_RACE */

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

/* HUD glyphs reuse draw.c's font code: they are built in the shared line
 * batch, then flushed to the HUD plane.  Safe because every HUD draw runs
 * between frames (or during the intro, after the credits batch has been
 * drawn), and each plane batch starts with its own lines_reset(). */
static void hud_flush(void) {
    uint16_t i;
    for (i = 0; i < gNLines; i++)
        backend_hud_line(gLines[i].p0.x, gLines[i].p0.y,
                         gLines[i].p1.x, gLines[i].p1.y);
    lines_reset();
}

static void draw_char(const Seg *segs, int16_t ox, int16_t oy, int8_t sx, int8_t sy) {
    lines_reset();
    font_draw(segs, ox, oy, sx, sy);
    hud_flush();
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

/* Small-font HUD text; returns the x after the last glyph. */
static int16_t draw_small_text(const char *s, int16_t x, int16_t y) {
    lines_reset();
    x = draw_text(s, x, y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP,
                  SUB_SP_W + SUB_GAP);
    hud_flush();
    return x;
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

/* Race readout, one centred row: "LAP n" left of centre, a mine icon and
 * one tick per mine left right of centre.  It sits in the empty band
 * between the HUD title block (ends y=41) and the eye-level band where
 * aliens and the ghost fly (y~100; the leading-opponent chevron is at
 * y=88), so the player reads it without moving their eyes far from the
 * action.  On the HUD plane because it changes a few times per race: drawn
 * once per change instead of every frame on a cleared plane (measured ~36k
 * cycles per cruise frame — a per-frame readout high on the screen drags the
 * plane 0/1 clear up with it; the HUD plane is never cleared per frame, so
 * its height is free).  Box x/w are 16-px aligned for backend_hud_clear_rect. */
#define RACE_Y        60
#define RACE_GAP       8                  /* each side of screen centre    */
#define RACE_LAP_W    29                  /* "LAP" + space + one digit     */
#define RACE_LAP_X    (SCREEN_WIDTH / 2 - RACE_GAP - RACE_LAP_W)
#define RACE_MINE_X   (SCREEN_WIDTH / 2 + RACE_GAP)
#define RACE_TICKS_X  (RACE_MINE_X + 10)  /* after the 7-px mine icon      */
#define RACE_TICK_Y   (RACE_Y + 3)        /* ticks bottom-aligned with text */
#define RACE_TICK_H    4
#define RACE_BOX_X    112
#define RACE_BOX_W     96
#define RACE_BOX_H     8
_Static_assert(RACE_BOX_X <= RACE_LAP_X &&
               RACE_TICKS_X + 5 * (MINES_PER_RACE - 1) < RACE_BOX_X + RACE_BOX_W,
               "race readout must fit its clear box");

/* Mine icon: apex-up triangle, the in-world mine's shape (draw_mine). */
static const Seg kSegMineIcon[] = {
    { 0,6, 3,0 }, { 3,0, 6,6 }, { 6,6, 0,6 },
    { -1,0, 0,0 }
};

static void hud_draw_race(bool show, uint8_t lap, uint8_t mines_left) {
    int16_t x;
    uint8_t i;
    backend_hud_clear_rect(RACE_BOX_X, RACE_Y, RACE_BOX_W, RACE_BOX_H);
    if (!show) return;
    x = draw_small_text("LAP", RACE_LAP_X, RACE_Y);
    draw_char(kDigitSegs[lap % 10], (int16_t)(x + FONT_SML_STEP), RACE_Y,
              FONT_SML_SX, FONT_SML_SY);
    draw_char(kSegMineIcon, RACE_MINE_X, RACE_Y + 1, 1, 1);
    for (i = 0, x = RACE_TICKS_X; i < mines_left; i++, x = (int16_t)(x + 5))
        backend_hud_line(x, RACE_TICK_Y, x, RACE_TICK_Y + RACE_TICK_H);
}

/* hud_update_race — per-frame entry point for the race readout: hidden at
 * the gate, redrawn only when what it shows changes (the HUD plane
 * persists, so the steady state costs one compare). */
static void hud_update_race(bool show, uint8_t lap, uint8_t mines_left) {
    static uint8_t shown;   /* 0 = hidden, else 0x80 | lap << 2 | mines_left */
    uint8_t key = show ? (uint8_t)(0x80 | (lap << 2) | mines_left) : 0;
    _Static_assert(LAPS_PER_RACE < 32 && MINES_PER_RACE < 4,
                   "readout key packs lap:5, mines:2");
    if (likely(key == shown)) return;
    hud_draw_race(show, lap, mines_left);
    shown = key;
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

