#include <string.h>
#include <stdint.h>
#include <osbind.h>
#include <mintbind.h>

#include "backend.h"

#define ST_LOW_REZ_MODE    0
#define SCREEN_PLANES      4
#define SCREEN_BYTES_PER_ROW (SCREEN_WIDTH / 8 * SCREEN_PLANES) /* 160 */
#define SCREEN_SIZE_BYTES  (SCREEN_BYTES_PER_ROW * SCREEN_HEIGHT)

#define COLOR_BACKGROUND   0
#define COLOR_FOREGROUND   1

/* from segline.s / clipline.s */
void SegmentedLineSetup(void);
void SegmentedLine(unsigned short x0, unsigned short y0,
                   unsigned short x1, unsigned short y1, void *buffer);
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

    Setcolor(COLOR_BACKGROUND,   0x007);
    Setcolor(COLOR_FOREGROUND,   0x700);
    Setcolor(COLOR_FOREGROUND+1, 0x707);
    Setcolor(COLOR_FOREGROUND+2, 0x770);
    Setcolor(COLOR_FOREGROUND+3, 0x070);
    Setcolor(COLOR_FOREGROUND+4, 0x077);
    Setcolor(COLOR_FOREGROUND+5, 0x700);

    Cursconf(0, 0);

    memset(gScreenBufferA, 0, SCREEN_SIZE_BYTES);
    memset(gScreenBufferB, 0, SCREEN_SIZE_BYTES);

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

void backend_clear(void) {
    memset(gDrawingBuffer, 0, SCREEN_SIZE_BYTES);
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
        if (gKeyTimer[i] > 0) { m |= (uint8_t)(1 << i); gKeyTimer[i]--; }
    return m;
}

int backend_check_input(void) { return (backend_get_keys() & KEY_QUIT) != 0; }

void backend_set_flash(int on) {
    /* Normal: bg=0x007 (dark blue), fg=0x700 (red). Flash: swap them. */
    Setcolor(COLOR_BACKGROUND, on ? 0x700 : 0x007);
    Setcolor(COLOR_FOREGROUND,  on ? 0x007 : 0x700);
}
