#include <string.h>
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
static unsigned short gOriginalPalette[16];

static int init_system(void) {
    short i;
    void *raw_buffer;

    gOriginalRez = Getrez();
    if (gOriginalRez < 0) return 0;

    for (i = 0; i < 16; ++i)
        gOriginalPalette[i] = Setcolor(i, -1);

    raw_buffer = (void *)Malloc((SCREEN_SIZE_BYTES * 2) + 256);
    if (!raw_buffer) return 0;

    gScreenBufferA = (void *)(((long)raw_buffer + 255L) & ~255L);
    gScreenBufferB = (void *)((long)gScreenBufferA + SCREEN_SIZE_BYTES);

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
    short i;
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

void backend_present(short angleY __attribute__((unused)),
                     short angleX __attribute__((unused))) {
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

int backend_check_input(void) {
    if (Bconstat(2) != 0) {
        long key = Bconin(2);
        unsigned char ascii = (unsigned char)(key & 0xFF);
        if (ascii == ' ' || ascii == 27)
            return 1;
    }
    return 0;
}
