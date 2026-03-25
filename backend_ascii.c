/*
 * ASCII debug backend — prints structured frame data to stdout.
 * Pure standard C: compiles for both Linux (gcc) and Atari (m68k-atari-mint-gcc).
 *
 * Output format (one block per frame):
 *
 *   FRAME 42
 *   ANGLES angleY=512 angleX=321
 *   LINES 288
 *   BBOX x=12..308 y=45..155
 *   LINE 160,100 200,120
 *   ...
 *   END_FRAME
 */

#include <stdio.h>
#include "backend.h"

#define MAX_LINES 512

static int   gFrameCount;
static short gFrameAngleY;
static short gFrameAngleX;
static int   gLineCount;
static Line  gLines[MAX_LINES];

/* bounding box of rendered lines */
static short gBboxMinX, gBboxMaxX, gBboxMinY, gBboxMaxY;

void backend_init(void) {
    gFrameCount = 0;
    printf("BACKEND=ascii\n");
    fflush(stdout);
}

void backend_clear(void) {
    gLineCount  = 0;
    gBboxMinX   =  32767;
    gBboxMaxX   = -32767;
    gBboxMinY   =  32767;
    gBboxMaxY   = -32767;
}

static short min_s(short a, short b) { return a < b ? a : b; }
static short max_s(short a, short b) { return a > b ? a : b; }

void backend_draw_lines(Line *lines, int count) {
    int i;
    for (i = 0; i < count && gLineCount < MAX_LINES; ++i) {
        gLines[gLineCount++] = lines[i];
        gBboxMinX = min_s(gBboxMinX, min_s(lines[i].p0.x, lines[i].p1.x));
        gBboxMaxX = max_s(gBboxMaxX, max_s(lines[i].p0.x, lines[i].p1.x));
        gBboxMinY = min_s(gBboxMinY, min_s(lines[i].p0.y, lines[i].p1.y));
        gBboxMaxY = max_s(gBboxMaxY, max_s(lines[i].p0.y, lines[i].p1.y));
    }
}

void backend_present(short angleY, short angleX) {
    int i;
    gFrameAngleY = angleY;
    gFrameAngleX = angleX;

    printf("FRAME %d\n", gFrameCount);
    printf("ANGLES angleY=%d angleX=%d\n", (int)gFrameAngleY, (int)gFrameAngleX);
    printf("LINES %d\n", gLineCount);
    if (gLineCount > 0)
        printf("BBOX x=%d..%d y=%d..%d\n",
               (int)gBboxMinX, (int)gBboxMaxX,
               (int)gBboxMinY, (int)gBboxMaxY);
    for (i = 0; i < gLineCount; ++i)
        printf("LINE %d,%d %d,%d\n",
               (int)gLines[i].p0.x, (int)gLines[i].p0.y,
               (int)gLines[i].p1.x, (int)gLines[i].p1.y);
    printf("END_FRAME\n");
    fflush(stdout);

    ++gFrameCount;
}

void backend_cleanup(void) {
    printf("DONE frames=%d\n", gFrameCount);
    fflush(stdout);
}

int backend_check_input(void) {
    return 0;
}
