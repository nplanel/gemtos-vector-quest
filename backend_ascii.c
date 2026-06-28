/*
 * ASCII debug backend — prints structured frame data to stdout.
 * Pure standard C: compiles for both Linux (gcc) and Atari (m68k-atari-mint-gcc).
 * serial_* and platform_frame_pace() come from the platform-common file the
 * unity wrapper includes first (posix_serial.c or atari_serial.c).
 *
 * The backend has no input device, so it autopilots: FIRE (skips intro and
 * the start gate) + UP (climbs to cruise altitude) are always held.
 * On POSIX, VQ_FRAME_MS=<ms> paces frames (see posix_serial.c).
 *
 * Output format (one block per frame; ALINE = alien-plane line, where
 * aliens, missiles and the remote race player are drawn; RLINE = the remote
 * triangle's extra plane-0 copy — the same lines also appear as ALINEs):
 *
 *   FRAME 42
 *   ANGLES angleY=512 angleX=321
 *   LINES 288
 *   BBOX x=12..308 y=45..155
 *   LINE 160,100 200,120
 *   ...
 *   ALINES 9
 *   ALINE 160,90 150,110
 *   ...
 *   RLINES 3
 *   RLINE 160,95 157,105
 *   ...
 *   END_FRAME
 */

#include <stdbool.h>
#include <stdio.h>
#include "backend.h"

static int     gFrameCount;
static int16_t gFrameAngleY;
static int16_t gFrameAngleX;
static int     gLineCount;
static Line    gAsciiLines[MAX_DRAW_LINES];
static int     gALineCount;
static Line    gAsciiALines[MAX_DRAW_LINES];
static int     gRLineCount;
static Line    gAsciiRLines[MAX_DRAW_LINES];

/* bounding box of rendered lines */
static int16_t gBboxMinX, gBboxMaxX, gBboxMinY, gBboxMaxY;

void backend_init(void) {
    gFrameCount = 0;
    printf("BACKEND=ascii\n");
    fflush(stdout);
    stars_init();
}

void backend_draw_star(uint16_t x __attribute__((unused)),
                       uint16_t y __attribute__((unused))) {}

void backend_hud_begin(void) {}
void backend_hud_line(int16_t x0 __attribute__((unused)), int16_t y0 __attribute__((unused)),
                      int16_t x1 __attribute__((unused)), int16_t y1 __attribute__((unused))) {}

void backend_clear(void) {
    gLineCount  = 0;
    gALineCount = 0;
    gRLineCount = 0;
    gBboxMinX   =  32767;
    gBboxMaxX   = -32767;
    gBboxMinY   =  32767;
    gBboxMaxY   = -32767;
}

static int16_t min_s(int16_t a, int16_t b) { return a < b ? a : b; }
static int16_t max_s(int16_t a, int16_t b) { return a > b ? a : b; }

void backend_draw_lines(Line *lines, int count) {
    int i;
    for (i = 0; i < count && gLineCount < MAX_DRAW_LINES; ++i) {
        gAsciiLines[gLineCount++] = lines[i];
        gBboxMinX = min_s(gBboxMinX, min_s(lines[i].p0.x, lines[i].p1.x));
        gBboxMaxX = max_s(gBboxMaxX, max_s(lines[i].p0.x, lines[i].p1.x));
        gBboxMinY = min_s(gBboxMinY, min_s(lines[i].p0.y, lines[i].p1.y));
        gBboxMaxY = max_s(gBboxMaxY, max_s(lines[i].p0.y, lines[i].p1.y));
    }
}

void backend_draw_alien_lines(Line *lines, int count) {
    int i;
    for (i = 0; i < count && gALineCount < MAX_DRAW_LINES; ++i)
        gAsciiALines[gALineCount++] = lines[i];
}

void backend_draw_remote_lines(Line *lines, int count) {
    int i;
    for (i = 0; i < count && gRLineCount < MAX_DRAW_LINES; ++i)
        gAsciiRLines[gRLineCount++] = lines[i];
}

void backend_present(int16_t angleY, int16_t angleX) {
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
               (int)gAsciiLines[i].p0.x, (int)gAsciiLines[i].p0.y,
               (int)gAsciiLines[i].p1.x, (int)gAsciiLines[i].p1.y);
    printf("ALINES %d\n", gALineCount);
    for (i = 0; i < gALineCount; ++i)
        printf("ALINE %d,%d %d,%d\n",
               (int)gAsciiALines[i].p0.x, (int)gAsciiALines[i].p0.y,
               (int)gAsciiALines[i].p1.x, (int)gAsciiALines[i].p1.y);
    printf("RLINES %d\n", gRLineCount);
    for (i = 0; i < gRLineCount; ++i)
        printf("RLINE %d,%d %d,%d\n",
               (int)gAsciiRLines[i].p0.x, (int)gAsciiRLines[i].p0.y,
               (int)gAsciiRLines[i].p1.x, (int)gAsciiRLines[i].p1.y);
    printf("END_FRAME\n");
    fflush(stdout);

    ++gFrameCount;
    platform_frame_pace();
}

void backend_cleanup(void) {
    printf("DONE frames=%d\n", gFrameCount);
    fflush(stdout);
}

/* No input device: autopilot.  FIRE skips the intro and the press-fire gate
 * (and fires missiles in cruise); UP holds thrust so takeoff reaches cruise
 * altitude instead of timing out into a crash. */
uint8_t backend_get_keys(void)    { return KEY_UP | KEY_FIRE; }
int     backend_check_input(void) { return 0; }
void    backend_set_flash(int on __attribute__((unused))) {}

uint16_t backend_snd_switch(int slot) { (void)slot; return 0; }
void backend_snd_sfx(int slot)    { (void)slot; }
