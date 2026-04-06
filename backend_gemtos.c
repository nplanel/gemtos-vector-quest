#include <string.h>
#include <stdint.h>
#include <osbind.h>
#include <mintbind.h>

#include "backend.h"
#include "stars.h"

#define ST_LOW_REZ_MODE    0
#define SCREEN_PLANES      4
#define SCREEN_BYTES_PER_ROW (SCREEN_WIDTH / 8 * SCREEN_PLANES) /* 160 */
#define SCREEN_SIZE_BYTES  (SCREEN_BYTES_PER_ROW * SCREEN_HEIGHT)

#define COLOR_BACKGROUND   0
#define COLOR_FOREGROUND   1

/* from segline.s / clipline.s */
void SegmentedLineSetup(void);
void SegmentedLine(unsigned long x0, unsigned long y0,
                   unsigned long x1, unsigned long y1, void *buffer);
void SegmentedMultiLine(Line *lines, void *buffer);

static void  *gScreenBufferA;
static void  *gScreenBufferB;
static void  *gActiveBuffer;
static void  *gDrawingBuffer;

static int           gOriginalRez = -1;
static uint16_t gOriginalPalette[16];

static int init_system(void) {
    int i;
    void *raw_buffer;

    gOriginalRez = Getrez();
    if (gOriginalRez < 0) return 0;

    for (i = 0; i < 16; ++i)
        gOriginalPalette[i] = (uint16_t)Setcolor(i, -1);

    raw_buffer = (void *)Malloc((long)SCREEN_SIZE_BYTES * 2 + 256L);
    if (!raw_buffer) return 0;

    gScreenBufferA = (void *)(((uintptr_t)raw_buffer + 255) & ~(uintptr_t)255);
    gScreenBufferB = (void *)((uintptr_t)gScreenBufferA + SCREEN_SIZE_BYTES);

    Setscreen(gScreenBufferA, gScreenBufferA, ST_LOW_REZ_MODE);
    gActiveBuffer  = gScreenBufferA;
    gDrawingBuffer = gScreenBufferB;

    Setcolor(COLOR_BACKGROUND,   PAL_BG);
    Setcolor(COLOR_FOREGROUND,   PAL_LINE);
    Setcolor(COLOR_FOREGROUND+1, PAL_STAR);
    Setcolor(COLOR_FOREGROUND+2, PAL_BLEND);
    Setcolor(COLOR_FOREGROUND+3, PAL_HUD);
    Setcolor(COLOR_FOREGROUND+4, PAL_MIX_02);
    Setcolor(COLOR_FOREGROUND+5, PAL_MIX_12);
    Setcolor(COLOR_FOREGROUND+6, PAL_MIX_012);

    Cursconf(0, 0);

    memset(gScreenBufferA, 0, SCREEN_SIZE_BYTES);
    memset(gScreenBufferB, 0, SCREEN_SIZE_BYTES);

    stars_init();

    return 1;
}

static void restore_system(void) {
    int i;
    Cursconf(1, 0);
    for (i = 0; i < 16; ++i)
        Setcolor(i, gOriginalPalette[i]);
    if (gOriginalRez != -1)
        Setscreen(-1L, -1L, gOriginalRez);
}

void backend_init(void) {
    if (!init_system()) {
        (void)Cconws("System initialization failed!\r\n");
        return;
    }
    SegmentedLineSetup();
}

void backend_draw_star(uint16_t x, uint16_t y) {
    /* Write to plane 1 of both screen buffers so stars survive buffer swaps.
       4-plane interleaved: each 8-byte group is [plane0][plane1][plane2][plane3].
       Plane 1 word offset within group = +2. */
    uint16_t *a = (uint16_t *)((uint8_t *)gScreenBufferA
                  + y * SCREEN_BYTES_PER_ROW + ((uint16_t)(x >> 4) << 3) + 2);
    uint16_t *b = (uint16_t *)((uint8_t *)gScreenBufferB
                  + y * SCREEN_BYTES_PER_ROW + ((uint16_t)(x >> 4) << 3) + 2);
    uint16_t bit = (uint16_t)(0x8000u >> (x & 15u));
    *a |= bit;
    *b |= bit;
}

/* Clear one bitplane in a screen buffer.
   plane_word: word index within each 8-byte interleaved group (0=plane0, 2=plane2, …).
   Inlined so constant plane_word folds into fixed offsets. */
static inline void clear_plane(void *buf, uint8_t plane_word) {
    uint16_t *p = (uint16_t *)buf + plane_word;
    uint8_t row;
    for (row = 0; row < SCREEN_HEIGHT; row++, p += SCREEN_BYTES_PER_ROW / 2) {
        p[ 0]=0; p[ 4]=0; p[ 8]=0; p[12]=0; p[16]=0;
        p[20]=0; p[24]=0; p[28]=0; p[32]=0; p[36]=0;
        p[40]=0; p[44]=0; p[48]=0; p[52]=0; p[56]=0;
        p[60]=0; p[64]=0; p[68]=0; p[72]=0; p[76]=0;
    }
}

void backend_clear(void) {
    clear_plane(gDrawingBuffer, 0);
}

void backend_hud_begin(void) {
    /* Clear plane 2 of both buffers; HUD is redrawn into both each time. */
    clear_plane(gScreenBufferA, 2);
    clear_plane(gScreenBufferB, 2);
}

void backend_hud_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    /* SegmentedLine with buffer offset +4 writes to plane 2.
       a0 = (buf+4) + y*160 + (x>>4)*8, so all OR.W offsets hit plane 2 words. */
    SegmentedLine(x0, y0, x1, y1, (uint8_t *)gScreenBufferA + 4);
    SegmentedLine(x0, y0, x1, y1, (uint8_t *)gScreenBufferB + 4);
}

void backend_draw_lines(Line *lines, int count __attribute__((unused))) {
    /* SegmentedMultiLine uses a zero-sentinel at lines[count],
       which render() sets before calling us */
    SegmentedMultiLine(lines, gDrawingBuffer);
}

void backend_present(int16_t angleY __attribute__((unused)),
                     int16_t angleX __attribute__((unused))) {
    void *temp;
    Setscreen(gDrawingBuffer, gDrawingBuffer, -1);
    Vsync();
    temp           = gActiveBuffer;
    gActiveBuffer  = gDrawingBuffer;
    gDrawingBuffer = temp;
}

void backend_cleanup(void) {
    (void)Cconws("End!\r\n");
    restore_system();
}

/* Simulate held-key state: Bconin is edge-triggered, so we reset a
 * per-key countdown each time a key event arrives. Auto-repeat at ~50 Hz
 * fires every 2-3 frames, so a countdown of 3 bridges the gaps cleanly. */
static int8_t gKeyTimer[5]; /* 0=UP 1=DOWN 2=LEFT 3=RIGHT 4=QUIT */

/* Atari ST arrow key scan codes (bits 16-23 of Bconin result) */
#define SCAN_UP    0x48
#define SCAN_DOWN  0x50
#define SCAN_LEFT  0x4B
#define SCAN_RIGHT 0x4D

uint8_t backend_get_keys(void) {
    int i;
    while (Bconstat(2)) {
        int32_t k    = (int32_t)Bconin(2);
        uint8_t ascii = (uint8_t)(k & 0xFF);
        uint8_t scan  = (uint8_t)((k >> 16) & 0xFF);
        if (ascii == 27 || ascii == 32) gKeyTimer[4] = 3;
        if (scan == SCAN_UP)    gKeyTimer[0] = 3;
        if (scan == SCAN_DOWN)  gKeyTimer[1] = 3;
        if (scan == SCAN_LEFT)  gKeyTimer[2] = 3;
        if (scan == SCAN_RIGHT) gKeyTimer[3] = 3;
    }
    uint8_t m = 0;
    for (i = 0; i < 5; i++)
        if (gKeyTimer[i] > 0) { m |= 1u << i; gKeyTimer[i]--; }
    return m;
}

int backend_check_input(void) { return (backend_get_keys() & KEY_QUIT) != 0; }

void backend_set_flash(int on) {
    Setcolor(COLOR_BACKGROUND, on ? PAL_FLASH : PAL_BG);
}
