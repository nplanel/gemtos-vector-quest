/* draw.c — shared line buffer, vector font data, and font drawing.
 * Unity-included from main_*.c; not a standalone translation unit. */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "backend.h"

#define MAX_DRAW_LINES  512

typedef struct { int8_t x0, y0, x1, y1; } Seg;

static Line    *gLines;
static uint16_t gNLines;

/* Dirty y-range of the lines appended since the last lines_reset().
 * The Atari backend merges it per buffer so backend_clear() erases only
 * the rows actually touched; other backends ignore it. */
static int16_t  gLinesYMin, gLinesYMax;

static inline void lines_reset(void) {
    gNLines    = 0;
    gLinesYMin = SCREEN_HEIGHT;
    gLinesYMax = -1;
}

static inline void append_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    assert(gNLines < MAX_DRAW_LINES);
    /* The Atari SegmentedLine assembly has no clipping: out-of-range coords
     * write outside the framebuffer.  Callers must pre-clamp to [1,W-1]x[1,H-1]. */
    assert(x0 >= 1 && x0 < SCREEN_WIDTH && y0 >= 1 && y0 < SCREEN_HEIGHT);
    assert(x1 >= 1 && x1 < SCREEN_WIDTH && y1 >= 1 && y1 < SCREEN_HEIGHT);
    /* -DNDEBUG strips the assert above in the shipping build: without this,
     * a batch that grows past MAX_DRAW_LINES would scribble past gLines'
     * allocation instead of just dropping a line. */
    if (gNLines >= MAX_DRAW_LINES) return;
    if (y0 < gLinesYMin) gLinesYMin = y0;
    if (y0 > gLinesYMax) gLinesYMax = y0;
    if (y1 < gLinesYMin) gLinesYMin = y1;
    if (y1 > gLinesYMax) gLinesYMax = y1;
    gLines[gNLines].p0.x = x0; gLines[gNLines].p0.y = y0;
    gLines[gNLines].p1.x = x1; gLines[gNLines].p1.y = y1;
    gNLines++;
}

/* ── Font drawing ────────────────────────────────────────────────────────── */

#define FONT_BIG_SX    4
#define FONT_BIG_SY    4
#define FONT_BIG_STEP 20

#define FONT_MED_SX    2
#define FONT_MED_SY    2
#define FONT_MED_STEP 11

#define FONT_SML_SX    1
#define FONT_SML_SY    1
#define FONT_SML_STEP  6

static const Seg seg_B[] = {
    { 0,0, 0,7 },
    { 0,0, 3,0 }, { 3,0, 4,1 }, { 4,1, 4,3 }, { 4,3, 3,4 }, { 3,4, 0,4 },
    { 3,4, 4,5 }, { 4,5, 4,6 }, { 4,6, 3,7 }, { 3,7, 0,7 },
    { -1,0, 0,0 }
};
static const Seg seg_C[] = {
    { 4,1, 3,0 }, { 3,0, 0,0 }, { 0,0, 0,7 }, { 0,7, 3,7 }, { 3,7, 4,6 },
    { -1,0, 0,0 }
};
static const Seg seg_D[] = {
    { 0,0, 0,7 }, { 0,0, 3,0 }, { 3,0, 4,1 }, { 4,1, 4,6 },
    { 4,6, 3,7 }, { 3,7, 0,7 },
    { -1,0, 0,0 }
};
static const Seg seg_A[] = {
    { 0,7, 2,0 }, { 4,7, 2,0 }, { 1,4, 3,4 },
    { -1,0, 0,0 }
};
static const Seg seg_E[] = {
    { 0,0, 0,7 }, { 0,0, 4,0 }, { 0,4, 3,4 }, { 0,7, 4,7 },
    { -1,0, 0,0 }
};
static const Seg seg_F[] = {
    { 0,0, 0,7 }, { 0,0, 4,0 }, { 0,4, 3,4 },
    { -1,0, 0,0 }
};
static const Seg seg_G[] = {
    { 4,1, 3,0 }, { 3,0, 0,0 }, { 0,0, 0,7 }, { 0,7, 3,7 }, { 3,7, 4,6 },
    { 4,6, 4,4 }, { 4,4, 2,4 },
    { -1,0, 0,0 }
};
static const Seg seg_H[] = {
    { 0,0, 0,7 }, { 4,0, 4,7 }, { 0,4, 4,4 },
    { -1,0, 0,0 }
};
static const Seg seg_I[] = {
    { 0,0, 4,0 }, { 2,0, 2,7 }, { 0,7, 4,7 },
    { -1,0, 0,0 }
};
static const Seg seg_K[] = {
    { 0,0, 0,7 }, { 4,0, 0,4 }, { 0,4, 4,7 },
    { -1,0, 0,0 }
};
static const Seg seg_L[] = {
    { 0,0, 0,7 }, { 0,7, 4,7 },
    { -1,0, 0,0 }
};
static const Seg seg_M[] = {
    { 0,0, 0,7 }, { 0,0, 2,4 }, { 2,4, 4,0 }, { 4,0, 4,7 },
    { -1,0, 0,0 }
};
static const Seg seg_N[] = {
    { 0,0, 0,7 }, { 0,0, 4,7 }, { 4,0, 4,7 },
    { -1,0, 0,0 }
};
static const Seg seg_O[] = {
    { 1,0, 3,0 }, { 3,0, 4,1 }, { 4,1, 4,6 }, { 4,6, 3,7 },
    { 3,7, 1,7 }, { 1,7, 0,6 }, { 0,6, 0,1 }, { 0,1, 1,0 },
    { -1,0, 0,0 }
};
static const Seg seg_P[] = {
    { 0,0, 0,7 },
    { 0,0, 3,0 }, { 3,0, 4,1 }, { 4,1, 4,3 }, { 4,3, 3,4 }, { 3,4, 0,4 },
    { -1,0, 0,0 }
};
static const Seg seg_Q[] = {
    { 1,0, 3,0 }, { 3,0, 4,1 }, { 4,1, 4,6 }, { 4,6, 3,7 },
    { 3,7, 1,7 }, { 1,7, 0,6 }, { 0,6, 0,1 }, { 0,1, 1,0 },
    { 3,5, 4,7 },
    { -1,0, 0,0 }
};
static const Seg seg_R[] = {
    { 0,0, 0,7 },
    { 0,0, 3,0 }, { 3,0, 4,1 }, { 4,1, 4,3 }, { 4,3, 3,4 }, { 3,4, 0,4 },
    { 2,4, 4,7 },
    { -1,0, 0,0 }
};
static const Seg seg_S[] = {
    { 4,0, 1,0 }, { 1,0, 0,1 }, { 0,1, 0,3 }, { 0,3, 4,4 },
    { 4,4, 4,6 }, { 4,6, 3,7 }, { 3,7, 0,7 },
    { -1,0, 0,0 }
};
static const Seg seg_T[] = {
    { 0,0, 4,0 }, { 2,0, 2,7 },
    { -1,0, 0,0 }
};
static const Seg seg_U[] = {
    { 0,0, 0,6 }, { 0,6, 1,7 }, { 1,7, 3,7 }, { 3,7, 4,6 }, { 4,6, 4,0 },
    { -1,0, 0,0 }
};
static const Seg seg_V[] = {
    { 0,0, 2,7 }, { 4,0, 2,7 },
    { -1,0, 0,0 }
};
static const Seg seg_Y[] = {
    { 0,0, 2,4 }, { 4,0, 2,4 }, { 2,4, 2,7 },
    { -1,0, 0,0 }
};
static const Seg seg_Z[] = {
    { 0,0, 4,0 }, { 4,0, 0,7 }, { 0,7, 4,7 },
    { -1,0, 0,0 }
};
static const Seg seg_2[] = {
    { 0,1, 1,0 }, { 1,0, 3,0 }, { 3,0, 4,1 }, { 4,1, 4,3 },
    { 4,3, 0,7 }, { 0,7, 4,7 },
    { -1,0, 0,0 }
};
static const Seg seg_6[] = {
    { 3,0, 1,0 }, { 1,0, 0,1 }, { 0,1, 0,6 }, { 0,6, 1,7 },
    { 1,7, 3,7 }, { 3,7, 4,6 }, { 4,6, 4,4 }, { 4,4, 0,4 },
    { -1,0, 0,0 }
};
static const Seg seg_times[] = {
    { 0,1, 4,6 }, { 4,1, 0,6 },
    { -1,0, 0,0 }
};
static const Seg seg_0[] = {
    { 0,1, 4,1 }, { 4,1, 4,6 }, { 4,6, 0,6 }, { 0,6, 0,1 },
    { -1,0, 0,0 }
};
static const Seg seg_1[] = {
    { 2,0, 2,7 }, { 1,7, 3,7 },
    { -1,0, 0,0 }
};
static const Seg seg_3[] = {
    { 0,0, 4,0 }, { 4,0, 4,3 }, { 0,4, 4,4 },
    { 4,4, 4,7 }, { 4,7, 0,7 },
    { -1,0, 0,0 }
};
static const Seg seg_4[] = {
    { 0,0, 0,3 }, { 3,0, 3,7 }, { 0,3, 4,3 },
    { -1,0, 0,0 }
};
static const Seg seg_5[] = {
    { 4,0, 0,0 }, { 0,0, 0,3 }, { 0,3, 4,4 },
    { 4,4, 4,7 }, { 4,7, 0,7 },
    { -1,0, 0,0 }
};
static const Seg seg_7[] = {
    { 0,0, 4,0 }, { 4,0, 0,7 },
    { -1,0, 0,0 }
};
static const Seg seg_8[] = {
    { 0,0, 4,0 }, { 4,0, 4,7 }, { 4,7, 0,7 },
    { 0,7, 0,0 }, { 0,4, 4,4 },
    { -1,0, 0,0 }
};
static const Seg seg_9[] = {
    { 0,0, 4,0 }, { 4,0, 4,7 }, { 0,4, 4,4 },
    { 0,0, 0,3 },
    { -1,0, 0,0 }
};
static const Seg seg_up[] = {       /* small up-chevron (opponent ahead) */
    { 0,3, 2,0 }, { 2,0, 4,3 },
    { -1,0, 0,0 }
};
static const Seg seg_dn[] = {       /* small down-chevron (opponent behind) */
    { 0,0, 2,3 }, { 2,3, 4,0 },
    { -1,0, 0,0 }
};

static const Seg * const kDigitSegs[] = {
    seg_0, seg_1, seg_2, seg_3, seg_4,
    seg_5, seg_6, seg_7, seg_8, seg_9
};

/* Glyphs for 'A'..'Z'; J, W, X are never drawn by any string in the game and
 * are NULL — glyph_for's caller (draw_text) asserts rather than render a
 * silent gap if one is ever referenced. */
static const Seg * const kAlphaSegs[26] = {
    seg_A, seg_B, seg_C, seg_D, seg_E, seg_F, seg_G, seg_H, seg_I,
    NULL,  seg_K, seg_L, seg_M, seg_N, seg_O, seg_P, seg_Q, seg_R,
    seg_S, seg_T, seg_U, seg_V, NULL,  NULL,  seg_Y, seg_Z
};

static const Seg *glyph_for(char c) {
    if (c >= 'A' && c <= 'Z') return kAlphaSegs[c - 'A'];
    if (c >= '0' && c <= '9') return kDigitSegs[c - '0'];
    return NULL;   /* ' ' and anything unknown */
}

static void font_draw(const Seg *s, int16_t ox, int16_t oy, int8_t sx, int8_t sy) {
    for (; s->x0 >= 0; s++)
        append_line((int16_t)(ox + s->x0 * sx), (int16_t)(oy + s->y0 * sy),
                    (int16_t)(ox + s->x1 * sx), (int16_t)(oy + s->y1 * sy));
}

/* Draw a number at (x,y) with the given scale and step.
 * Returns the x position after the last digit drawn. */
static int16_t draw_number(int16_t val, int16_t x, int16_t y,
                           int8_t sx, int8_t sy, int16_t step) {
    uint8_t digits[5];
    int n = 0, i;
    int16_t cx = x;
    if (val == 0) {
        font_draw(kDigitSegs[0], cx, y, sx, sy);
        return (int16_t)(cx + step);
    }
    while (val > 0 && n < 5) {
        digits[n++] = (uint8_t)(val % 10);
        val /= 10;
    }
    for (i = n - 1; i >= 0; i--) {
        font_draw(kDigitSegs[digits[i]], cx, y, sx, sy);
        cx = (int16_t)(cx + step);
    }
    return cx;
}

/* Draw the NUL-terminated string s at (x,y) with scale (sx,sy).
 * ' ' advances x by sp_w without drawing; any other glyph draws and
 * advances by step. */
static void draw_text(const char *s, int16_t x, int16_t y, int8_t sx, int8_t sy,
                       int16_t step, int16_t sp_w) {
    for (; *s; s++) {
        if (*s == ' ') { x = (int16_t)(x + sp_w); continue; }
        const Seg *g = glyph_for(*s);
        assert(g);
        font_draw(g, x, y, sx, sy);
        x = (int16_t)(x + step);
    }
}
