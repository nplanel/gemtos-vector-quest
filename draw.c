/* draw.c — shared line buffer, vector font data, and font drawing.
 * Unity-included from main_*.c; not a standalone translation unit. */

#include <assert.h>
#include <stdint.h>
#include "backend.h"

#define MAX_DRAW_LINES  400

typedef struct { int8_t x0, y0, x1, y1; } Seg;

static Line    *gLines;
static uint16_t gNLines;

static inline void append_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    assert(gNLines < MAX_DRAW_LINES);
    gLines[gNLines].p0.x = x0; gLines[gNLines].p0.y = y0;
    gLines[gNLines].p1.x = x1; gLines[gNLines].p1.y = y1;
    gNLines++;
}

/* ── Segment data ───────────────────────────────────────────────────────── */

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
static const Seg seg_E[] = {
    { 0,0, 0,7 }, { 0,0, 4,0 }, { 0,4, 3,4 }, { 0,7, 4,7 },
    { -1,0, 0,0 }
};
static const Seg seg_G[] = {
    { 4,1, 3,0 }, { 3,0, 0,0 }, { 0,0, 0,7 }, { 0,7, 3,7 }, { 3,7, 4,6 },
    { 4,6, 4,4 }, { 4,4, 2,4 },
    { -1,0, 0,0 }
};
static const Seg seg_I[] = {
    { 0,0, 4,0 }, { 2,0, 2,7 }, { 0,7, 4,7 },
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

/* ── Font drawing ────────────────────────────────────────────────────────── */

static void font_draw(const Seg *s, int16_t ox, int16_t oy, int8_t sx, int8_t sy) {
    for (; s->x0 >= 0; s++)
        append_line((int16_t)(ox + s->x0 * sx), (int16_t)(oy + s->y0 * sy),
                    (int16_t)(ox + s->x1 * sx), (int16_t)(oy + s->y1 * sy));
}
