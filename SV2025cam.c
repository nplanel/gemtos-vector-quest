#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
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

void initLUTs() {
    loadLUTs();
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

/* ── Camera takeoff ───────────────────────────────────────────────────────── */
#define FOCAL        160       /* focal length = SCREEN_WIDTH_HALF → 90° HFOV  */
#define CAM_Y_INIT   ((int16_t)(FP_ONE / 4))   /* start 0.25 units above ground */
#define CAM_LIFT     5         /* altitude rise per frame (in FP_ONE/1024 units) */
#define CAM_ZSPEED   64        /* Z advance per frame (= FP_ONE/16)              */

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

/* Perspective projection for the ground grid.
 *   wx    — world X (FP_ONE units)
 *   z_rel — Z in front of camera (must be > 0, FP_ONE units)
 *   cam_y — camera altitude above ground (FP_ONE units)
 */
static inline Point2D project_persp(int16_t wx, int16_t z_rel, int16_t cam_y) {
    Point2D out;
    if (z_rel < 1) z_rel = 1;
    out.x = (int16_t)(SCREEN_WIDTH_HALF  + ((int32_t)wx    * FOCAL) / z_rel);
    out.y = (int16_t)(SCREEN_HEIGHT_HALF + ((int32_t)cam_y * FOCAL) / z_rel);
    return out;
}

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
static Line    gAllLines[GRID_NUM_LINES + NUM_EDGES + 1];

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
 * Cohen-Sutherland line clip against the full screen rectangle.
 * Uses int32_t throughout to handle large off-screen perspective coordinates.
 * Returns 1 if any part of the line is visible, 0 if entirely outside.
 */
#define CS_LEFT   1
#define CS_RIGHT  2
#define CS_TOP    4
#define CS_BOTTOM 8
#define CS_CODE(x,y) \
    (((x)<SC_X0?CS_LEFT:0)|((x)>SC_X1?CS_RIGHT:0)| \
     ((y)<SC_Y0?CS_TOP:0)|((y)>SC_Y1?CS_BOTTOM:0))

static int clip_line(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1) {
    for (;;) {
        int c0 = CS_CODE(*x0,*y0), c1 = CS_CODE(*x1,*y1);
        int32_t x, y, dx, dy, cout;
        if (!(c0|c1)) return 1;   /* trivially inside */
        if (c0&c1)    return 0;   /* trivially outside */
        cout = c0 ? c0 : c1;
        dx = *x1-*x0; dy = *y1-*y0;
        if      (cout&CS_LEFT)   { x=SC_X0; y=*y0+dy*(SC_X0-*x0)/dx; }
        else if (cout&CS_RIGHT)  { x=SC_X1; y=*y0+dy*(SC_X1-*x0)/dx; }
        else if (cout&CS_TOP)    { y=SC_Y0; x=*x0+dx*(SC_Y0-*y0)/dy; }
        else                     { y=SC_Y1; x=*x0+dx*(SC_Y1-*y0)/dy; }
        if (cout==c0) { *x0=x; *y0=y; } else { *x1=x; *y1=y; }
    }
}
#undef CS_LEFT
#undef CS_RIGHT
#undef CS_TOP
#undef CS_BOTTOM
#undef CS_CODE

void render(int16_t angleY, int16_t angleX, int16_t cam_y, int16_t z_phase) {
    unsigned int i;
    /* z_wrap: one full cycle of horizontal-line spacing */
    int16_t z_wrap = (int16_t)((GRID_ZDIVS + 1) * GRID_ZSTEP);

    /* Horizontal lines: scroll via z_phase, wrap near lines back to far end.
     * Both endpoints share the same Z, so CLAMP(x) is correct (y is uniform). */
    for (i = 0; i < (unsigned int)(GRID_ZDIVS + 1); i++) {
        int16_t z_rel = (int16_t)(gGridWorld[i].p0.z - z_phase);
        if (z_rel <= 0) z_rel += z_wrap;
        Point2D q0 = project_persp(gGridWorld[i].p0.x, z_rel, cam_y);
        Point2D q1 = project_persp(gGridWorld[i].p1.x, z_rel, cam_y);
        gAllLines[i].p0.x = CLAMP(q0.x, SC_X0, SC_X1);
        gAllLines[i].p0.y = CLAMP(q0.y, SC_Y0, SC_Y1);
        gAllLines[i].p1.x = CLAMP(q1.x, SC_X0, SC_X1);
        gAllLines[i].p1.y = CLAMP(q1.y, SC_Y0, SC_Y1);
    }

    /* Vertical lines: near endpoints are far off-screen in X (wide perspective),
     * so clip with Y interpolation so they converge to the vanishing point. */
    for (i = GRID_ZDIVS + 1; i < (unsigned int)GRID_NUM_LINES; i++) {
        int16_t wx = gGridWorld[i].p0.x;
        int32_t x0 = SCREEN_WIDTH_HALF  + ((int32_t)wx    * FOCAL) / gGridWorld[i].p0.z;
        int32_t y0 = SCREEN_HEIGHT_HALF + ((int32_t)cam_y * FOCAL) / gGridWorld[i].p0.z;
        int32_t x1 = SCREEN_WIDTH_HALF  + ((int32_t)wx    * FOCAL) / gGridWorld[i].p1.z;
        int32_t y1 = SCREEN_HEIGHT_HALF + ((int32_t)cam_y * FOCAL) / gGridWorld[i].p1.z;
        if (!clip_line(&x0, &y0, &x1, &y1))
            x0=x1=SC_X0, y0=y1=SC_Y0;   /* off-screen: collapse to a point */
        gAllLines[i].p0.x = (int16_t)x0;
        gAllLines[i].p0.y = (int16_t)y0;
        gAllLines[i].p1.x = (int16_t)x1;
        gAllLines[i].p1.y = (int16_t)y1;
    }

    /* Logo stays centred — orthographic, unaffected by camera */
    for (i = 0; i < NUM_VERTICES; ++i) {
        Point3DInt t = rotate(i, angleY, angleX);
        gProjVerts[i] = project(t);
    }

    /* Zero-sentinel for SegmentedMultiLine on Atari */
    memset(&gAllLines[GRID_NUM_LINES + NUM_EDGES], 0, sizeof(Line));

    /* Append logo edges with basic clamping */
    for (i = 0; i < NUM_EDGES; ++i) {
        Point2D p1 = gProjVerts[sv2025Edges[i][0]];
        Point2D p2 = gProjVerts[sv2025Edges[i][1]];
        p1.x = CLAMP(p1.x, SC_X0, SC_X1);
        p1.y = CLAMP(p1.y, SC_Y0, SC_Y1);
        p2.x = CLAMP(p2.x, SC_X0, SC_X1);
        p2.y = CLAMP(p2.y, SC_Y0, SC_Y1);
        gAllLines[GRID_NUM_LINES + i].p0 = p1;
        gAllLines[GRID_NUM_LINES + i].p1 = p2;
    }

    backend_draw_lines(gAllLines, GRID_NUM_LINES + NUM_EDGES);
}

int main(int argc, char *argv[]) {
    int16_t angleY = 0, angleX = 0;
    int16_t angleYinc, angleXinc;
    int16_t cam_y   = CAM_Y_INIT;   /* altitude: rises each frame */
    int16_t z_phase = 0;             /* Z scroll phase: wraps every GRID_ZSTEP */
    int frame = 0;
    int min_frame = 0;
    int max_frame = -1;

    if (argc >= 2) min_frame = atoi(argv[1]);
    if (argc >= 3) max_frame = atoi(argv[2]);

    initLUTs();
    backend_init();
    model_scale();
    build_grid();

    /* Convert rad/frame speeds to LUT-index increments */
    angleYinc = (int16_t)(0.08 * FP_ONE);
    angleXinc = (int16_t)(0.13 * FP_ONE);
    angleYinc = (int16_t)((int32_t)angleYinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));
    angleXinc = (int16_t)((int32_t)angleXinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));

    while (!backend_check_input()) {
        if (max_frame >= 0 && frame > max_frame)
            break;

        angleY += angleYinc;
        angleX += angleXinc;

        /* Rise and fly forward */
        cam_y   += CAM_LIFT;
        z_phase += CAM_ZSPEED;
        if (z_phase >= GRID_ZSTEP) z_phase -= GRID_ZSTEP;

        if (frame >= min_frame) {
            backend_clear();
            render(angleY, angleX, cam_y, z_phase);
            backend_present(angleY, angleX);
        }
        frame++;
    }

    backend_cleanup();
    return 0;
}
