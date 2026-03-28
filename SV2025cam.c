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

/* ── Camera orbit ─────────────────────────────────────────────────────────── */

#define CAM_RADIUS   80       /* screen-pixel camera orbit radius              */
#define CAM_SPEED_F  0.05f    /* radians/frame — one orbit ≈ 2 seconds         */

/* ── Ground plane grid (in fixed-point FP_ONE = 1024 units) ──────────────── */
/*
 * Projection:  screen_x = 160 + (p.x >> 5)
 *              screen_y = 100 + (p.y >> 5) - (p.z >> 6)
 *
 * GRID_Y = FP_ONE  → base screen_y = 132 (32 px below logo centre)
 * Z range [-2,+5]  → rows at screen_y 52..180
 * X range ±6 units → columns at screen_x -32..352 (wider than screen)
 *
 * At render time the camera offset is SUBTRACTED from every grid endpoint,
 * making the floor scroll while the spinning logo stays centred.
 */
#define GRID_Y      ((int16_t)(    FP_ONE))   /* 1024  — 32 px below centre   */
#define GRID_XHALF  ((int16_t)(6 * FP_ONE))   /* 6144  — columns ±192 px      */
#define GRID_XSTEP  ((int16_t)(    FP_ONE))   /* 1024  — 32 px column spacing */
#define GRID_XDIVS  12                         /* 13 vertical lines            */
#define GRID_ZMIN   ((int16_t)(-2 * FP_ONE))  /* -2048 — near row  y≈180      */
#define GRID_ZMAX   ((int16_t)( 5 * FP_ONE))  /*  5120 — far row   y≈52       */
#define GRID_ZDIVS  7                          /* 8 horizontal lines           */
#define GRID_NUM_LINES ((GRID_XDIVS + 1) + (GRID_ZDIVS + 1))   /* 13+8 = 21  */

#define SCREEN_WIDTH_HALF  (SCREEN_WIDTH  / 2)
#define SCREEN_HEIGHT_HALF (SCREEN_HEIGHT / 2)

static inline Point2D project(Point3DInt p) {
    Point2D out;
    out.x = SCREEN_WIDTH_HALF  + (p.x >> (FP_SHIFT - 5));
    out.y = SCREEN_HEIGHT_HALF + (p.y >> (FP_SHIFT - 5)) - (p.z >> (FP_SHIFT - 4));
    return out;
}

static Line gGridLines[GRID_NUM_LINES];

/* Build the wireframe ground plane once at startup */
static void build_grid(void) {
    int i = 0;
    int xi, zi;
    int16_t x, z;
    Point3DInt p0, p1;

    /* Horizontal lines: vary X, constant Z and Y */
    for (zi = 0; zi <= GRID_ZDIVS; zi++) {
        z = (int16_t)(GRID_ZMIN + zi * FP_ONE);
        p0.x = (int16_t)(-GRID_XHALF); p0.y = GRID_Y; p0.z = z;
        p1.x =           GRID_XHALF;   p1.y = GRID_Y; p1.z = z;
        gGridLines[i].p0 = project(p0);
        gGridLines[i].p1 = project(p1);
        i++;
    }

    /* Vertical lines: constant X, vary Z */
    for (xi = 0; xi <= GRID_XDIVS; xi++) {
        x = (int16_t)(-GRID_XHALF + xi * GRID_XSTEP);
        p0.x = x; p0.y = GRID_Y; p0.z = GRID_ZMIN;
        p1.x = x; p1.y = GRID_Y; p1.z = GRID_ZMAX;
        gGridLines[i].p0 = project(p0);
        gGridLines[i].p1 = project(p1);
        i++;
    }
}

static Point2D gProjVerts[NUM_VERTICES];
static Line    gAllLines[GRID_NUM_LINES + NUM_EDGES + 1];

/*
 * cam_sx / cam_sz: camera position in screen pixels.
 * Subtracting them from every grid endpoint makes the floor scroll
 * as the camera orbits — the logo stays fixed at screen centre.
 *
 * ALL endpoints must be clamped to [0,319] x [0,199] before calling
 * backend_draw_lines: the Atari SegmentedLine assembly takes unsigned
 * short coordinates — passing negative values causes an address error.
 */
#define SC_X0   1
#define SC_X1 319
#define SC_Y0   1
#define SC_Y1 199

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

void render(int16_t angleY, int16_t angleX, int16_t cam_sx, int16_t cam_sz) {
    unsigned int i;

    /* Scroll the grid by the camera offset; clamp every endpoint */
    for (i = 0; i < (unsigned int)GRID_NUM_LINES; i++) {
        Point2D q0, q1;
        q0.x = (int16_t)(gGridLines[i].p0.x - cam_sx);
        q0.y = (int16_t)(gGridLines[i].p0.y - cam_sz);
        q1.x = (int16_t)(gGridLines[i].p1.x - cam_sx);
        q1.y = (int16_t)(gGridLines[i].p1.y - cam_sz);
        gAllLines[i].p0.x = CLAMP(q0.x, SC_X0, SC_X1);
        gAllLines[i].p0.y = CLAMP(q0.y, SC_Y0, SC_Y1);
        gAllLines[i].p1.x = CLAMP(q1.x, SC_X0, SC_X1);
        gAllLines[i].p1.y = CLAMP(q1.y, SC_Y0, SC_Y1);
    }

    /* Logo stays centred — no camera offset applied */
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
    int16_t cam_angle = 0, cam_angleinc;
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

    cam_angleinc = (int16_t)(CAM_SPEED_F * FP_ONE);
    cam_angleinc = (int16_t)((int32_t)cam_angleinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));

    while (!backend_check_input()) {
        int16_t cam_sx, cam_sz;

        if (max_frame >= 0 && frame > max_frame)
            break;

        angleY += angleYinc;
        angleX += angleXinc;
        cam_angle += cam_angleinc;

        /* Camera orbits in a circle over the XZ plane */
        cam_sx = (int16_t)(((int32_t)fastSin(cam_angle) * CAM_RADIUS) >> FP_SHIFT);
        cam_sz = (int16_t)(((int32_t)fastCos(cam_angle) * CAM_RADIUS) >> FP_SHIFT);

        if (frame >= min_frame) {
            backend_clear();
            render(angleY, angleX, cam_sx, cam_sz);
            backend_present(angleY, angleX);
        }
        frame++;
    }

    backend_cleanup();
    return 0;
}
