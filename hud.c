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
#include "hud.h"
#include "backend.h"

/* ── geometry constants ──────────────────────────────────────────────── */

#define SX        4      /* pixels per unit, x */
#define SY        4      /* pixels per unit, y */
#define CELL_W   16      /* SX * 4 units */
#define CELL_GAP  4      /* gap between character cells */
#define CELL_STEP 20     /* CELL_W + CELL_GAP */
#define SPACE_W   8      /* width of space character */
#define TITLE_Y0  3      /* screen row of top of font */

#define SX_S       1      /* small font: pixels per unit, x */
#define SY_S       1      /* small font: pixels per unit, y */
#define SUB_STEP   5      /* small cell step (4*SX_S + 1px gap) */
#define SUB_SP_W   2      /* small space width */
#define SUB_GAP    1      /* small cell gap */
#define SUBTITLE_Y0 34    /* top of subtitle (title ends at ~30, 3px gap) */

#define TALLY_Y0 35
#define TALLY_Y1 38
#define TALLY_X0   3     /* leftmost tally x (first mark) */
#define TALLY_GAP  6     /* pixels between tally marks */

/* ── font segment data ───────────────────────────────────────────────── */
/* Each segment is {x0, y0, x1, y1} in unit coords. -1 terminates. */

typedef struct { int8_t x0, y0, x1, y1; } Seg;

/* V — two diagonals from top corners meeting at base-centre */
static const Seg seg_V[] = {
    { 0,0, 2,7 }, { 4,0, 2,7 },
    { -1,0, 0,0 }
};

/* E — spine + 3 horizontals (mid bar is shorter) */
static const Seg seg_E[] = {
    { 0,0, 0,7 },
    { 0,0, 4,0 },
    { 0,4, 3,4 },
    { 0,7, 4,7 },
    { -1,0, 0,0 }
};

/* C — open right with chamfered top-right and bottom-right corners */
static const Seg seg_C[] = {
    { 4,1, 3,0 }, { 3,0, 0,0 },
    { 0,0, 0,7 },
    { 0,7, 3,7 }, { 3,7, 4,6 },
    { -1,0, 0,0 }
};

/* T — top bar + centred vertical */
static const Seg seg_T[] = {
    { 0,0, 4,0 },
    { 2,0, 2,7 },
    { -1,0, 0,0 }
};

/* O — chamfered octagon */
static const Seg seg_O[] = {
    { 1,0, 3,0 }, { 3,0, 4,1 },
    { 4,1, 4,6 },
    { 4,6, 3,7 }, { 3,7, 1,7 },
    { 1,7, 0,6 },
    { 0,6, 0,1 },
    { 0,1, 1,0 },
    { -1,0, 0,0 }
};

/* R — spine, upper bowl, diagonal leg */
static const Seg seg_R[] = {
    { 0,0, 0,7 },
    { 0,0, 3,0 }, { 3,0, 4,1 }, { 4,1, 4,3 }, { 4,3, 3,4 }, { 3,4, 0,4 },
    { 2,4, 4,7 },
    { -1,0, 0,0 }
};

/* Q — chamfered octagon + diagonal tail */
static const Seg seg_Q[] = {
    { 1,0, 3,0 }, { 3,0, 4,1 },
    { 4,1, 4,6 },
    { 4,6, 3,7 }, { 3,7, 1,7 },
    { 1,7, 0,6 },
    { 0,6, 0,1 },
    { 0,1, 1,0 },
    { 3,5, 4,7 },
    { -1,0, 0,0 }
};

/* U — two verticals + chamfered base */
static const Seg seg_U[] = {
    { 0,0, 0,6 }, { 0,6, 1,7 }, { 1,7, 3,7 }, { 3,7, 4,6 }, { 4,6, 4,0 },
    { -1,0, 0,0 }
};

/* S — top arc, diagonal cross, bottom arc */
static const Seg seg_S[] = {
    { 4,0, 1,0 }, { 1,0, 0,1 }, { 0,1, 0,3 },
    { 0,3, 4,4 },
    { 4,4, 4,6 }, { 4,6, 3,7 }, { 3,7, 0,7 },
    { -1,0, 0,0 }
};

/* G — like C but with inner horizontal stub at mid-right */
static const Seg seg_G[] = {
    { 4,1, 3,0 }, { 3,0, 0,0 }, { 0,0, 0,7 }, { 0,7, 3,7 }, { 3,7, 4,6 },
    { 4,6, 4,4 }, { 4,4, 2,4 },
    { -1,0, 0,0 }
};

/* M — two verticals + V-peak */
static const Seg seg_M[] = {
    { 0,0, 0,7 }, { 0,0, 2,4 }, { 2,4, 4,0 }, { 4,0, 4,7 },
    { -1,0, 0,0 }
};

/* 2 — top arc + diagonal sweep + base */
static const Seg seg_2[] = {
    { 0,1, 1,0 }, { 1,0, 3,0 }, { 3,0, 4,1 }, { 4,1, 4,3 },
    { 4,3, 0,7 }, { 0,7, 4,7 },
    { -1,0, 0,0 }
};

/* 6 — top open, full left side, closed bottom loop, mid bar */
static const Seg seg_6[] = {
    { 3,0, 1,0 }, { 1,0, 0,1 }, { 0,1, 0,6 }, { 0,6, 1,7 },
    { 1,7, 3,7 }, { 3,7, 4,6 }, { 4,6, 4,4 }, { 4,4, 0,4 },
    { -1,0, 0,0 }
};

/* D — spine + rounded right side */
static const Seg seg_D[] = {
    { 0,0, 0,7 }, { 0,0, 3,0 }, { 3,0, 4,1 }, { 4,1, 4,6 },
    { 4,6, 3,7 }, { 3,7, 0,7 },
    { -1,0, 0,0 }
};

/* I — vertical with serifs */
static const Seg seg_I[] = {
    { 0,0, 4,0 }, { 2,0, 2,7 }, { 0,7, 4,7 },
    { -1,0, 0,0 }
};

/* N — two verticals + diagonal */
static const Seg seg_N[] = {
    { 0,0, 0,7 }, { 0,0, 4,7 }, { 4,0, 4,7 },
    { -1,0, 0,0 }
};

/* ── character table ─────────────────────────────────────────────────── */

static const Seg * const kCharSegs[] = {
    seg_V, seg_E, seg_C, seg_T, seg_O, seg_R,  /* V E C T O R */
    NULL,                                        /* space       */
    seg_Q, seg_U, seg_E, seg_S, seg_T           /* Q U E S T   */
};

#define NCHARS ((int)(sizeof(kCharSegs) / sizeof(kCharSegs[0])))

/* "GEMTOS 2026 EDITION" — subtitle, 19 characters */
static const Seg * const kSubSegs[] = {
    seg_G, seg_E, seg_M, seg_T, seg_O, seg_S,       /* G E M T O S */
    NULL,                                             /* space       */
    seg_2, seg_O, seg_2, seg_6,                      /* 2 0 2 6     */
    NULL,                                             /* space       */
    seg_E, seg_D, seg_I, seg_T, seg_I, seg_O, seg_N /* E D I T I O N */
};
#define NSUB ((int)(sizeof(kSubSegs) / sizeof(kSubSegs[0])))

/* ── drawing helpers ─────────────────────────────────────────────────── */

static void draw_char(const Seg *segs, int16_t ox, int16_t oy, int8_t sx, int8_t sy) {
    const Seg *s;
    for (s = segs; s->x0 >= 0; s++)
        backend_hud_line(ox + s->x0 * sx, oy + s->y0 * sy,
                         ox + s->x1 * sx, oy + s->y1 * sy);
}

/* ── public API ──────────────────────────────────────────────────────── */

/* Pixel x of the i-th character's left edge. */
static int16_t letter_ox(int8_t idx) {
    int16_t total_w = 11 * CELL_STEP + (SPACE_W + CELL_GAP) - CELL_GAP;
    int16_t ox = (SCREEN_WIDTH - total_w) / 2;
    int8_t j;
    for (j = 0; j < idx; j++)
        ox += (kCharSegs[j] == NULL) ? (SPACE_W + CELL_GAP) : CELL_STEP;
    return ox;
}

/* Pixel x of the i-th subtitle character's left edge (right-aligned to title). */
static int16_t subletter_ox(int8_t idx) {
    int16_t title_w  = 11 * CELL_STEP + (SPACE_W + CELL_GAP) - CELL_GAP;
    int16_t title_rx = (SCREEN_WIDTH - title_w) / 2 + title_w;
    int16_t sub_w    = 0;
    int8_t  j;
    for (j = 0; j < NSUB; j++)
        sub_w += (kSubSegs[j] == NULL) ? (SUB_SP_W + SUB_GAP) : SUB_STEP;
    sub_w -= SUB_GAP;
    int16_t ox = title_rx - sub_w;
    for (j = 0; j < idx; j++)
        ox += (kSubSegs[j] == NULL) ? (SUB_SP_W + SUB_GAP) : SUB_STEP;
    return ox;
}

void hud_begin(void) { backend_hud_begin(); }

int hud_draw_letter(int8_t i) {
    if (kCharSegs[i]) {
        draw_char(kCharSegs[i], letter_ox(i), TITLE_Y0, SX, SY);
        return 1;
    }
    return 0;
}

int hud_draw_subletter(int8_t i) {
    if (i >= NSUB || kSubSegs[i] == NULL) return 0;
    draw_char(kSubSegs[i], subletter_ox(i), SUBTITLE_Y0, SX_S, SY_S);
    return 1;
}

void hud_draw_tally(int round) {
    int8_t  i;
    int16_t x = TALLY_X0;
    for (i = 0; i < round - 1; i++, x += TALLY_GAP)
        backend_hud_line(x, TALLY_Y0, x, TALLY_Y1);
}

void hud_draw(int round) {
    int8_t  i;
    int16_t total_w = 11 * CELL_STEP + (SPACE_W + CELL_GAP) - CELL_GAP;
    int16_t ox = (SCREEN_WIDTH - total_w) / 2;
    backend_hud_begin();
    for (i = 0; i < NCHARS; i++) {
        if (kCharSegs[i])
            draw_char(kCharSegs[i], ox, TITLE_Y0, SX, SY);
        ox += (kCharSegs[i] == NULL) ? (SPACE_W + CELL_GAP) : CELL_STEP;
    }
    for (i = 0; i < NSUB; i++) {
        if (kSubSegs[i])
            draw_char(kSubSegs[i], subletter_ox(i), SUBTITLE_Y0, SX_S, SY_S);
    }
    hud_draw_tally(round);
}
