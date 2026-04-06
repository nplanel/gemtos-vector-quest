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

    /* 4-plane palette: index = plane3|plane2|plane1|plane0 */
    Setcolor( 0, PAL_BG);    /* 0000: background                  */
    Setcolor( 1, PAL_LINE);  /* 0001: plane 0 (grid)              */
    Setcolor( 2, PAL_ALIEN); /* 0010: plane 1 (aliens)            */
    Setcolor( 3, PAL_LINE);  /* 0011: grid + alien overlap        */
    Setcolor( 4, PAL_HUD);   /* 0100: plane 2 (HUD)              */
    Setcolor( 5, PAL_LINE);  /* 0101: grid + HUD                  */
    Setcolor( 6, PAL_HUD);   /* 0110: alien + HUD                 */
    Setcolor( 7, PAL_LINE);  /* 0111: grid + alien + HUD          */
    Setcolor( 8, PAL_STAR);  /* 1000: plane 3 (stars)             */
    Setcolor( 9, PAL_LINE);  /* 1001: stars + grid                */
    Setcolor(10, PAL_ALIEN); /* 1010: stars + alien               */
    Setcolor(11, PAL_LINE);  /* 1011: stars + grid + alien        */
    Setcolor(12, PAL_HUD);   /* 1100: stars + HUD                 */
    Setcolor(13, PAL_LINE);  /* 1101: stars + HUD + grid          */
    Setcolor(14, PAL_HUD);   /* 1110: stars + HUD + alien         */
    Setcolor(15, PAL_LINE);  /* 1111: all                         */

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
    /* Write to plane 3 of both screen buffers so stars survive buffer swaps.
       4-plane interleaved: each 8-byte group is [plane0][plane1][plane2][plane3].
       Plane 3 word offset within group = +6. */
    uint16_t *a = (uint16_t *)((uint8_t *)gScreenBufferA
                  + y * SCREEN_BYTES_PER_ROW + ((uint16_t)(x >> 4) << 3) + 6);
    uint16_t *b = (uint16_t *)((uint8_t *)gScreenBufferB
                  + y * SCREEN_BYTES_PER_ROW + ((uint16_t)(x >> 4) << 3) + 6);
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

/* Clear planes 0 and 1 together using 32-bit stores: each group is
   [P0 2B][P1 2B][P2 2B][P3 2B]; a uint32_t at offset 0 covers P0+P1.
   One 32-bit write per group vs two 16-bit writes — same loop count, half the stores. */
static inline void clear_planes_01(void *buf) {
    uint32_t *p = (uint32_t *)buf;   /* P0+P1 pair at byte offset 0 of each 8-byte group */
    uint8_t row;
    /* Each row = 160 bytes = 40 uint32_ts; P0+P1 pairs are every 2nd uint32_t (skip P2+P3). */
    for (row = 0; row < SCREEN_HEIGHT; row++, p += 40) {
        p[ 0]=0; p[ 2]=0; p[ 4]=0; p[ 6]=0; p[ 8]=0;
        p[10]=0; p[12]=0; p[14]=0; p[16]=0; p[18]=0;
        p[20]=0; p[22]=0; p[24]=0; p[26]=0; p[28]=0;
        p[30]=0; p[32]=0; p[34]=0; p[36]=0; p[38]=0;
    }
}

void backend_clear(void) {
    clear_planes_01(gDrawingBuffer);
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

/* Plane 1 is cleared every frame by backend_clear() (clear_planes_01). */
void backend_alien_begin(void) {}

void backend_alien_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    /* +2 = byte offset of plane 1 within each 8-byte interleaved group. */
    SegmentedLine(x0, y0, x1, y1, (uint8_t *)gDrawingBuffer + 2);
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
static int8_t gKeyTimer[6]; /* 0=UP 1=DOWN 2=LEFT 3=RIGHT 4=QUIT 5=FIRE */

/* Atari ST arrow key scan codes (bits 16-23 of Bconin result) */
#define SCAN_UP    0x48
#define SCAN_DOWN  0x50
#define SCAN_LEFT  0x4B
#define SCAN_RIGHT 0x4D
#define SCAN_SPACE 0x39

uint8_t backend_get_keys(void) {
    int i;
    while (Bconstat(2)) {
        int32_t k     = (int32_t)Bconin(2);
        uint8_t ascii = (uint8_t)(k & 0xFF);
        uint8_t scan  = (uint8_t)((k >> 16) & 0xFF);
        if (ascii == 27) gKeyTimer[4] = 3;            /* Escape = QUIT  */
        if (scan == SCAN_SPACE) gKeyTimer[5] = 3;     /* Space  = FIRE  */
        if (scan == SCAN_UP)    gKeyTimer[0] = 3;
        if (scan == SCAN_DOWN)  gKeyTimer[1] = 3;
        if (scan == SCAN_LEFT)  gKeyTimer[2] = 3;
        if (scan == SCAN_RIGHT) gKeyTimer[3] = 3;
    }
    uint8_t m = 0;
    for (i = 0; i < 6; i++)
        if (gKeyTimer[i] > 0) { m |= 1u << i; gKeyTimer[i]--; }
    return m;
}

int backend_check_input(void) { return (backend_get_keys() & KEY_QUIT) != 0; }

void backend_set_flash(int on) {
    Setcolor(COLOR_BACKGROUND, on ? PAL_FLASH : PAL_BG);
}
