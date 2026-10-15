/* draw.c — shared line buffer, vector font data, and font drawing.
 * Unity-included from main_*.c; not a standalone translation unit. */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "backend.h"

/* Measured peaks per batch (30,000 autopiloted frames, 2026-10-09): the gate
 * credits are a batch of their own, 300 lines (~65 glyphs; see draw_credits);
 * the alien batch peaks at 260; the debug overlay adds ~205 to the world
 * batch.  Re-measure before adding anything to the credits or the overlay.
 * Overflow is silent (append_line drops the excess), so it shows up as a
 * truncated readout, not a crash. */
#define MAX_DRAW_LINES  512

typedef struct { int8_t x0, y0, x1, y1; } Seg;

/* The batch buffer, plus one slot for the x0 == 0 sentinel SegmentedMultiLine
 * stops on.  Static, not malloc'd: it is the same RAM either way (heap vs
 * BSS), but append_line is the hottest function in the frame and a pointer in
 * BSS costs it a load per call, and main() needs no allocation failure path
 * before the backend is even up. */
static Line     gLines[MAX_DRAW_LINES + 1];
static Line    *gLineEnd;   /* next free slot: a pointer, not a count, so
                             * append_line stores through it with no index
                             * arithmetic (−1.5k cycles per cruise frame) */

/* Dirty y-range of the lines appended since the last lines_reset().
 * The Atari backend merges it per buffer so backend_clear() erases only
 * the rows actually touched; other backends ignore it. */
static int16_t  gLinesYMin, gLinesYMax;

static inline uint16_t lines_count(void) {
    return (uint16_t)(gLineEnd - gLines);
}

static inline void lines_reset(void) {
    gLineEnd   = gLines;
    gLinesYMin = SCREEN_HEIGHT;
    gLinesYMax = -1;
}

/* Terminate the batch.  SegmentedMultiLine stops on the first Line with
 * x0 == 0, which no real line has (append_line's [1,W-1] contract).  gLines
 * has MAX_DRAW_LINES+1 slots so the sentinel always has room even after a
 * full batch.  Every backend_draw_*_lines() caller must seal first, or the
 * rasterizer runs off the end of the batch.  always_inline: one store, and
 * -Os outlines it once it has enough callers (measured +130 cycles/frame). */
static inline __attribute__((always_inline)) void lines_seal(void) {
    gLineEnd->p0.x = 0;
}

/* always_inline: at -Os gcc keeps this out of line, and the call (four
 * pushed args, two saved registers) costs as much as the body.  Inlining
 * every call site is worth ~7k cycles per cruise frame for ~1.3 kB of text. */
static inline __attribute__((always_inline))
void append_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    assert(lines_count() < MAX_DRAW_LINES);
    /* The Atari SegmentedLine assembly has no clipping: out-of-range coords
     * write outside the framebuffer.  Callers must pre-clamp to [1,W-1]x[1,H-1]. */
    assert(x0 >= 1 && x0 < SCREEN_WIDTH && y0 >= 1 && y0 < SCREEN_HEIGHT);
    assert(x1 >= 1 && x1 < SCREEN_WIDTH && y1 >= 1 && y1 < SCREEN_HEIGHT);
    /* -DNDEBUG strips the assert above in the shipping build: without this,
     * a batch that grows past MAX_DRAW_LINES would scribble past gLines'
     * allocation instead of just dropping a line. */
    if (unlikely(gLineEnd >= gLines + MAX_DRAW_LINES)) return;
    {   /* order the pair first: two memory compares instead of four */
        int16_t lo = y0, hi = y1;
        if (lo > hi) { lo = y1; hi = y0; }
        if (lo < gLinesYMin) gLinesYMin = lo;
        if (hi > gLinesYMax) gLinesYMax = hi;
    }
    {
        Line *l = gLineEnd;
        l->p0.x = x0; l->p0.y = y0; l->p1.x = x1; l->p1.y = y1;
        gLineEnd = l + 1;
    }
}

/* ── Font drawing ────────────────────────────────────────────────────────── */

/* A font is the one 5x8-grid glyph set at a scale.  The scale is a left
 * shift (x4, x2, x1), not a multiply, because font_draw scales four
 * coordinates per segment and muls.w costs ~40 cycles where lsl is ~8.
 * Passed by pointer: one address register instead of four scalar args at
 * every call site.  The FONT_*_STEP macros stay for compile-time layout. */
typedef struct {
    int8_t  shx, shy;   /* glyph scale, as left shifts          */
    int16_t step;       /* advance per glyph, px                */
    int16_t space;      /* advance per ' ', px                  */
} Font;

#define FONT_BIG_STEP  20
#define FONT_MED_STEP  11
#define FONT_SML_STEP   6
#define FONT_SML_SPACE  3

static const Font kFontBig = { 2, 2, FONT_BIG_STEP, 6 };
static const Font kFontMed = { 1, 1, FONT_MED_STEP, 6 };
static const Font kFontSml = { 0, 0, FONT_SML_STEP, FONT_SML_SPACE };

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

static const Seg seg_mine[] = {     /* HUD mine icon: draw_mine's apex-up triangle */
    { 0,7, 3,1 }, { 3,1, 6,7 }, { 6,7, 0,7 },
    { -1,0, 0,0 }
};
static const Seg seg_tick[] = {     /* HUD tick, one per mine left */
    { 0,3, 0,7 },
    { -1,0, 0,0 }
};

/* The small-font glyphs a backend may keep pre-rendered (see
 * backend_draw_marker_glyph / backend_hud_glyphs), by index: the digits first,
 * so a digit is its own index, then the opponent marker's chevrons, then the
 * HUD race readout's letters and icons.  At most 16 px wide and 8 rows. */
#define GLYPH_UP       10
#define GLYPH_DOWN     11
#define GLYPH_L        12
#define GLYPH_A        13
#define GLYPH_P        14
#define GLYPH_MINE     15
#define GLYPH_TICK     16
#define NUM_GLYPHS     17
static const Seg * const kGlyphSegs[NUM_GLYPHS] = {
    seg_0, seg_1, seg_2, seg_3, seg_4,
    seg_5, seg_6, seg_7, seg_8, seg_9,
    seg_up, seg_dn,
    seg_L, seg_A, seg_P, seg_mine, seg_tick
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
    if (c >= '0' && c <= '9') return kGlyphSegs[c - '0'];
    return NULL;   /* ' ' and anything unknown */
}

static void font_draw(const Seg *s, int16_t ox, int16_t oy, const Font *f) {
    int8_t shx = f->shx, shy = f->shy;
    for (; s->x0 >= 0; s++)
        append_line(S16(ox + (s->x0 << shx)), S16(oy + (s->y0 << shy)),
                    S16(ox + (s->x1 << shx)), S16(oy + (s->y1 << shy)));
}

/* Draw a non-negative number at (x,y); a negative one draws nothing.
 * Returns the x position after the last digit drawn. */
static int16_t draw_number(int16_t val, int16_t x, int16_t y, const Font *f) {
    uint8_t digits[5];
    int n = 0, i;
    if (val < 0) return x;
    do {
        digits[n++] = (uint8_t)(val % 10);
        val /= 10;
    } while (val > 0);
    for (i = n - 1; i >= 0; i--) {
        font_draw(kGlyphSegs[digits[i]], x, y, f);
        x = S16(x + f->step);
    }
    return x;
}

/* Draw the NUL-terminated string s at (x,y).  ' ' advances x by f->space
 * without drawing; any other glyph draws and advances by f->step.  Returns
 * the x after the last glyph, like draw_number. */
static int16_t draw_text(const char *s, int16_t x, int16_t y, const Font *f) {
    for (; *s; s++) {
        if (*s == ' ') { x = S16(x + f->space); continue; }
        const Seg *g = glyph_for(*s);
        /* NDEBUG strips the assert, and font_draw would then walk a NULL Seg*
         * and feed unclamped coords to a rasterizer with no clipping.  J/W/X
         * are intentionally NULL in kAlphaSegs, so this is reachable by a
         * one-character typo in any caller's string literal. */
        assert(g);
        if (g) font_draw(g, x, y, f);
        x = S16(x + f->step);
    }
    return x;
}
