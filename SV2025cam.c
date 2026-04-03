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

int16_t sinLUT[LUT_SIZE];
int16_t cosLUT[LUT_SIZE];
int16_t logLUT[LUT_SIZE];
int16_t expLUT[LUT_SIZE * 2];

void loadLUTs() {
    FILE *fp = fopen("luts", "rb");
    fread(sinLUT, sizeof(sinLUT), 1, fp);
    fread(cosLUT, sizeof(cosLUT), 1, fp);
    fread(logLUT, sizeof(logLUT), 1, fp);
    fread(expLUT, sizeof(expLUT), 1, fp);
    fclose(fp);
#ifndef __m68k__
    unsigned int i;
    for (i = 0; i < LUT_SIZE;     i++) sinLUT[i] = (int16_t)be16toh((uint16_t)sinLUT[i]);
    for (i = 0; i < LUT_SIZE;     i++) cosLUT[i] = (int16_t)be16toh((uint16_t)cosLUT[i]);
    for (i = 0; i < LUT_SIZE;     i++) logLUT[i] = (int16_t)be16toh((uint16_t)logLUT[i]);
    for (i = 0; i < LUT_SIZE * 2; i++) expLUT[i] = (int16_t)be16toh((uint16_t)expLUT[i]);
#endif
}


static inline int16_t fastSin(int16_t angle) {
    return sinLUT[angle & (LUT_SIZE-1)];
}

static inline int16_t fastCos(int16_t angle) {
    return cosLUT[angle & (LUT_SIZE-1)];
}

static inline int16_t fastLog(int16_t x) {
    int16_t index = (int16_t)(((int32_t)x * (LUT_SIZE/4)) >> FP_SHIFT);
    return logLUT[index & (LUT_SIZE-1)];
}

static inline int16_t fastExp(int16_t x) {
    int16_t index = (int16_t)(((int32_t)(x + (16 << FP_SHIFT)) * (LUT_SIZE*2)) / (32L << FP_SHIFT));
    return expLUT[index & ((LUT_SIZE*2)-1)];
}

static inline int16_t mulViaLogExp(int16_t a, int16_t b) {
    int16_t aa = (a < 0) ? -a : a;
    int16_t bb = (b < 0) ? -b : b;
    int16_t r  = fastExp((int16_t)(fastLog(aa) + fastLog(bb)));
    if ((a != aa) ^ (b != bb))
        r = -r;
    return r;
}

#include "SV2025.h"

#define NUM_VERTICES (sizeof(sv2025Vertices) / sizeof(sv2025Vertices[0]))
Point3DLong gVerticesLongScale[NUM_VERTICES];
#define NUM_EDGES (sizeof(sv2025Edges) / sizeof(sv2025Edges[0]))

void model_scale() {
    unsigned i;
    for (i = 0; i < NUM_VERTICES; i++) {
        gVerticesLongScale[i].x = (int32_t)((sv2025Vertices[i].x * LOGO_SCALE) * FP_ONE);
        gVerticesLongScale[i].y = (int32_t)((sv2025Vertices[i].y * LOGO_SCALE) * FP_ONE);
        gVerticesLongScale[i].z = (int32_t)((sv2025Vertices[i].z * LOGO_SCALE) * FP_ONE);
    }
}

static inline Point3DInt rotate(unsigned i, int16_t angleY, int16_t angleX) {
    Point3DInt p_out;
    int32_t x, y, z;
    int32_t temp_x, temp_y, temp_z;
    int16_t cosY, sinY, cosX, sinX;

    const Point3DLong *p_in = &gVerticesLongScale[i];
    x = p_in->x;
    y = p_in->y;
    z = p_in->z;

    cosY   = fastCos(angleY);
    sinY   = fastSin(angleY);
    temp_x = (mulViaLogExp((int16_t)x, cosY) + mulViaLogExp((int16_t)z, sinY));
    temp_z = (mulViaLogExp((int16_t)x, sinY) + mulViaLogExp((int16_t)z, cosY));
    x = temp_x;
    z = temp_z;

    cosX   = fastCos(angleX);
    sinX   = fastSin(angleX);
    temp_y = (mulViaLogExp((int16_t)y, cosX) + mulViaLogExp((int16_t)z, sinX));
    temp_z = (mulViaLogExp((int16_t)y, sinX) + mulViaLogExp((int16_t)z, cosX));
    y = temp_y;
    z = temp_z;

    p_out.x = (int16_t)x;
    p_out.y = (int16_t)y;
    p_out.z = (int16_t)z;

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

#define CRUISE_ALT    ((int16_t)(2 * FP_ONE))  /* altitude for TAKEOFF→CRUISE   */
#define CRASH_CAM_X   ((int16_t)(2 * FP_ONE))  /* lateral crash boundary        */
#define LAND_CAM_X    ((int16_t)(FP_ONE))       /* landing alignment zone        */
#define CRASH_VEL_Y    50   /* max descent speed for safe touchdown             */
#define CRASH_FLASH_FRAMES 30  /* ~0.6 s at 50 Hz                               */
#define CRUISE_DWELL   50   /* frames in cruise before landing begins           */

/* ── Progressive difficulty ───────────────────────────────────────────────── *
 * Wind: fastSin(frame*wind_freq)>>wind_shift applied as lateral force.        *
 * wind_freq controls oscillation speed; wind_shift controls amplitude.        *
 * Takeoff timer: must reach CRUISE_ALT within takeoff_limit frames.           */
/* ── Fuel (Lunar Lander style) ────────────────────────────────────────────── *
 * MAX_FUEL=128 fits in uint8_t; bar height = fuel (no shift), full bar=128px. *
 * Vertical gauge at right edge: top-anchored, shrinks downward as fuel drains.*/
#define MAX_FUEL          128   /* total fuel units per round (fits uint8_t)   */
#define FUEL_THRUST_COST    2   /* fuel/frame holding Up                       */
#define FUEL_STEER_COST     1   /* fuel/frame holding Left or Right            */
#define FUEL_BAR_X        317   /* screen X for vertical fuel bar (right edge) */
#define FUEL_BAR_TOP        1   /* screen Y of top anchor                      */

#define WIND_FREQ_BASE       4   /* LUT index increment/frame (slow oscillation) */
#define WIND_FREQ_STEP       2   /* increment per round                          */
#define WIND_SHIFT_BASE      8   /* amplitude shift: FP_ONE>>8 = ±4 peak force   */
#define WIND_SHIFT_MIN       5   /* strongest wind: FP_ONE>>5 = ±32              */
#define TAKEOFF_FRAMES_BASE 120  /* ~2.4s at 50Hz — generous on round 1          */
#define TAKEOFF_FRAMES_STEP  10  /* shrinks per round                            */
#define TAKEOFF_FRAMES_MIN   40  /* floor: ~0.8s                                 */

/* ── Landing strip constants ─────────────────────────────────────────────── *
 * draw_ground_strip() projects a rectangle at a dynamic z each frame.        *
 * STRIP_HALF: lateral half-width of the strip in world units.                *
 * STRIP_LEN:  depth of the strip box in world units.                         *
 * LANDING_APPROACH_DIST: strip z-distance when cruise/descent begins.       */
#define STRIP_HALF             ((int16_t)(FP_ONE / 2))   /* ±0.5 units — between grid lines */
#define STRIP_LEN              ((int16_t)(2 * FP_ONE))   /* box depth             */
#define LANDING_APPROACH_DIST  (8 * FP_ONE)              /* strip starts at grid far plane */

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
static Line    gGridLines[GRID_NUM_LINES + 1]; /* grid + sentinel */
static Line    gLogoLines[NUM_EDGES + 1];      /* logo + sentinel */


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
#include <assert.h>
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

void render(int16_t angleY, int16_t angleX, int16_t cam_y,
            int16_t z_phase, int16_t cam_x,
            bool show_grid, bool show_logo) {
    unsigned int i;
    /* ── Grid + strip (flight view) ─────────────────────────────────────── */
    if (show_grid) {
        int16_t z_wrap = (int16_t)((GRID_ZDIVS + 1) * GRID_ZSTEP);

        for (i = 0; i < (unsigned int)(GRID_ZDIVS + 1); i++) {
            int16_t z_rel = (int16_t)(gGridWorld[i].p0.z - z_phase);
            if (z_rel <= 0) z_rel += z_wrap;
            if (z_rel < HLINE_ZMIN) z_rel = HLINE_ZMIN;
            int16_t x_off    = divs16((int32_t)GRID_XHALF * FOCAL, z_rel);
            int16_t vp_shift = divs16((int32_t)cam_x * FOCAL, z_rel);
            int16_t y        = (int16_t)(SCREEN_HEIGHT_HALF + divs16((int32_t)cam_y * FOCAL, z_rel));
            gGridLines[i].p0.x = CLAMP((int16_t)(SCREEN_WIDTH_HALF - vp_shift - x_off), SC_X0, SC_X1);
            gGridLines[i].p0.y = CLAMP(y, SC_Y0, SC_Y1);
            gGridLines[i].p1.x = CLAMP((int16_t)(SCREEN_WIDTH_HALF - vp_shift + x_off), SC_X0, SC_X1);
            gGridLines[i].p1.y = CLAMP(y, SC_Y0, SC_Y1);
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
                gGridLines[i].p0.x = (int16_t)CLAMP(x0, SC_X0, SC_X1);
                gGridLines[i].p0.y = (int16_t)y0;
                gGridLines[i].p1.x = (int16_t)x1;
                gGridLines[i].p1.y = (int16_t)CLAMP(y1, SC_Y0, SC_Y1);
            }
        }

        memset(&gGridLines[GRID_NUM_LINES], 0, sizeof(Line));
        backend_draw_lines(gGridLines, GRID_NUM_LINES);
    }

    /* ── Logo (celebration view) ─────────────────────────────────────────── */
    if (show_logo) {
        for (i = 0; i < NUM_VERTICES; ++i) {
            Point3DInt t = rotate(i, angleY, angleX);
            gProjVerts[i] = project(t);
        }
        for (i = 0; i < NUM_EDGES; ++i) {
            Point2D p1 = gProjVerts[sv2025Edges[i][0]];
            Point2D p2 = gProjVerts[sv2025Edges[i][1]];
            p1.x = CLAMP(p1.x, SC_X0, SC_X1); p1.y = CLAMP(p1.y, SC_Y0, SC_Y1);
            p2.x = CLAMP(p2.x, SC_X0, SC_X1); p2.y = CLAMP(p2.y, SC_Y0, SC_Y1);
            gLogoLines[i].p0 = p1;
            gLogoLines[i].p1 = p2;
        }
        memset(&gLogoLines[NUM_EDGES], 0, sizeof(Line));
        backend_draw_lines(gLogoLines, NUM_EDGES);
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
    Line ls[5];
    /* Near crossbar */
    ls[0].p0.x = CLAMP((int16_t)sxl_n, SC_X0, SC_X1); ls[0].p0.y = (int16_t)sy_n;
    ls[0].p1.x = CLAMP((int16_t)sxr_n, SC_X0, SC_X1); ls[0].p1.y = (int16_t)sy_n;
    /* Far crossbar */
    ls[1].p0.x = CLAMP(sxl_f, SC_X0, SC_X1); ls[1].p0.y = CLAMP(sy_f, SC_Y0, SC_Y1);
    ls[1].p1.x = CLAMP(sxr_f, SC_X0, SC_X1); ls[1].p1.y = CLAMP(sy_f, SC_Y0, SC_Y1);
    /* Left guide */
    ls[2].p0.x = CLAMP((int16_t)sxl_n, SC_X0, SC_X1); ls[2].p0.y = (int16_t)sy_n;
    ls[2].p1.x = CLAMP(sxl_f, SC_X0, SC_X1);          ls[2].p1.y = CLAMP(sy_f, SC_Y0, SC_Y1);
    /* Right guide */
    ls[3].p0.x = CLAMP((int16_t)sxr_n, SC_X0, SC_X1); ls[3].p0.y = (int16_t)sy_n;
    ls[3].p1.x = CLAMP(sxr_f, SC_X0, SC_X1);          ls[3].p1.y = CLAMP(sy_f, SC_Y0, SC_Y1);
    memset(&ls[4], 0, sizeof(Line));
    backend_draw_lines(ls, 4);
}

typedef enum { STATE_TAKEOFF, STATE_CRUISE, STATE_LANDING, STATE_CRASH, STATE_SUCCESS } GameState;

static inline bool lateral_crash(int16_t cam_x) {
    return cam_x > CRASH_CAM_X || cam_x < -CRASH_CAM_X;
}

/* apply_lateral — update vel_x and cam_x from Left/Right keys.
 * use_fuel=true: deduct FUEL_STEER_COST and guard against underflow.
 * use_fuel=false (CRUISE): no fuel check, no deduction.
 * GCC constant-folds the use_fuel branch at each call site. */
static inline void apply_lateral(bool use_fuel, uint8_t keys,
                                  int16_t *vel_x, int16_t *cam_x, uint8_t *fuel)
{
    if (use_fuel) {
        if ((keys & KEY_LEFT)  && *fuel >= FUEL_STEER_COST) { *vel_x = (int16_t)(*vel_x - STEER); *fuel -= FUEL_STEER_COST; }
        if ((keys & KEY_RIGHT) && *fuel >= FUEL_STEER_COST) { *vel_x = (int16_t)(*vel_x + STEER); *fuel -= FUEL_STEER_COST; }
    } else {
        if (keys & KEY_LEFT)  *vel_x = (int16_t)(*vel_x - STEER);
        if (keys & KEY_RIGHT) *vel_x = (int16_t)(*vel_x + STEER);
    }
    *vel_x = (int16_t)(*vel_x - (*vel_x >> DRAG_SHIFT));
    if (*vel_x >  VEL_X_MAX) *vel_x =  VEL_X_MAX;
    if (*vel_x < -VEL_X_MAX) *vel_x = -VEL_X_MAX;
    *cam_x = (int16_t)(*cam_x + *vel_x);
}

/* apply_vertical — update vel_y and cam_y from Up key.
 * thrust/vel_y_max/clamp_ground are compile-time constants at each call site;
 * GCC folds all dependent branches away when inlined. */
static inline void apply_vertical(int16_t thrust, int16_t vel_y_max,
                                   bool clamp_ground, uint8_t keys,
                                   int16_t *vel_y, int16_t *cam_y, uint8_t *fuel)
{
    if ((keys & KEY_UP) && *fuel >= FUEL_THRUST_COST) {
        *vel_y = (int16_t)(*vel_y + thrust - GRAVITY);
        *fuel -= FUEL_THRUST_COST;
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
    uint8_t fuel          = MAX_FUEL;
    int16_t round         = 1;
    int16_t wind_freq     = WIND_FREQ_BASE;
    int16_t wind_shift    = WIND_SHIFT_BASE;
    int16_t takeoff_limit = TAKEOFF_FRAMES_BASE;
    int16_t takeoff_timer = TAKEOFF_FRAMES_BASE;
    GameState state       = STATE_TAKEOFF;

    if (argc >= 2) min_frame = (int16_t)atoi(argv[1]);
    if (argc >= 3) max_frame = (int16_t)atoi(argv[2]);

    loadLUTs();
    backend_init();
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

        backend_set_flash(state == STATE_CRASH);

        /* Logo spins only during celebration */
        if (state == STATE_SUCCESS) {
            angleY = (int16_t)(angleY + angleYinc);
            angleX = (int16_t)(angleX + angleXinc);
        }
        /* Grid always scrolls */
        z_phase = (int16_t)(z_phase + CAM_ZSPEED);
        if (z_phase >= GRID_ZSTEP) z_phase = (int16_t)(z_phase - GRID_ZSTEP);

        /* Wind: sinusoidal lateral force — amplitude and speed increase each round.
         * Applied in all active states so it's continuous across the flight.
         * Frozen during SUCCESS and CRASH so the player doesn't drift off-centre. */
        if (state != STATE_SUCCESS && state != STATE_CRASH) {
            int16_t wind = fastSin((int16_t)(frame * wind_freq)) >> wind_shift;
            vel_x = (int16_t)(vel_x + wind);
        }

        switch (state) {
        case STATE_TAKEOFF:
            /* Runway timer: must reach CRUISE_ALT before it expires */
            if (--takeoff_timer <= 0 && cam_y < CRUISE_ALT) {
                state = STATE_CRASH; crash_timer = CRASH_FLASH_FRAMES; break;
            }

            /* Vertical: Up = thrust (costs fuel); gravity always pulls */
            apply_vertical(TAKEOFF_THRUST, VEL_Y_MAX, true, keys, &vel_y, &cam_y, &fuel);

            /* Lateral: Left/Right steers against wind (costs fuel) */
            apply_lateral(true, keys, &vel_x, &cam_x, &fuel);

            if (lateral_crash(cam_x)) {
                state = STATE_CRASH; crash_timer = CRASH_FLASH_FRAMES;
            } else if (cam_y >= CRUISE_ALT) {
                cam_y = CRUISE_ALT; vel_y = 0;
                /* cam_x/vel_x intentionally NOT reset — carry into landing */
                strip_dist = LANDING_APPROACH_DIST;
                state = STATE_CRUISE; crash_timer = CRUISE_DWELL;
            }
            break;

        case STATE_CRUISE:
            apply_lateral(false, keys, &vel_x, &cam_x, &fuel);

            if (lateral_crash(cam_x)) {
                state = STATE_CRASH; crash_timer = CRASH_FLASH_FRAMES;
            } else if (--crash_timer <= 0) {
                state = STATE_LANDING;
            }
            break;

        case STATE_LANDING:
            /* Up brakes descent (costs fuel); gravity always pulls */
            apply_vertical(BRAKE_THRUST, CRASH_VEL_Y, false, keys, &vel_y, &cam_y, &fuel);
            apply_lateral(true, keys, &vel_x, &cam_x, &fuel);

            if (lateral_crash(cam_x)) {
                state = STATE_CRASH; crash_timer = CRASH_FLASH_FRAMES;
            } else if (cam_y <= 0) {
                int16_t abs_vel_y = (int16_t)(vel_y < 0 ? -vel_y : vel_y);
                int16_t abs_cam_x = (int16_t)(cam_x < 0 ? -cam_x : cam_x);
                if (abs_vel_y < CRASH_VEL_Y && abs_cam_x < LAND_CAM_X) {
                    /* Successful landing: advance difficulty, then celebrate */
                    round++;
                    wind_freq  = (int16_t)(WIND_FREQ_BASE + (round - 1) * WIND_FREQ_STEP);
                    wind_shift = (int16_t)(WIND_SHIFT_BASE - (round - 1));
                    if (wind_shift < WIND_SHIFT_MIN) wind_shift = WIND_SHIFT_MIN;
                    takeoff_limit -= TAKEOFF_FRAMES_STEP;
                    if (takeoff_limit < TAKEOFF_FRAMES_MIN) takeoff_limit = TAKEOFF_FRAMES_MIN;
                    fuel        = MAX_FUEL;
                    cam_y       = CAM_Y_INIT;
                    vel_y       = 0; vel_x = 0;
                    crash_timer = SUCCESS_FLASH_FRAMES;
                    state       = STATE_SUCCESS;
                } else {
                    state = STATE_CRASH; crash_timer = CRASH_FLASH_FRAMES;
                }
            }
            break;

        case STATE_CRASH:
            if (--crash_timer <= 0) {
                /* Reset to round 1 */
                round         = 1;
                wind_freq     = WIND_FREQ_BASE;
                wind_shift    = WIND_SHIFT_BASE;
                takeoff_limit = TAKEOFF_FRAMES_BASE;
                takeoff_timer = TAKEOFF_FRAMES_BASE;
                fuel  = MAX_FUEL;
                cam_y = CAM_Y_INIT; vel_y = 0; cam_x = 0; vel_x = 0;
                state = STATE_TAKEOFF;
            }
            break;

        case STATE_SUCCESS:
            if (--crash_timer <= 0) {
                cam_y = CAM_Y_INIT; cam_x = 0; vel_y = 0; vel_x = 0;
                takeoff_timer = takeoff_limit;
                state = STATE_TAKEOFF;
            }
            break;
        }

        /* Advance strip approach — strip_dist counts down to 0 as player flies in */
        if (state == STATE_CRUISE || state == STATE_LANDING)
            strip_dist -= CAM_ZSPEED;

        /* Safety clamp for divs16 in render (crash detection fires 1 frame late) */
        if (cam_x >  3 * FP_ONE) cam_x =  (int16_t)(3 * FP_ONE);
        if (cam_x < -3 * FP_ONE) cam_x = -(int16_t)(3 * FP_ONE);

        if (frame >= min_frame) {
            backend_clear();
            {
                bool celebrating = (state == STATE_SUCCESS);
                render(angleY, angleX, cam_y, z_phase, cam_x, !celebrating, celebrating);
            }
            if (state == STATE_TAKEOFF) {
                int32_t z_end_val = (int32_t)takeoff_timer * CAM_ZSPEED;
                if (z_end_val >= HLINE_ZMIN && z_end_val <= GRID_ZFAR) {
                    draw_ground_strip(STRIP_HALF, GRID_ZNEAR,
                                      (int16_t)z_end_val, cam_x, cam_y);
                }
            }
            if ((state == STATE_CRUISE || state == STATE_LANDING) &&
                strip_dist > 0 && strip_dist <= GRID_ZFAR) {
                int16_t sz = (int16_t)(strip_dist < HLINE_ZMIN ? HLINE_ZMIN : strip_dist);
                draw_ground_strip(STRIP_HALF, sz, (int16_t)(sz + STRIP_LEN), cam_x, cam_y);
            }
            /* Tally marks: one short vertical line per completed landing */
            if (round > 1) {
                /* max marks = (TAKEOFF_FRAMES_BASE-TAKEOFF_FRAMES_MIN)/TAKEOFF_FRAMES_STEP = 8 */
                Line tally[9];
                int16_t t, tx = 4;
                for (t = 0; t < round - 1; t++, tx = (int16_t)(tx + 5)) {
                    tally[t].p0.x = tx; tally[t].p0.y = 3;
                    tally[t].p1.x = tx; tally[t].p1.y = 10;
                }
                memset(&tally[t], 0, sizeof(Line));
                backend_draw_lines(tally, t);
            }
            /* Fuel bar: vertical line at right edge, top-anchored, shrinks down */
            if (fuel > 0) {
                Line fuel_bar[2];
                fuel_bar[0].p0.x = FUEL_BAR_X;
                fuel_bar[0].p0.y = FUEL_BAR_TOP;
                fuel_bar[0].p1.x = FUEL_BAR_X;
                fuel_bar[0].p1.y = (int16_t)(FUEL_BAR_TOP + fuel);
                memset(&fuel_bar[1], 0, sizeof(Line));
                backend_draw_lines(fuel_bar, 1);
            }
            /* Direction arrows: arrowhead at screen edge pointing toward x=0.
             * Arrow shows when cam_x drifts more than FP_ONE/2 off-centre. */
            if (state != STATE_CRASH && state != STATE_SUCCESS) {
                Line arr[3];
                memset(arr, 0, sizeof(arr));
                if (cam_x > FP_ONE / 2) {
                    /* Player is right of strip — point left */
                    arr[0].p0.x =  10; arr[0].p0.y = 97;
                    arr[0].p1.x =   3; arr[0].p1.y = 100;
                    arr[1].p0.x =   3; arr[1].p0.y = 100;
                    arr[1].p1.x =  10; arr[1].p1.y = 103;
                    backend_draw_lines(arr, 2);
                } else if (cam_x < -(FP_ONE / 2)) {
                    /* Player is left of strip — point right */
                    arr[0].p0.x = 310; arr[0].p0.y = 97;
                    arr[0].p1.x = 317; arr[0].p1.y = 100;
                    arr[1].p0.x = 317; arr[1].p0.y = 100;
                    arr[1].p1.x = 310; arr[1].p1.y = 103;
                    backend_draw_lines(arr, 2);
                }
            }
            backend_present(angleY, angleX);
        }
        frame++;
    }

    backend_cleanup();
    return 0;
}
