#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef __m68k__
#include <endian.h>
#endif

#include "backend.h"

#define INT_ROTATION_SPEED ((short)(0.1f * (1 << 10))) /* Fixed-point representation */

/* Perspective/Model Constants */
#define LOGO_SCALE          (3.0f/230.0f)
#define PERSPECTIVE_FACTOR_F 180.0f
#define OBJECT_Z_OFFSET_F    800.0f

typedef struct {
    float x, y, z;
} Point3DFloat;

typedef struct {
    short x, y, z;
} Point3DInt;

typedef struct {
    long x, y, z;
} Point3DLong;

#define FP_SHIFT 10
#define FP_ONE (1 << FP_SHIFT)
#define FP_16  (16 * FP_ONE)

static inline double fixed_fp(short y_fp) {
    return (double)y_fp / FP_ONE;
}

#define LUT_SIZE 2048

short sinLUT[LUT_SIZE];
short cosLUT[LUT_SIZE];
short logLUT[LUT_SIZE];
short expLUT[LUT_SIZE * 2]; /* Extra range for exp */

void saveLUTs() {
    FILE *fp = fopen("luts", "w");
    fwrite(sinLUT, sizeof(sinLUT), 1, fp);
    fwrite(cosLUT, sizeof(cosLUT), 1, fp);
    fwrite(logLUT, sizeof(logLUT), 1, fp);
    fwrite(expLUT, sizeof(expLUT), 1, fp);
    fclose(fp);
}

void loadLUTs() {
    unsigned int i;
    FILE *fp = fopen("luts", "r");
    fread(sinLUT, sizeof(sinLUT), 1, fp);
    fread(cosLUT, sizeof(cosLUT), 1, fp);
    fread(logLUT, sizeof(logLUT), 1, fp);
    fread(expLUT, sizeof(expLUT), 1, fp);
    fclose(fp);
#ifndef __m68k__
    /* 'luts' is stored big-endian (m68k); convert to host byte order */
    for (i = 0; i < LUT_SIZE;     i++) sinLUT[i] = (short)be16toh((unsigned short)sinLUT[i]);
    for (i = 0; i < LUT_SIZE;     i++) cosLUT[i] = (short)be16toh((unsigned short)cosLUT[i]);
    for (i = 0; i < LUT_SIZE;     i++) logLUT[i] = (short)be16toh((unsigned short)logLUT[i]);
    for (i = 0; i < LUT_SIZE * 2; i++) expLUT[i] = (short)be16toh((unsigned short)expLUT[i]);
#endif
}

void initLUTs() {
#if 0
    for (int i = 0; i < LUT_SIZE; i++) {
        float angle = 2 * M_PI * i / LUT_SIZE;
        sinLUT[i] = (short)(sin(angle) * FP_ONE);
        cosLUT[i] = (short)(cos(angle) * FP_ONE);
    }
    for (int i = 0; i < LUT_SIZE; i++) {
        float x = 1.0f * i / (LUT_SIZE/4); /* 0 to 4 */
        logLUT[i] = (short)(log(x + 0.01f) * FP_ONE); /* Avoid log(0) */
    }
    for (int i = 0; i < LUT_SIZE * 2; i++) {
        float x = -16.0f + (32.0f * i / (LUT_SIZE * 2)); /* -16 to 16 */
        float expVal = exp(x);
        if (expVal > 32767.0f / FP_ONE) expVal = 32767.0f / FP_ONE;
        if (expVal < -32768.0f / FP_ONE) expVal = -32768.0f / FP_ONE;
        expLUT[i] = (short)(expVal * FP_ONE);
    }
    saveLUTs();
#else
    loadLUTs();
#endif
}

static inline short fastSin(short angle) {
    return sinLUT[angle & (LUT_SIZE-1)];
}

static inline short fastCos(short angle) {
    return cosLUT[angle & (LUT_SIZE-1)];
}

static inline short fastLog(short x) {
    short index = (x * (LUT_SIZE/4)) >> FP_SHIFT;
    return logLUT[index & (LUT_SIZE-1)];
}

static inline short fastExp(short x) {
    short index = ((x + (16 << FP_SHIFT)) * (LUT_SIZE*2)) / (32 << FP_SHIFT);
    return expLUT[index & ((LUT_SIZE*2)-1)];
}

static inline short mulViaLogExp(short a, short b) {
    short aa = (a < 0) ? -a : a;
    short bb = (b < 0) ? -b : b;
    short r  = fastExp(fastLog(aa) + fastLog(bb));
    if ((a != aa) ^ (b != bb))
        r = -r;
    return r;
}

short fixedMul(short a, short b) {
    return mulViaLogExp(a, b);
}

#include "SV2025.h"

#define NUM_VERTICES (sizeof(sv2025Vertices) / sizeof(sv2025Vertices[0]))
Point3DLong gVerticesLongScale[NUM_VERTICES];
#define NUM_EDGES (sizeof(sv2025Edges) / sizeof(sv2025Edges[0]))

void model_scale() {
    unsigned i;
    for (i = 0; i < NUM_VERTICES; i++) {
        gVerticesLongScale[i].x = (long)((sv2025Vertices[i].x * LOGO_SCALE) * FP_ONE);
        gVerticesLongScale[i].y = (long)((sv2025Vertices[i].y * LOGO_SCALE) * FP_ONE);
        gVerticesLongScale[i].z = (long)((sv2025Vertices[i].z * LOGO_SCALE) * FP_ONE);
    }
}

static inline Point3DInt rotate(unsigned i, short angleY, short angleX) {
    Point3DInt p_out;
    long x, y, z;
    long temp_x, temp_y, temp_z;
    short cosY, sinY, cosX, sinX;

    const Point3DLong *p_in = &gVerticesLongScale[i];
    x = p_in->x;
    y = p_in->y;
    z = p_in->z;

    cosY   = fastCos(angleY);
    sinY   = fastSin(angleY);
    temp_x = (mulViaLogExp(x, cosY) + mulViaLogExp(z, sinY));
    temp_z = (mulViaLogExp(x, sinY) + mulViaLogExp(z, cosY));
    x = temp_x;
    z = temp_z;

    cosX   = fastCos(angleX);
    sinX   = fastSin(angleX);
    temp_y = (mulViaLogExp(y, cosX) + mulViaLogExp(z, sinX));
    temp_z = (mulViaLogExp(y, sinX) + mulViaLogExp(z, cosX));
    y = temp_y;
    z = temp_z;

    p_out.x = (short)x;
    p_out.y = (short)y;
    p_out.z = (short)z;

    return p_out;
}

/* Project integer 3D point to 2D screen coordinates */
static inline Point2D project(Point3DInt p) {
#define SCREEN_WIDTH_HALF  (SCREEN_WIDTH  / 2)
#define SCREEN_HEIGHT_HALF (SCREEN_HEIGHT / 2)
    Point2D projected;
    projected.x = SCREEN_WIDTH_HALF  + (p.x >> (FP_SHIFT-5));
    projected.y = SCREEN_HEIGHT_HALF + (p.y >> (FP_SHIFT-5)) - (p.z >> (FP_SHIFT-4));
    return projected;
}

void render(short angleY, short angleX) {
    Point2D projectedVertices[NUM_VERTICES];
    Line    lines[NUM_EDGES + 1];
    unsigned short i;

    for (i = 0; i < NUM_VERTICES; ++i) {
        Point3DInt transform = rotate(i, angleY, angleX);
        projectedVertices[i] = project(transform);
    }

    memset(&lines[NUM_EDGES], 0, sizeof(Line));
    for (i = 0; i < NUM_EDGES; ++i) {
        int v1_idx = sv2025Edges[i][0];
        int v2_idx = sv2025Edges[i][1];
        Point2D p1 = projectedVertices[v1_idx];
        Point2D p2 = projectedVertices[v2_idx];

        /* TODO smart clipping */
        #define min_y 1
        #define max_y 199
        if (p1.y < min_y) p1.y = 1;
        if (p2.y < min_y) p2.y = 1;
        if (p1.y > max_y) p1.y = 199;
        if (p2.y > max_y) p2.y = 199;

        #define min_x 1
        #define max_x 319
        if (p1.x < min_x) p1.x = 1;
        if (p2.x < min_x) p2.x = 1;
        if (p1.x > max_x) p1.x = max_x;
        if (p2.x > max_x) p2.x = max_x;

        lines[i].p0 = p1;
        lines[i].p1 = p2;
    }

    backend_draw_lines(lines, NUM_EDGES);
}

int main(int argc, char *argv[]) {
    short angleY = 0, angleX = 0;
    short angleYinc, angleXinc;

    (void)argc;
    (void)argv;

    initLUTs();
    backend_init();
    model_scale();

    angleYinc = (short)(0.08 * FP_ONE);
    angleXinc = (short)(0.13 * FP_ONE);
    angleYinc = (angleYinc * LUT_SIZE) / (2 * FP_ONE * 31415 / 10000);
    angleXinc = (angleXinc * LUT_SIZE) / (2 * FP_ONE * 31415 / 10000);

    while (!backend_check_input()) {
        backend_clear();
        angleY += angleYinc;
        angleX += angleXinc;
        render(angleY, angleX);
        backend_present(angleY, angleX);
    }

    backend_cleanup();
    return 0;
}
