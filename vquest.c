#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#ifndef __m68k__
#include <endian.h>
#endif

#include "backend.h"
#include "hud.h"

/* Perspective/Model Constants */
#define LOGO_SCALE          (3.0f/230.0f)

typedef struct {
    float x, y, z;
} Point3DFloat;

typedef struct {
    int16_t x, y, z;
} Point3DInt;

typedef struct {
    int32_t x, y, z;
} Point3DLong;

#define FP_SHIFT 10
#define FP_ONE (1 << FP_SHIFT)

#define LUT_SIZE 2048

static int16_t sinLUT[LUT_SIZE];
static int16_t cosLUT[LUT_SIZE];

void loadLUTs() {
    FILE *fp = fopen("luts", "rb");
    fread(sinLUT, sizeof(sinLUT), 1, fp);
    fread(cosLUT, sizeof(cosLUT), 1, fp);
    /* skip logLUT and expLUT — no longer used */
    fseek(fp, (long)(sizeof(int16_t) * LUT_SIZE * 3), SEEK_SET);
    fclose(fp);
#ifndef __m68k__
    unsigned int i;
    for (i = 0; i < LUT_SIZE; i++) sinLUT[i] = (int16_t)be16toh((uint16_t)sinLUT[i]);
    for (i = 0; i < LUT_SIZE; i++) cosLUT[i] = (int16_t)be16toh((uint16_t)cosLUT[i]);
#endif
}

static inline int16_t fastSin(int16_t angle) {
    return sinLUT[angle & (LUT_SIZE-1)];
}

static inline int16_t fastCos(int16_t angle) {
    return cosLUT[angle & (LUT_SIZE-1)];
}

/* Fixed-point multiply: (a * b) >> FP_SHIFT.
 * GCC m68k emits muls.w (~70 cycles) + asr.l #10.
 * Exact integer result — no LUT quantization error. */
static inline int16_t mul_fp(int16_t a, int16_t b) {
    return (int16_t)(((int32_t)a * b) >> FP_SHIFT);
}

#include "vquest.h"

#define NUM_VERTICES (sizeof(vquest_vertices) / sizeof(vquest_vertices[0]))
Point3DLong gVerticesLongScale[NUM_VERTICES];
#define NUM_EDGES (sizeof(vquest_edges) / sizeof(vquest_edges[0]))

void model_scale() {
    unsigned i;
    for (i = 0; i < NUM_VERTICES; i++) {
        gVerticesLongScale[i].x = (int32_t)((vquest_vertices[i].x * LOGO_SCALE) * FP_ONE);
        gVerticesLongScale[i].y = (int32_t)((vquest_vertices[i].y * LOGO_SCALE) * FP_ONE);
        gVerticesLongScale[i].z = (int32_t)((vquest_vertices[i].z * LOGO_SCALE) * FP_ONE);
    }
}

/* Rotate vertex i around Y then X axes using precomputed sin/cos values.
 * Caller hoists the 4 trig lookups outside the per-vertex loop (PERF-2).
 * Vertex coords post-scale: max ~3072 (3*FP_ONE); after one rotation step
 * max grows by sqrt(2) → ~4344, well within int16_t range. */
static inline Point3DInt rotate(unsigned i,
    int16_t cosY, int16_t sinY, int16_t cosX, int16_t sinX) {
    Point3DInt p_out;
    int16_t x, y, z, temp_x, temp_z;

    const Point3DLong *p_in = &gVerticesLongScale[i];
    x = (int16_t)p_in->x;
    y = (int16_t)p_in->y;
    z = (int16_t)p_in->z;

    temp_x = (int16_t)(mul_fp(x, cosY) + mul_fp(z, sinY));
    temp_z = (int16_t)(mul_fp(x, sinY) + mul_fp(z, cosY));
    x = temp_x;
    z = temp_z;

    p_out.x = x;
    p_out.y = (int16_t)(mul_fp(y, cosX) + mul_fp(z, sinX));
    p_out.z = (int16_t)(mul_fp(y, sinX) + mul_fp(z, cosX));

    return p_out;
}

/* ── Camera / projection ──────────────────────────────────────────────────── */
#define FOCAL        128       /* focal length (2^7); gives ~102° HFOV; power-of-2
                                * so val*FOCAL == val<<7, avoiding muls on m68k   */
#define CAM_Y_INIT   ((int16_t)(FP_ONE / 4))   /* start 0.25 units above ground */
#define CAM_ZSPEED   64        /* Z advance per frame (= FP_ONE/16)              */

/* ── Takeoff / landing physics ────────────────────────────────────────────── *
 * Constants chosen so net per-frame deltas fit in addq/subq range (1-8)      *
 * on m68k: the compiler folds e.g. vel_y+=(TAKEOFF_THRUST-GRAVITY) → addq #4 */
#define TAKEOFF_THRUST  8   /* vel_y gain/frame holding Up (takeoff)            */
#define GRAVITY         4   /* vel_y loss/frame always                          */
#define BRAKE_THRUST    2   /* vel_y gain/frame holding Up (landing)            */
#define DRAG_SHIFT      4   /* drag: vel -= vel>>4  (GCC arithmetic shift ok)   */
#define VEL_Y_MAX     200
#define VEL_Y_MIN    -200
#define STEER           4   /* vel_x correction/frame from Left/Right keys      */
#define VEL_X_MAX       8
#define CRUISE_STEER      (4 * STEER)      /* 4× lateral accel in free-roam cruise  */
#define CRUISE_VEL_X_MAX  (6 * VEL_X_MAX)  /* 6× cap: crosses ±3-unit strip range   */
#define DESCENT_THRUST  GRAVITY   /* extra downward push/frame holding Down (landing) */

#define CRUISE_ALT    ((int16_t)(2 * FP_ONE))  /* altitude for TAKEOFF→CRUISE   */
#define CRASH_CAM_X   ((int16_t)(2 * FP_ONE))  /* lateral crash boundary        */
#define LAND_CAM_X    ((int16_t)(FP_ONE))       /* landing alignment zone        */
#define CRASH_VEL_Y    50   /* max descent speed for safe touchdown             */
#define CRASH_FLASH_FRAMES 30  /* ~0.6 s at 50 Hz                               */
#define CRUISE_DWELL  120   /* frames aligned with strip before landing begins  */

/* ── Physics interaction summary (all in vel units per frame) ──────────────── *
 * Takeoff:  net thrust = TAKEOFF_THRUST - GRAVITY = +4  (accelerates upward)  *
 * Braking:  net thrust = BRAKE_THRUST   - GRAVITY = -2  (descends slower)     *
 * → Landing requires timing Up key to bleed speed, not halt it.               *
 * Drag:     vel -= vel >> DRAG_SHIFT (~vel/16)  each frame after thrust.       *
 * Terminal velocity (no keys): gravity / drag_fraction ≈ 4 units/frame.       *
 * All thrust/gravity constants ≤ 8 → addq/subq on m68k; no muls.w in physics. */

/* ── Progressive difficulty ───────────────────────────────────────────────── *
 * Wind: fastSin(frame*wind_freq)>>wind_shift applied as lateral force.        *
 * wind_freq controls oscillation speed; wind_shift controls amplitude.        *
 * Takeoff timer: must reach CRUISE_ALT within takeoff_limit frames.           */
/* ── Fuel (Lunar Lander style) ────────────────────────────────────────────── *
 * MAX_FUEL=128 fits in uint8_t; bar width = fuel (no shift), full bar=128px. *
 * Horizontal gauge at right side, same height as HUD tally, shrinks left.   */
#define MAX_FUEL          128   /* total fuel units per round (fits uint8_t)   */
#define FUEL_THRUST_COST    2   /* fuel/frame holding Up                       */
#define FUEL_STEER_COST     1   /* fuel/frame holding Left or Right            */
#define FUEL_BAR_X1       316   /* right edge of horizontal fuel bar           */
#define FUEL_Y             36   /* y of fuel bar — matches HUD tally height    */
#define ARROW_SHAFT_X_LEFT   10  /* left arrow: shaft x                         */
#define ARROW_TIP_X_LEFT      3  /* left arrow: tip x (near left edge)          */
#define ARROW_SHAFT_X_RIGHT 310  /* right arrow: shaft x                        */
#define ARROW_TIP_X_RIGHT   317  /* right arrow: tip x (near right edge)        */
#define ARROW_Y_CENTER      100  /* == SCREEN_HEIGHT_HALF                       */
#define ARROW_Y_HALF          3  /* half-height of arrowhead (±3 rows)          */

#define TAKEOFF_FRAMES_BASE 120  /* ~2.4s at 50Hz — generous on round 1          */
#define TAKEOFF_FRAMES_STEP  10  /* shrinks per round                            */
#define TAKEOFF_FRAMES_MIN   40  /* floor: ~0.8s                                 */

/* ── Landing strip constants ─────────────────────────────────────────────── *
 * draw_ground_strip() projects a rectangle at a dynamic z each frame.        *
 * STRIP_HALF: lateral half-width of the strip in world units.                *
 * STRIP_LEN:  depth of the strip box in world units.                         *
 * LANDING_APPROACH_DIST: strip z-distance when cruise/descent begins.       */
#define STRIP_HALF             ((int16_t)(FP_ONE / 2))   /* ±0.5 units — between grid lines */
#define STRIP_LEN              ((int16_t)(2 * FP_ONE))    /* box depth             */
#define STRIP_Z_MARGIN         (GRID_ZSTEP / 4)          /* inset box off grid horizontal lines */
#define LANDING_APPROACH_DIST  (8 * FP_ONE)              /* strip starts at grid far plane */
#define LANDING_STRIP_MIN      (3 * FP_ONE)              /* closest strip gets during cruise;
                                                           * leaves ~48 frames of approach during descent */
#define STRIP_X_MIN  ((int16_t)(FP_ONE + FP_ONE/2))     /* 1.5 units min lateral offset  */
#define STRIP_X_MAX  ((int16_t)(3 * FP_ONE))             /* 3.0 units max lateral offset  */

/* ── Alien enemies ───────────────────────────────────────────────────────── */
#define ALIEN_COUNT    4
#define ALIEN_AIM_TOL  ((int16_t)(FP_ONE / 2))  /* ±0.5 unit lateral hit tolerance  */
#define ALIEN_SCALE_W  (6 * FP_ONE)              /* screen half-width  at z=FP_ONE   */
#define ALIEN_SCALE_H  (8 * FP_ONE)              /* screen half-height at z=FP_ONE   */
#define ALIEN_MIN_PIX  3                          /* min half-size (far away)         */
#define ALIEN_Z_GAP    ((int16_t)(FP_ONE + FP_ONE / 2))  /* z spacing between aliens */
/* Minimum z for draw_alien divs16 safety: max|(wx-cam_x)|=9*FP_ONE, *FOCAL=1185792,
 * /32767 = 37; use 40 for margin. sy is fixed so no sy overflow concern. */
#define ALIEN_ZMIN     ((int16_t)40)

/* ── Missiles ────────────────────────────────────────────────────────────── */
#define MISSILE_COUNT    3    /* max simultaneous missiles in flight              */
#define MISSILE_SPEED    (CAM_ZSPEED * 3)   /* forward advance per frame        */
#define MISSILE_GUN_SEP  20   /* screen-space half-separation of the two cannons */
/* Segment half-length in z-units.  GRID_ZFAR=8192=2^13; the segment slides
 * along the cannon→horizon line as z increases.  400 ≈ 2 frames of travel. */
#define MISSILE_SEG_HZ   400

/* ── Perspective ground grid (world units; FP_ONE = 1.0 unit) ────────────── */
/*
 * Projection:  screen_x = 160 + wx*FOCAL/z_rel
 *              screen_y = 100 + cam_y*FOCAL/z_rel   (horizon at y=100)
 *
 * Horizontal lines tile by wrapping their z_rel with z_phase each frame.
 * Vertical lines span GRID_ZNEAR..GRID_ZFAR unchanged (converge to VP).
 */
#define GRID_ZNEAR   ((int16_t)(FP_ONE / 2))   /* 0.5 units — near clip         */
#define GRID_ZFAR    ((int16_t)(8 * FP_ONE))   /* 8.0 units — far edge          */
#define GRID_ZSTEP   ((int16_t)(FP_ONE))        /* 1.0 unit between Z rows       */
#define GRID_ZDIVS   7                           /* 8 horizontal lines            */
#define GRID_XHALF   ((int16_t)(5 * FP_ONE))   /* ±5 units wide                 */
#define GRID_XDIVS   10                          /* 11 vertical lines             */
#define GRID_XSTEP   ((int16_t)(FP_ONE))        /* 1.0 unit column spacing       */
#define GRID_NUM_LINES ((GRID_XDIVS + 1) + (GRID_ZDIVS + 1))   /* 11+8 = 19    */


#define SCREEN_WIDTH_HALF  (SCREEN_WIDTH  / 2)
#define SCREEN_HEIGHT_HALF (SCREEN_HEIGHT / 2)

/* Orthographic logo projection (not perspective — grid uses divs16() for that).
 * FP_SHIFT-5 = 5: screen_x = 160 + p.x/32 → 32 px per FP_ONE unit laterally.
 * Z offset uses FP_SHIFT-4 = 6 → 16 px per FP_ONE unit, giving a shallower
 * isometric feel on the depth axis than the lateral axes. */
static inline Point2D project(Point3DInt p) {
    Point2D out;
    out.x = SCREEN_WIDTH_HALF  + (p.x >> (FP_SHIFT - 5));
    out.y = SCREEN_HEIGHT_HALF + (p.y >> (FP_SHIFT - 5)) - (p.z >> (FP_SHIFT - 4));
    return out;
}

/* 3-D world-space line pair (grid stored as world coords, projected per frame) */
typedef struct { Point3DInt p0, p1; } Line3D;


static Line3D gGridWorld[GRID_NUM_LINES];

/* Store the ground grid as 3-D world coordinates (projected per frame) */
static void build_grid(void) {
    int i = 0, xi, zi;
    int16_t x, z;

    /* Horizontal lines: constant Z rows (z_phase applied at render time) */
    for (zi = 0; zi <= GRID_ZDIVS; zi++) {
        z = (int16_t)(GRID_ZNEAR + zi * GRID_ZSTEP);
        gGridWorld[i].p0.x = (int16_t)(-GRID_XHALF); gGridWorld[i].p0.y = 0; gGridWorld[i].p0.z = z;
        gGridWorld[i].p1.x =           GRID_XHALF;   gGridWorld[i].p1.y = 0; gGridWorld[i].p1.z = z;
        i++;
    }

    /* Vertical lines: span full Z range, converge to vanishing point */
    for (xi = 0; xi <= GRID_XDIVS; xi++) {
        x = (int16_t)(-GRID_XHALF + xi * GRID_XSTEP);
        gGridWorld[i].p0.x = x; gGridWorld[i].p0.y = 0; gGridWorld[i].p0.z = GRID_ZNEAR;
        gGridWorld[i].p1.x = x; gGridWorld[i].p1.y = 0; gGridWorld[i].p1.z = GRID_ZFAR;
        i++;
    }
}

static Point2D gProjVerts[NUM_VERTICES];
/* MAX_LINES: logo (306) dominates grid (19); strip+tally+fuel+arrows = 6+8+1+2 */
#define MAX_LINES (NUM_EDGES + 6 + 8 + 1 + 2)
static Line     gLines[MAX_LINES + 1];   /* +1 for null sentinel */
static uint16_t gNLines;

static inline void append_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    assert(gNLines < MAX_LINES);
    gLines[gNLines].p0.x = x0; gLines[gNLines].p0.y = y0;
    gLines[gNLines].p1.x = x1; gLines[gNLines].p1.y = y1;
    gNLines++;
}

/* Alien/missile lines (plane 1) — accumulated each frame, drawn in one batch. */
#define MAX_ALIEN_LINES (ALIEN_COUNT * 3 + MISSILE_COUNT * 2)  /* 18 */
static Line     gAlienLines[MAX_ALIEN_LINES + 1]; /* +1 for null sentinel */
static uint16_t gNAlienLines;

static inline void append_alien_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    assert(gNAlienLines < MAX_ALIEN_LINES);
    gAlienLines[gNAlienLines].p0.x = x0; gAlienLines[gNAlienLines].p0.y = y0;
    gAlienLines[gNAlienLines].p1.x = x1; gAlienLines[gNAlienLines].p1.y = y1;
    gNAlienLines++;
}


/*
 * ALL endpoints must be clamped to [1,319] x [1,199] before calling
 * backend_draw_lines: the Atari SegmentedLine assembly takes unsigned
 * short coordinates — passing negative values causes an address error.
 */
#define SC_X0   1
#define SC_X1 319
#define SC_Y0   1
#define SC_Y1 199

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/*
 * divs16 — force the m68k hardware divs.w instruction (32÷16→16).
 * CALLER MUST ENSURE the quotient fits in int16_t — divs traps on overflow.
 * The assert catches violations in debug builds on all targets.
 */
static inline int16_t divs16(int32_t num, int16_t den) {
    assert(num / den >= -32768 && num / den <= 32767);
#ifdef __m68k__
    __asm__("divs.w %1,%0" : "+d"(num) : "dmi"(den));
    return (int16_t)num;   /* quotient in low word after divs */
#else
    return (int16_t)(num / den);
#endif
}

/* Minimum z_rel that keeps 655360/z_rel (= GRID_XHALF*FOCAL) within int16_t.
 * 655360/20 = 32768 overflows; 655360/21 = 31207. Visually identical since the
 * line is already clamped to screen edges at any z_rel this small. */
#define HLINE_ZMIN ((int16_t)21)

static void render_grid(bool enabled, int16_t cam_y, int16_t z_phase, int16_t cam_x) {
    if (!enabled) return;
    unsigned int i;
    int16_t z_wrap = (int16_t)((GRID_ZDIVS + 1) * GRID_ZSTEP);

    for (i = 0; i < (unsigned int)(GRID_ZDIVS + 1); i++) {
        int16_t z_rel = (int16_t)(gGridWorld[i].p0.z - z_phase);
        if (z_rel <= 0) z_rel += z_wrap;
        if (z_rel < HLINE_ZMIN) z_rel = HLINE_ZMIN;
        int16_t x_off    = divs16((int32_t)GRID_XHALF * FOCAL, z_rel);
        int16_t vp_shift = divs16((int32_t)cam_x * FOCAL, z_rel);
        int16_t y        = (int16_t)(SCREEN_HEIGHT_HALF + divs16((int32_t)cam_y * FOCAL, z_rel));
        append_line(CLAMP((int16_t)(SCREEN_WIDTH_HALF - vp_shift - x_off), SC_X0, SC_X1),
                    CLAMP(y, SC_Y0, SC_Y1),
                    CLAMP((int16_t)(SCREEN_WIDTH_HALF - vp_shift + x_off), SC_X0, SC_X1),
                    CLAMP(y, SC_Y0, SC_Y1));
    }

    {
        int16_t cam_x_near = (int16_t)((int32_t)cam_x * FOCAL / GRID_ZNEAR);
        int16_t cam_x_far  = (int16_t)((int32_t)cam_x * FOCAL / GRID_ZFAR);
        int16_t cam_y_near = (int16_t)(SCREEN_HEIGHT_HALF + (int32_t)cam_y * FOCAL / GRID_ZNEAR);
        int16_t cam_y_far  = (int16_t)(SCREEN_HEIGHT_HALF + (int32_t)cam_y * FOCAL / GRID_ZFAR);
        for (i = GRID_ZDIVS + 1; i < (unsigned int)GRID_NUM_LINES; i++) {
            int16_t wx = gGridWorld[i].p0.x;
            int32_t x0 = SCREEN_WIDTH_HALF - cam_x_near + (int32_t)wx * FOCAL / GRID_ZNEAR;
            int32_t y0 = cam_y_near;
            int32_t x1 = SCREEN_WIDTH_HALF - cam_x_far  + (int32_t)wx * FOCAL / GRID_ZFAR;
            int32_t y1 = cam_y_far;
            if (x0 < SC_X0 || x0 > SC_X1) {
                int16_t edge = (x0 < SC_X0) ? SC_X0 : SC_X1;
                int16_t dx   = (int16_t)(x1 - x0);
                int16_t dy   = (int16_t)(y1 - y0);
                y0 += divs16((int32_t)dy * (edge - (int16_t)x0), dx);
                x0  = edge;
            }
            if (y0 > SC_Y1) {
                int16_t dy = (int16_t)(y1 - y0);
                int16_t dx = (int16_t)(x1 - x0);
                if (dy != 0)
                    x0 += divs16((int32_t)dx * (SC_Y1 - (int16_t)y0), dy);
                y0 = SC_Y1;
            }
            append_line((int16_t)CLAMP(x0, SC_X0, SC_X1), (int16_t)y0,
                        (int16_t)x1, (int16_t)CLAMP(y1, SC_Y0, SC_Y1));
        }
    }
}

static void render_logo(bool enabled, int16_t angleY, int16_t angleX) {
    if (!enabled) return;
    unsigned int i;
    /* Hoist trig lookups outside vertex loop — identical for all 309 vertices */
    int16_t cosY = fastCos(angleY), sinY = fastSin(angleY);
    int16_t cosX = fastCos(angleX), sinX = fastSin(angleX);
    for (i = 0; i < NUM_VERTICES; ++i) {
        Point3DInt t = rotate(i, cosY, sinY, cosX, sinX);
        gProjVerts[i] = project(t);
    }
    for (i = 0; i < NUM_EDGES; ++i) {
        Point2D p1 = gProjVerts[vquest_edges[i][0]];
        Point2D p2 = gProjVerts[vquest_edges[i][1]];
        p1.x = CLAMP(p1.x, SC_X0, SC_X1); p1.y = CLAMP(p1.y, SC_Y0, SC_Y1);
        p2.x = CLAMP(p2.x, SC_X0, SC_X1); p2.y = CLAMP(p2.y, SC_Y0, SC_Y1);
        append_line(p1.x, p1.y, p2.x, p2.y);
    }
}

/*
 * Project and draw a ground-level rectangle: near crossbar at z_near,
 * far crossbar at z_far, lateral half-width x_half.
 * Near endpoints are clipped to y=SC_Y1 if they project below the screen.
 */
static void draw_ground_strip(int16_t x_half, int16_t z_near, int16_t z_far,
                               int16_t cam_x, int16_t cam_y)
{
    /* Inset box off grid horizontal lines (which sit at GRID_ZNEAR + k*GRID_ZSTEP) */
    z_near = (int16_t)(z_near + STRIP_Z_MARGIN);
    z_far  = (int16_t)(z_far  - STRIP_Z_MARGIN);
    if (z_far < HLINE_ZMIN || z_near >= z_far) return;  /* degenerate after inset */
    /* Far endpoint projections */
    int16_t sxl_f = (int16_t)(SCREEN_WIDTH_HALF + divs16((-x_half - (int32_t)cam_x) * FOCAL, z_far));
    int16_t sxr_f = (int16_t)(SCREEN_WIDTH_HALF + divs16(( x_half - (int32_t)cam_x) * FOCAL, z_far));
    int16_t sy_f  = (int16_t)(SCREEN_HEIGHT_HALF + divs16((int32_t)cam_y * FOCAL, z_far));
    /* Near endpoint projections (int32 to allow off-screen values before clip) */
    int32_t sxl_n = SCREEN_WIDTH_HALF + divs16((-x_half - (int32_t)cam_x) * FOCAL, z_near);
    int32_t sxr_n = SCREEN_WIDTH_HALF + divs16(( x_half - (int32_t)cam_x) * FOCAL, z_near);
    int32_t sy_n  = SCREEN_HEIGHT_HALF + divs16((int32_t)cam_y * FOCAL, z_near);
    /* Slide near endpoints up to y=SC_Y1 if below screen */
    if (sy_n > SC_Y1) {
        int16_t dy = (int16_t)((int32_t)sy_f - sy_n);
        if (dy != 0) {
            int16_t dt = (int16_t)(SC_Y1 - sy_n);
            sxl_n += divs16((int32_t)(sxl_f - sxl_n) * dt, dy);
            sxr_n += divs16((int32_t)(sxr_f - sxr_n) * dt, dy);
        }
        sy_n = SC_Y1;
    }
    int16_t sy_nc  = (int16_t)sy_n;
    int16_t sy_fc  = CLAMP(sy_f,           SC_Y0, SC_Y1);
    int16_t sxl_nc = CLAMP((int16_t)sxl_n, SC_X0, SC_X1);
    int16_t sxr_nc = CLAMP((int16_t)sxr_n, SC_X0, SC_X1);
    int16_t sxl_fc = CLAMP(sxl_f,          SC_X0, SC_X1);
    int16_t sxr_fc = CLAMP(sxr_f,          SC_X0, SC_X1);
    append_line(sxl_nc, sy_nc, sxr_nc, sy_nc);  /* Near crossbar      */
    append_line(sxl_fc, sy_fc, sxr_fc, sy_fc);  /* Far crossbar       */
    append_line(sxl_nc, sy_nc, sxl_fc, sy_fc);  /* Left guide         */
    append_line(sxr_nc, sy_nc, sxr_fc, sy_fc);  /* Right guide        */
    append_line(sxl_nc, sy_nc, sxr_fc, sy_fc);  /* Diagonal: NL → FR  */
    append_line(sxr_nc, sy_nc, sxl_fc, sy_fc);  /* Diagonal: NR → FL  */
}

/* draw_alien — project and draw a triangle on the alien plane.
 * Alien flies at eye level → sy = SCREEN_HEIGHT_HALF (no division for y).
 * ALIEN_ZMIN guards the sx divs16 from overflow; all coords clamped before
 * backend_alien_line because SegmentedLine has no clip. */
static void draw_alien(int16_t wx, int16_t z, int16_t cam_x)
{
    int16_t sx, hw, hh, tx, ty, lx, ly, rx, ry;
    if (z < ALIEN_ZMIN || z > GRID_ZFAR) return;
    sx = (int16_t)(SCREEN_WIDTH_HALF  + divs16((int32_t)(wx - cam_x) * FOCAL, z));
    hw = (int16_t)(ALIEN_SCALE_W / z); if (hw < ALIEN_MIN_PIX) hw = ALIEN_MIN_PIX;
    hh = (int16_t)(ALIEN_SCALE_H / z); if (hh < ALIEN_MIN_PIX) hh = ALIEN_MIN_PIX;
    if (sx - hw > SC_X1 || sx + hw < SC_X0) return;  /* fully off-screen */
    tx = CLAMP(sx,                     SC_X0, SC_X1);
    ty = CLAMP((int16_t)(SCREEN_HEIGHT_HALF - hh), SC_Y0, SC_Y1);
    lx = CLAMP((int16_t)(sx - hw),     SC_X0, SC_X1);
    ly = CLAMP((int16_t)(SCREEN_HEIGHT_HALF + hh), SC_Y0, SC_Y1);
    rx = CLAMP((int16_t)(sx + hw),     SC_X0, SC_X1);
    ry = ly;
    append_alien_line(tx, ty, lx, ly);
    append_alien_line(lx, ly, rx, ry);
    append_alien_line(rx, ry, tx, ty);
}

/* draw_missile — two perspective-correct segments sliding along their cannon→horizon
 * trajectories.  Both endpoints use 1/t perspective so that when missile_z ≈ alien_z
 * the segment appears at (sx, SCREEN_HEIGHT_HALF) — the alien's screen position.
 *
 * Formula: pos = horizon_pos + (gun_pos - horizon_pos) * HLINE_ZMIN / t
 *   • t = HLINE_ZMIN → pos = gun_pos   (missile just fired, segment at cannon)
 *   • t → ∞          → pos = horizon_pos (missile at vanishing point)
 *   • t ≈ alien_z    → pos ≈ alien screen pos (hit looks correct)
 *
 * Left  cannon: screen x = 160-SEP, y = SC_Y1
 * Right cannon: screen x = 160+SEP, y = SC_Y1
 * Horizon target: screen x = sx (perspective projection of missile_x), y = SCREEN_HEIGHT_HALF */
static void draw_missile(int16_t wx, int16_t z, int16_t cam_x)
{
    int32_t t0, t1;
    int16_t sx, lx, rx, y0, y1, lx0, lx1, rx0, rx1;
    if (z < HLINE_ZMIN || z > GRID_ZFAR) return;
    sx = CLAMP((int16_t)(SCREEN_WIDTH_HALF + divs16((int32_t)(wx - cam_x) * FOCAL, z)),
               SC_X0, SC_X1);
    lx = (int16_t)(SCREEN_WIDTH_HALF - MISSILE_GUN_SEP);   /* left  cannon x */
    rx = (int16_t)(SCREEN_WIDTH_HALF + MISSILE_GUN_SEP);   /* right cannon x */

    t0 = (int32_t)z - MISSILE_SEG_HZ; if (t0 < 0) t0 = 0;
    t1 = (int32_t)z + MISSILE_SEG_HZ; if (t1 > GRID_ZFAR) t1 = GRID_ZFAR;

    /* Perspective position at parameter t: horizon + (gun - horizon) * HLINE_ZMIN / t.
     * t0 == 0 means segment tail is still at the cannon — use gun coordinates directly. */
    if (t0 > 0) {
        int16_t t0s = (int16_t)t0;
        y0  = (int16_t)(SCREEN_HEIGHT_HALF + divs16((int32_t)(SC_Y1 - SCREEN_HEIGHT_HALF) * HLINE_ZMIN, t0s));
        lx0 = (int16_t)(sx               - divs16((int32_t)(sx - lx)                     * HLINE_ZMIN, t0s));
        rx0 = (int16_t)(sx               + divs16((int32_t)(rx - sx)                     * HLINE_ZMIN, t0s));
    } else {
        y0 = SC_Y1; lx0 = lx; rx0 = rx;
    }
    {
        int16_t t1s = (int16_t)t1;
        y1  = (int16_t)(SCREEN_HEIGHT_HALF + divs16((int32_t)(SC_Y1 - SCREEN_HEIGHT_HALF) * HLINE_ZMIN, t1s));
        lx1 = (int16_t)(sx               - divs16((int32_t)(sx - lx)                     * HLINE_ZMIN, t1s));
        rx1 = (int16_t)(sx               + divs16((int32_t)(rx - sx)                     * HLINE_ZMIN, t1s));
    }

    append_alien_line(CLAMP(lx0, SC_X0, SC_X1), CLAMP(y0, SC_Y0, SC_Y1),
                      CLAMP(lx1, SC_X0, SC_X1), CLAMP(y1, SC_Y0, SC_Y1));
    append_alien_line(CLAMP(rx0, SC_X0, SC_X1), CLAMP(y0, SC_Y0, SC_Y1),
                      CLAMP(rx1, SC_X0, SC_X1), CLAMP(y1, SC_Y0, SC_Y1));
}

typedef enum { STATE_TAKEOFF, STATE_CRUISE, STATE_LANDING, STATE_CRASH, STATE_SUCCESS } GameState;

/* Per-state render flags — indexed by GameState value.
 * Adding a new visual element: add a field here + one column in the table.
 * `flash` is transient (set per-frame in STATE_CRASH) so it stays in the switch. */
typedef struct {
    bool grid, logo, arrows, takeoff_strip, landing_strip, aliens;
} RenderFlags;

static const RenderFlags kStateFlags[] = {
/*                        grid   logo   arrows takeof  land   aliens */
/* STATE_TAKEOFF */     { true,  false, true,  true,   false, false },
/* STATE_CRUISE  */     { true,  false, true,  false,  true,  true  },
/* STATE_LANDING */     { true,  false, true,  false,  true,  true  },
/* STATE_CRASH   */     { true,  false, false, false,  false, false },
/* STATE_SUCCESS */     { false, true,  false, false,  false, false },
};

static inline bool lateral_crash(int16_t cam_x) {
    return cam_x > CRASH_CAM_X || cam_x < -CRASH_CAM_X;
}

static inline bool lateral_crash_landing(int16_t cam_x, int16_t strip_x) {
    int16_t rel = (int16_t)(cam_x - strip_x);
    return rel > CRASH_CAM_X || rel < -CRASH_CAM_X;
}

/* next_strip_x — deterministic but varying strip position per round.
 * Uses a simple LCG to produce a magnitude in [STRIP_X_MIN, STRIP_X_MAX]
 * and picks sign from the high bit, ensuring the strip is never centred. */
static int16_t next_strip_x(int16_t round) {
    uint16_t r   = (uint16_t)((uint16_t)round * 2053u + 13849u);
    int16_t  mag = (int16_t)(STRIP_X_MIN + (r >> 13) % (STRIP_X_MAX - STRIP_X_MIN + 1));
    return (r & 0x8000) ? mag : (int16_t)(-mag);
}

/* apply_lateral — update vel_x and cam_x from Left/Right keys.
 * use_fuel=true: deduct FUEL_STEER_COST and guard against underflow.
 * use_fuel=false (CRUISE): no fuel check, no deduction.
 * steer/vel_x_max are compile-time constants at each call site;
 * GCC constant-folds all branches when inlined. */
static inline void apply_lateral(bool use_fuel, int16_t steer, int16_t vel_x_max,
                                  uint8_t keys, int16_t *vel_x, int16_t *cam_x, uint8_t *fuel)
{
    if (use_fuel) {
        if ((keys & KEY_LEFT)  && *fuel >= FUEL_STEER_COST) { *vel_x = (int16_t)(*vel_x - steer); *fuel -= FUEL_STEER_COST; }
        if ((keys & KEY_RIGHT) && *fuel >= FUEL_STEER_COST) { *vel_x = (int16_t)(*vel_x + steer); *fuel -= FUEL_STEER_COST; }
    } else {
        if (keys & KEY_LEFT)  *vel_x = (int16_t)(*vel_x - steer);
        if (keys & KEY_RIGHT) *vel_x = (int16_t)(*vel_x + steer);
    }
    *vel_x = (int16_t)(*vel_x - (*vel_x >> DRAG_SHIFT));
    if (*vel_x >  vel_x_max) *vel_x =  vel_x_max;
    if (*vel_x < -vel_x_max) *vel_x = -vel_x_max;
    *cam_x = (int16_t)(*cam_x + *vel_x);
}

/* apply_vertical — update vel_y and cam_y from Up/Down keys.
 * thrust/descent_thrust/vel_y_max/clamp_ground are compile-time constants;
 * GCC folds all dependent branches away when inlined.
 * descent_thrust=0 disables KEY_DOWN (used for takeoff). */
static inline void apply_vertical(int16_t thrust, int16_t descent_thrust,
                                   int16_t vel_y_max, bool clamp_ground, uint8_t keys,
                                   int16_t *vel_y, int16_t *cam_y, uint8_t *fuel)
{
    if ((keys & KEY_UP) && *fuel >= FUEL_THRUST_COST) {
        *vel_y = (int16_t)(*vel_y + thrust - GRAVITY);
        *fuel -= FUEL_THRUST_COST;
    } else if (descent_thrust && (keys & KEY_DOWN)) {
        *vel_y = (int16_t)(*vel_y - GRAVITY - descent_thrust);
    } else {
        *vel_y = (int16_t)(*vel_y - GRAVITY);
    }
    *vel_y = (int16_t)(*vel_y - (*vel_y >> DRAG_SHIFT));
    if (*vel_y > vel_y_max) *vel_y = vel_y_max;
    if (*vel_y < VEL_Y_MIN) *vel_y = VEL_Y_MIN;
    *cam_y = (int16_t)(*cam_y + *vel_y);
    if (clamp_ground && *cam_y < CAM_Y_INIT) { *cam_y = CAM_Y_INIT; *vel_y = 0; }
}

#define SUCCESS_FLASH_FRAMES 60  /* ~1 s of blinking runway on good landing */

/* ── Per-element render helpers (each owns its enabled guard) ────────────── */

static inline void render_takeoff_strip(bool enabled, int16_t takeoff_timer,
                                         int16_t cam_x, int16_t cam_y) {
    int32_t z_end_val;
    if (!enabled) return;
    z_end_val = (int32_t)takeoff_timer * CAM_ZSPEED;
    if (z_end_val >= HLINE_ZMIN && z_end_val <= GRID_ZFAR)
        draw_ground_strip(STRIP_HALF, GRID_ZNEAR, (int16_t)z_end_val, cam_x, cam_y);
}

static inline void render_landing_strip(bool enabled, int16_t strip_dist, int16_t strip_x,
                                         int16_t cam_x, int16_t cam_y) {
    int16_t sz, cam_x_rel;
    if (!enabled || strip_dist <= 0 || strip_dist > GRID_ZFAR) return;
    sz        = (int16_t)(strip_dist < HLINE_ZMIN ? HLINE_ZMIN : strip_dist);
    cam_x_rel = (int16_t)(cam_x - strip_x);
    if (cam_x_rel >  3 * FP_ONE) cam_x_rel =  (int16_t)(3 * FP_ONE);
    if (cam_x_rel < -3 * FP_ONE) cam_x_rel = -(int16_t)(3 * FP_ONE);
    draw_ground_strip(STRIP_HALF, sz, (int16_t)(sz + STRIP_LEN), cam_x_rel, cam_y);
}

static inline void render_fuel_bar(uint8_t fuel) {
    if (fuel > 0)
        append_line((int16_t)(FUEL_BAR_X1 - fuel), FUEL_Y, FUEL_BAR_X1, FUEL_Y);
}

static inline void render_arrows(bool enabled, int16_t cam_x, int16_t strip_x) {
    int16_t arrow_offset;
    if (!enabled) return;
    arrow_offset = (int16_t)(cam_x - strip_x);
    if (arrow_offset > FP_ONE / 2) {
        append_line(ARROW_SHAFT_X_LEFT,  ARROW_Y_CENTER - ARROW_Y_HALF,
                    ARROW_TIP_X_LEFT,    ARROW_Y_CENTER);
        append_line(ARROW_TIP_X_LEFT,    ARROW_Y_CENTER,
                    ARROW_SHAFT_X_LEFT,  ARROW_Y_CENTER + ARROW_Y_HALF);
    } else if (arrow_offset < -(FP_ONE / 2)) {
        append_line(ARROW_SHAFT_X_RIGHT, ARROW_Y_CENTER - ARROW_Y_HALF,
                    ARROW_TIP_X_RIGHT,   ARROW_Y_CENTER);
        append_line(ARROW_TIP_X_RIGHT,   ARROW_Y_CENTER,
                    ARROW_SHAFT_X_RIGHT, ARROW_Y_CENTER + ARROW_Y_HALF);
    }
}

static inline void draw_world_plane(const RenderFlags *rf,
    int16_t cam_y, int16_t z_phase, int16_t cam_x,
    int16_t angleY, int16_t angleX,
    int16_t takeoff_timer, int16_t strip_dist, int16_t strip_x,
    uint8_t fuel)
{
    gNLines = 0;
    render_grid(rf->grid, cam_y, z_phase, cam_x);
    render_logo(rf->logo, angleY, angleX);
    render_takeoff_strip(rf->takeoff_strip, takeoff_timer, cam_x, cam_y);
    render_landing_strip(rf->landing_strip, strip_dist, strip_x, cam_x, cam_y);
    render_fuel_bar(fuel);
    render_arrows(rf->arrows, cam_x, strip_x);
    memset(&gLines[gNLines], 0, sizeof(Line));
    backend_draw_lines(gLines, gNLines);
}

static inline void draw_alien_plane(bool enabled,
    int16_t alien_x[], int16_t alien_z[], bool alien_alive[],
    int16_t missile_x[], int16_t missile_z[], bool missile_alive[],
    int16_t cam_x)
{
    int i;
    gNAlienLines = 0;
    if (enabled) {
        for (i = 0; i < ALIEN_COUNT; i++)
            if (alien_alive[i]) draw_alien(alien_x[i], alien_z[i], cam_x);
        for (i = 0; i < MISSILE_COUNT; i++)
            if (missile_alive[i]) draw_missile(missile_x[i], missile_z[i], cam_x);
    }
    memset(&gAlienLines[gNAlienLines], 0, sizeof(Line));
    backend_draw_alien_lines(gAlienLines, gNAlienLines);
}

/* ── Alien / missile helpers ─────────────────────────────────────────────── */

/* Spawn ALIEN_COUNT aliens spread along the approach corridor, then clear
 * all missile slots.  Called once when transitioning TAKEOFF → CRUISE. */
static void spawn_aliens(int16_t round,
    int16_t alien_x[], int16_t alien_z[], bool alien_alive[],
    bool missile_alive[])
{
    int i;
    for (i = 0; i < ALIEN_COUNT; i++) {
        uint16_t r;
        int16_t  mag;
        alien_z[i]    = (int16_t)(LANDING_APPROACH_DIST - (i + 1) * ALIEN_Z_GAP);
        r             = (uint16_t)((uint16_t)(round * 37 + i * 13) * 2053u + 13849u);
        mag           = (int16_t)(FP_ONE + (r >> 13) % (2 * FP_ONE + 1));
        alien_x[i]    = (r & 0x8000) ? mag : (int16_t)(-mag);
        alien_alive[i] = true;
    }
    for (i = 0; i < MISSILE_COUNT; i++) missile_alive[i] = false;
}

/* Scroll all live aliens toward the camera each frame. */
static void update_aliens(int16_t alien_z[], bool alien_alive[])
{
    int i;
    for (i = 0; i < ALIEN_COUNT; i++) {
        if (alien_alive[i])
            alien_z[i] = (int16_t)(alien_z[i] - CAM_ZSPEED);
    }
}

/* Fire: spawn one missile in the first free slot (one shot per key press). */
static void try_fire_missile(uint8_t keys, int16_t cam_x,
    int16_t missile_x[], int16_t missile_z[], bool missile_alive[])
{
    int i;
    if (!(keys & KEY_FIRE)) return;
    for (i = 0; i < MISSILE_COUNT; i++) {
        if (!missile_alive[i]) {
            missile_x[i]    = cam_x;
            missile_z[i]    = HLINE_ZMIN;
            missile_alive[i] = true;
            break;
        }
    }
}

/* Advance all live missiles and test collision against all live aliens. */
static void update_missiles(
    int16_t missile_x[], int16_t missile_z[], bool missile_alive[],
    int16_t alien_x[],  int16_t alien_z[],  bool alien_alive[])
{
    int mi;
    for (mi = 0; mi < MISSILE_COUNT; mi++) {
        int ai;
        if (!missile_alive[mi]) continue;
        missile_z[mi] = (int16_t)(missile_z[mi] + MISSILE_SPEED);
        if (missile_z[mi] > GRID_ZFAR) { missile_alive[mi] = false; continue; }
        for (ai = 0; ai < ALIEN_COUNT; ai++) {
            int16_t rel;
            if (!alien_alive[ai]) continue;
            if (alien_z[ai] <= 0) continue;          /* already passed player */
            if (missile_z[mi] < alien_z[ai]) continue;
            rel = (int16_t)(missile_x[mi] - alien_x[ai]);
            if (rel > -ALIEN_AIM_TOL && rel < ALIEN_AIM_TOL) {
                alien_alive[ai]   = false;
                missile_alive[mi] = false;
            }
        }
    }
}

/* ── State update functions ───────────────────────────────────────────────── */

static GameState state_takeoff(
    int16_t *takeoff_timer, int16_t *cam_y, int16_t *cam_x,
    int16_t *vel_y, int16_t *vel_x, uint8_t *fuel,
    int16_t *strip_dist, int16_t *crash_timer, int16_t round,
    int16_t alien_x[], int16_t alien_z[], bool alien_alive[],
    bool missile_alive[], uint8_t keys)
{
    if (--(*takeoff_timer) <= 0 && *cam_y < CRUISE_ALT) {
        *crash_timer = CRASH_FLASH_FRAMES; return STATE_CRASH;
    }
    apply_vertical(TAKEOFF_THRUST, 0, VEL_Y_MAX, true, keys, vel_y, cam_y, fuel);
    apply_lateral(true, STEER, VEL_X_MAX, keys, vel_x, cam_x, fuel);
    if (lateral_crash(*cam_x)) {
        *crash_timer = CRASH_FLASH_FRAMES; return STATE_CRASH;
    }
    if (*cam_y >= CRUISE_ALT) {
        *cam_y = CRUISE_ALT; *vel_y = 0;
        /* cam_x/vel_x intentionally NOT reset — carry into landing */
        *strip_dist = LANDING_APPROACH_DIST;
        spawn_aliens(round, alien_x, alien_z, alien_alive, missile_alive);
        return STATE_CRUISE;
    }
    return STATE_TAKEOFF;
}

static GameState state_cruise(
    int16_t *strip_dist, int16_t *cam_x, int16_t *vel_x, uint8_t *fuel,
    int16_t alien_z[], bool alien_alive[],
    int16_t missile_x[], int16_t missile_z[], bool missile_alive[],
    uint8_t keys)
{
    apply_lateral(false, CRUISE_STEER, CRUISE_VEL_X_MAX, keys, vel_x, cam_x, fuel);
    *strip_dist = (int16_t)(*strip_dist - CAM_ZSPEED);
    if (*strip_dist <= LANDING_STRIP_MIN) {
        *strip_dist = LANDING_STRIP_MIN;
        return STATE_LANDING;
    }
    update_aliens(alien_z, alien_alive);
    try_fire_missile(keys, *cam_x, missile_x, missile_z, missile_alive);
    return STATE_CRUISE;
}

static GameState state_landing(
    int16_t *cam_y, int16_t *cam_x, int16_t *vel_y, int16_t *vel_x,
    uint8_t *fuel, int16_t *strip_dist, int16_t *strip_x,
    int16_t *round, int16_t *takeoff_limit, int16_t *crash_timer,
    int16_t alien_z[], bool alien_alive[],
    uint8_t keys)
{
    update_aliens(alien_z, alien_alive);   /* keep aliens scrolling with the world */
    apply_vertical(TAKEOFF_THRUST, DESCENT_THRUST, VEL_Y_MAX, false, keys, vel_y, cam_y, fuel);
    apply_lateral(true, STEER, VEL_X_MAX, keys, vel_x, cam_x, fuel);
    if (lateral_crash_landing(*cam_x, *strip_x)) {
        *crash_timer = CRASH_FLASH_FRAMES; return STATE_CRASH;
    }
    if (*cam_y <= 0) {
        int16_t abs_vel_y = (int16_t)(*vel_y < 0 ? -*vel_y : *vel_y);
        int16_t rel_x     = (int16_t)(*cam_x - *strip_x);
        int16_t abs_rel_x = (int16_t)(rel_x < 0 ? -rel_x : rel_x);
        if (abs_vel_y < CRASH_VEL_Y && abs_rel_x < LAND_CAM_X) {
            (*round)++;
            *takeoff_limit = (int16_t)(*takeoff_limit - TAKEOFF_FRAMES_STEP);
            if (*takeoff_limit < TAKEOFF_FRAMES_MIN) *takeoff_limit = TAKEOFF_FRAMES_MIN;
            *fuel     = 0;
            *cam_y    = CAM_Y_INIT;
            *vel_y    = 0; *vel_x = 0;
            *strip_x  = next_strip_x(*round);
            *crash_timer = SUCCESS_FLASH_FRAMES;
            hud_draw(*round);
            return STATE_SUCCESS;
        }
        *crash_timer = CRASH_FLASH_FRAMES; return STATE_CRASH;
    }
    *strip_dist = (int16_t)(*strip_dist - CAM_ZSPEED);
    return STATE_LANDING;
}

static GameState state_crash(
    bool *flash, int16_t *crash_timer, int16_t *round,
    int16_t *takeoff_limit, int16_t *takeoff_timer, uint8_t *fuel,
    int16_t *strip_x, int16_t *cam_y, int16_t *vel_y,
    int16_t *cam_x, int16_t *vel_x)
{
    *flash = true;
    if (--(*crash_timer) <= 0) {
        *round         = 1;
        *takeoff_limit = TAKEOFF_FRAMES_BASE;
        *takeoff_timer = TAKEOFF_FRAMES_BASE;
        *fuel    = MAX_FUEL;
        *strip_x = next_strip_x(1);
        *cam_y = CAM_Y_INIT; *vel_y = 0; *cam_x = 0; *vel_x = 0;
        hud_draw(1);
        return STATE_TAKEOFF;
    }
    return STATE_CRASH;
}

static GameState state_success(
    int16_t *angleY, int16_t *angleX, int16_t *crash_timer,
    int16_t *cam_y, int16_t *cam_x, int16_t *vel_y, int16_t *vel_x,
    int16_t *takeoff_timer, int16_t takeoff_limit,
    int16_t angleYinc, int16_t angleXinc)
{
    *angleY = (int16_t)(*angleY + angleYinc);
    *angleX = (int16_t)(*angleX + angleXinc);
    if (--(*crash_timer) <= 0) {
        *cam_y = CAM_Y_INIT; *cam_x = 0; *vel_y = 0; *vel_x = 0;
        *takeoff_timer = takeoff_limit;
        return STATE_TAKEOFF;
    }
    return STATE_SUCCESS;
}

int main(int argc, char *argv[]) {
    int16_t angleY = 0, angleX = 0;
    int16_t angleYinc, angleXinc;
    int16_t cam_y   = CAM_Y_INIT;
    int16_t cam_x   = 0;
    int16_t vel_y   = 0;
    int16_t vel_x   = 0;
    int16_t z_phase = 0;
    int16_t frame         = 0;
    int16_t min_frame     = 0;
    int16_t max_frame     = -1;
    int16_t crash_timer   = 0;
    int16_t strip_dist    = 0;   /* z-distance to landing strip; counts down each frame */
    int16_t strip_x       = 0;   /* lateral world position of landing strip             */
    int16_t alien_x[ALIEN_COUNT]       = {0};
    int16_t alien_z[ALIEN_COUNT]       = {0};
    bool    alien_alive[ALIEN_COUNT]   = {0};
    int16_t missile_x[MISSILE_COUNT]   = {0};
    int16_t missile_z[MISSILE_COUNT]   = {0};
    bool    missile_alive[MISSILE_COUNT] = {0};
    uint8_t fuel          = MAX_FUEL;
    int16_t round         = 1;
    int16_t takeoff_limit = TAKEOFF_FRAMES_BASE;
    int16_t takeoff_timer = TAKEOFF_FRAMES_BASE;
    GameState state       = STATE_TAKEOFF;

    if (argc >= 2) min_frame = (int16_t)atoi(argv[1]);
    if (argc >= 3) max_frame = (int16_t)atoi(argv[2]);

    loadLUTs();
    backend_init();
    strip_x = next_strip_x(round);


    /* Intro: reveal title one letter at a time; any key skips. */
#define INTRO_LETTER_FRAMES 6
    {
        int8_t k = 0;
        bool   skipped = false;
        hud_begin();
        while (k < HUD_NCHARS && !skipped) {
            int8_t j;
            hud_draw_letter(k++);
            for (j = 0; j < INTRO_LETTER_FRAMES; j++) {
                backend_clear();
                backend_present(0, 0);
                if (backend_check_input()) { skipped = true; break; }
            }
        }
        if (skipped)
            hud_draw(1);
        else
            hud_draw_tally(1);
    }

    model_scale();
    build_grid();

    /* Convert rad/frame speeds to LUT-index increments */
    angleYinc = (int16_t)(0.08 * FP_ONE);
    angleXinc = (int16_t)(0.13 * FP_ONE);
    angleYinc = (int16_t)((int32_t)angleYinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));
    angleXinc = (int16_t)((int32_t)angleXinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));

    for (;;) {
        uint8_t keys = backend_get_keys();
        if (keys & KEY_QUIT) break;
        if (max_frame >= 0 && frame > max_frame) break;

        /* Grid always scrolls */
        z_phase = (int16_t)(z_phase + CAM_ZSPEED);
        if (z_phase >= GRID_ZSTEP) z_phase = (int16_t)(z_phase - GRID_ZSTEP);

        bool flash = false;
        const RenderFlags *rf = &kStateFlags[state];

        switch (state) {
        case STATE_TAKEOFF:
            state = state_takeoff(&takeoff_timer, &cam_y, &cam_x, &vel_y, &vel_x,
                                  &fuel, &strip_dist, &crash_timer, round,
                                  alien_x, alien_z, alien_alive, missile_alive, keys);
            break;
        case STATE_CRUISE:
            state = state_cruise(&strip_dist, &cam_x, &vel_x, &fuel,
                                 alien_z, alien_alive,
                                 missile_x, missile_z, missile_alive, keys);
            break;
        case STATE_LANDING:
            state = state_landing(&cam_y, &cam_x, &vel_y, &vel_x, &fuel,
                                  &strip_dist, &strip_x, &round, &takeoff_limit,
                                  &crash_timer, alien_z, alien_alive, keys);
            break;
        case STATE_CRASH:
            state = state_crash(&flash, &crash_timer, &round, &takeoff_limit,
                                &takeoff_timer, &fuel, &strip_x,
                                &cam_y, &vel_y, &cam_x, &vel_x);
            break;
        case STATE_SUCCESS:
            state = state_success(&angleY, &angleX, &crash_timer,
                                  &cam_y, &cam_x, &vel_y, &vel_x,
                                  &takeoff_timer, takeoff_limit,
                                  angleYinc, angleXinc);
            break;
        }

        /* Advance missiles every frame so they expire at the horizon regardless of state */
        update_missiles(missile_x, missile_z, missile_alive,
                        alien_x,   alien_z,   alien_alive);

        /* Safety clamp: allow wider roam during cruise; grid uses cam_x only for
         * horizontal lines (world x fixed) so ±6 is safe.  Strip rendering guards
         * cam_x_rel separately below (strip can be up to STRIP_X_MAX away). */
        if (cam_x >  6 * FP_ONE) cam_x =  (int16_t)(6 * FP_ONE);
        if (cam_x < -6 * FP_ONE) cam_x = -(int16_t)(6 * FP_ONE);

        backend_set_flash(flash);
        frame++;
        if (frame < min_frame) continue;
        backend_clear();
        draw_world_plane(rf, cam_y, z_phase, cam_x, angleY, angleX,
                         takeoff_timer, strip_dist, strip_x, fuel);
        draw_alien_plane(rf->aliens, alien_x, alien_z, alien_alive,
                         missile_x, missile_z, missile_alive, cam_x);
        backend_present(angleY, angleX);
    }

    backend_cleanup();
    return 0;
}
