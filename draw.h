#ifndef DRAW_H
#define DRAW_H

/* draw.h — shared line buffer + vector font.
 *
 * All geometry (grid, aliens, credits) appends to the same scratch buffer
 * via append_line(), then flushes it with backend_draw_lines() /
 * backend_draw_alien_lines().  The buffer is reset (gNLines=0) before each
 * plane pass; a zero sentinel is written at gLines[gNLines] before the flush.
 *
 * Buffer size: must cover the largest single-plane segment count:
 *   alien plane  — NUM_EDGES logo segments (306 in current model)
 *   credits      — CREDITS_LINES (141)
 *   HUD          — HUD_MAX_LINES (153, drawn via backend_hud_line not gLines)
 * 400 gives comfortable headroom above the current 306-segment logo. */

#include <assert.h>
#include <stdint.h>
#include "backend.h"

#define MAX_DRAW_LINES  400

extern Line    *gLines;   /* MAX_DRAW_LINES+1 entries, +1 for zero sentinel */
extern uint16_t gNLines;

static inline void append_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    assert(gNLines < MAX_DRAW_LINES);
    gLines[gNLines].p0.x = x0; gLines[gNLines].p0.y = y0;
    gLines[gNLines].p1.x = x1; gLines[gNLines].p1.y = y1;
    gNLines++;
}

/* ── Vector font ─────────────────────────────────────────────────────────── */
/* Single-stroke, 4×7 unit grid (x: 0–4, y: 0–7).  Arrays terminated by
 * a sentinel entry with x0 = -1. */

typedef struct { int8_t x0, y0, x1, y1; } Seg;

extern const Seg seg_B[], seg_C[], seg_D[], seg_E[], seg_G[];
extern const Seg seg_I[], seg_M[], seg_N[], seg_O[], seg_P[];
extern const Seg seg_Q[], seg_R[], seg_S[], seg_T[], seg_U[];
extern const Seg seg_V[], seg_Y[];
extern const Seg seg_2[], seg_6[];
extern const Seg seg_times[];

/* Append the segments of one character, scaled by sx/sy pixels per unit. */
void font_draw(const Seg *s, int16_t ox, int16_t oy, int8_t sx, int8_t sy);

/* Max append_line calls in one credits_render() — used by callers that need
 * to know how much of the buffer will be consumed. */
#define CREDITS_LINES 142

#endif /* DRAW_H */
