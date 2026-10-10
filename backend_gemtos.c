#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <osbind.h>
#include <mintbind.h>
#include <mint/sysvars.h>
#include "backend.h"
#include "atari_common.h"
#include "glow.h"

#define ST_LOW_REZ_MODE    0

#define COLOR_BACKGROUND   0
#define COLOR_FOREGROUND   1

/* from segline.s / clipline.s */
void SegmentedLineSetup(void);
void SegmentedLine(uint16_t x0, uint16_t y0,
                   uint16_t x1, uint16_t y1, void *buffer);
void SegmentedMultiLine(const Line *lines, void *buffer);

static screen_t *gScreenBufferA;
static screen_t *gScreenBufferB;
static screen_t *gActiveBuffer;
static screen_t *gDrawingBuffer;

static int           gOriginalRez = -1;
static uint16_t gOriginalPalette[16];
static void  *gOriginalPhysbase;
static void  *gOriginalLogbase;
static void  *gRawBuffer;

static uint8_t  gGlowFrame;
static bool     gFlash;

/* Sound timer (Timer A) ticks since init: 2457600 Hz / 200 / 246 = 49.95 Hz
 * whatever the display rate, so it serves as backend_ticks() for free. */
static volatile uint16_t gTicks;

/* Raw IKBD ACIA data register — one byte per interrupt */
#define KBD_DATA (*(volatile uint8_t *)0xfffc02)

/* KEY_* bitmask maintained by the IKBD interrupt handler */
static volatile uint8_t gKeyState;

/* Refresh rate, as last set via F2 (see ikbdsys_handler).  Cached rather
 * than read back from SYNCMODE ($FF820A, a hardware register unreadable
 * from user mode) on every backend_get_hz() call: the IKBD interrupt
 * already runs in supervisor mode, so it updates this directly for free —
 * only the initial seed at backend_init needs a Supexec.  gHzChanged is
 * set alongside it so the main loop redraws the HUD only on the rare F2
 * press instead of polling every frame. */
static volatile uint8_t gSyncHz;
static volatile uint8_t gHzChanged;
static long int (*gOldIkbdSys)(void);
static _KBDVECS *gKbdVecs;

/* Opponent colour last passed to backend_draw_remote_lines: yellow (bot)
   until a live serial peer is drawn. */
static bool gOpponentIsBot = true;

/* Rebuild all 16 palette entries from current glow phase and flash state.
   Priority: HUD (plane 2) > remote (planes 0+1) > alien (plane 1)
             > grid (plane 0) > stars (plane 3).
   Indices 3/11 (planes 0+1 both set) are the remote player — but the
   start/finish line shares them: its edges are snapped onto the scrolling
   grid's horizontal lines by design, so most of its pixels are also index 3.
   With only two per-frame-cleared planes there are exactly three dynamic
   colours; finish line and remote sharing the third (yellow/purple) is
   accepted.  Grid×alien line crossings add isolated index-3 pixels too.
   Called once at init and once per frame in backend_present(); the alien
   glow only steps every 2nd frame, so on the others nothing changed and the
   Setpalette trap is skipped. */
static void update_palette(void) {
    static uint8_t shown = 0xFF;   /* never a valid key: the first call sets */
    uint8_t key = (uint8_t)(((gGlowFrame >> 1) & 31)   /* alien+star phase */
                            | (gFlash ? 0x20 : 0) | (gOpponentIsBot ? 0x40 : 0));
    if (key == shown) return;
    shown = key;
    uint16_t alien  = glow_alien(gGlowFrame);
    uint16_t remote = glow_remote(gGlowFrame, gOpponentIsBot);
    uint16_t star   = glow_star(gGlowFrame);
    uint16_t bg     = gFlash ? PAL_FLASH : PAL_BG;
    /* static: Setpalette() stores the pointer and applies it at the next VBL,
     * so the array must outlive this stack frame. */
    static uint16_t pal[16];
    pal[0] = bg;      pal[1] = PAL_LINE; pal[2]  = alien;   pal[3]  = remote;
    pal[4] = PAL_HUD; pal[5] = PAL_LINE; pal[6]  = PAL_HUD; pal[7]  = PAL_HUD;
    pal[8] = star;    pal[9] = PAL_LINE; pal[10] = alien;   pal[11] = remote;
    pal[12]= PAL_HUD; pal[13]= PAL_LINE; pal[14] = PAL_HUD; pal[15] = PAL_HUD;
    Setpalette(pal);
}

static int init_system(void) {
    int i;

    gOriginalRez = Getrez();
    if (gOriginalRez < 0) return 0;

    for (i = 0; i < 16; ++i)
        gOriginalPalette[i] = (uint16_t)Setcolor(i, -1);

    /* gScreenBufferA = current physical screen (seamless: stays displayed during init).
     * gScreenBufferB = Logbase if distinct from Physbase (GEM sets them apart), otherwise
     * allocate a separate buffer (bare TOS without GEM has Physbase == Logbase). */
    gScreenBufferA = gOriginalPhysbase = (void *)Physbase();
    gScreenBufferB = gOriginalLogbase = (void *)Logbase();
    if (gScreenBufferB == gScreenBufferA)
    {
        gRawBuffer = malloc(SCREEN_SIZE + 256);
        if (!gRawBuffer) return 0;
        gScreenBufferB = (void *)(((uintptr_t)gRawBuffer + 255) & ~(uintptr_t)255);
    }

    gFlash = false;
    gGlowFrame = 0;

    (void)Cursconf(0, 0);

    /* Set LOW_RES only if needed; skip when already set (e.g. launched from loader)
     * to avoid the mode-reset glitch on the currently-displayed screen.
     * TOS resets the target buffer on a mode change, so this must come before memset. */
    if (gOriginalRez != ST_LOW_REZ_MODE)
        Setscreen(gScreenBufferA, gScreenBufferA, ST_LOW_REZ_MODE);

    /* Draw stars into gScreenBufferB while gScreenBufferA is still displayed */
    memset(gScreenBufferB, 0, SCREEN_SIZE);
    stars_init();

    /* Switch display to gScreenBufferB at the next VBL, then mirror stars into A */
    Setscreen(gScreenBufferB, gScreenBufferB, -1);
    Vsync();
    memcpy(gScreenBufferA, gScreenBufferB, SCREEN_SIZE);

    update_palette();

    /* gScreenBufferB is now active/displayed; gScreenBufferA is the drawing buffer */
    gActiveBuffer  = gScreenBufferB;
    gDrawingBuffer = gScreenBufferA;

    return 1;
}

static void restore_system(void) {
    int i;
    (void)Cursconf(1, 0);
    for (i = 0; i < 16; ++i)
        (void)Setcolor(i, gOriginalPalette[i]);
    Setscreen(gOriginalPhysbase, gOriginalLogbase, gOriginalRez);
    Vsync();
    if (gRawBuffer) { free(gRawBuffer); }
}

/* Atari ST scan codes */
#define SCAN_ESC   0x01
#define SCAN_UP    0x48
#define SCAN_DOWN  0x50
#define SCAN_LEFT  0x4B
#define SCAN_RIGHT 0x4D
#define SCAN_SPACE 0x39
#define SCAN_F1    0x3b
#define SCAN_F2    0x3c
#define SCAN_D     0x20

/* ikbdsys replacement: called from the ACIA interrupt once per received byte.
   Reads the raw byte directly from the ACIA data register (clears RDRF so the
   original ikbdsys would see no data and return immediately if called).
   Joystick 1 arrives as a 2-byte sequence: 0xFF header, then direction/fire byte
     bit 0: up (physical fwd), bit 1: down (physical back),
     bit 2: left, bit 3: right, bit 7: fire button.
   Keyboard bytes: scan code | 0x80 on release, bare scan code on press. */
static long int ikbdsys_handler(void) {
    static uint8_t joy_pending = 0; /* waiting for 2nd byte of 0xFF joystick packet */
    static uint8_t mouse_skip  = 0; /* dx/dy bytes left to discard in mouse packet  */
    uint8_t data = KBD_DATA;
    if (joy_pending) {
        /* Push forward = accelerate, matching the keyboard's up-arrow
           (the keyboard branch below maps SCAN_UP to KEY_UP);
           pull back = brake, and later doubles as the mine-drop gesture
           (FIRE + KEY_DOWN). */
        uint8_t m = 0;
        if (data & 0x01) m |= KEY_UP;
        if (data & 0x02) m |= KEY_DOWN;
        if (data & 0x04) m |= KEY_LEFT;
        if (data & 0x08) m |= KEY_RIGHT;
        if (data & 0x80) m |= KEY_FIRE;
        gKeyState = (gKeyState & ~(KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT|KEY_FIRE)) | m;
        joy_pending = 0;
    } else if (mouse_skip) {
        mouse_skip--;
    } else if (data == 0xFF) {
        joy_pending = 1;
    } else if ((data & 0xF8) == 0xF8) {
        /* Mouse relative position header 0xF8-0xFB:
           bits 0-1 encode fire buttons (bit0=joy0/LMB, bit1=joy1/RMB).
           Followed by dx, dy bytes which we discard. */
        if (data & 0x03) gKeyState |=  KEY_FIRE;
        else             gKeyState &= ~KEY_FIRE;
        mouse_skip = 2;
    } else {
        uint8_t scan    = data & 0x7F;
        uint8_t release = data & 0x80;
        uint8_t bit     = 0;
        if (scan == SCAN_UP)    bit = KEY_UP;
        if (scan == SCAN_DOWN)  bit = KEY_DOWN;
        if (scan == SCAN_LEFT)  bit = KEY_LEFT;
        if (scan == SCAN_RIGHT) bit = KEY_RIGHT;
        if (scan == SCAN_ESC)   bit = KEY_QUIT;
        if (scan == SCAN_SPACE) bit = KEY_FIRE;
        if (scan == SCAN_D)     bit = KEY_DEBUG;
        if (scan == SCAN_F1)    bit = KEY_MODE;
        /* F2: switch between 50Hz and 60Hz */
        if (scan == SCAN_F2 && release) {
            SYNCMODE ^= 0x02;
            gSyncHz = (SYNCMODE & 0x02) ? 50 : 60;
            gHzChanged = 1;
        }
        if (bit) {
            if (release) gKeyState &= ~bit;
            else         gKeyState |=  bit;
        }
    }
    return 0;
}

/* These run inside Supexec (supervisor mode) — no trap calls allowed.
   gKbdVecs is fetched from user mode before Supexec is invoked. */
static void install_ikbdsys(void) {
    gOldIkbdSys      = gKbdVecs->ikbdsys;
    gKbdVecs->ikbdsys = ikbdsys_handler;
}

static void restore_ikbdsys(void) {
    gKbdVecs->ikbdsys = gOldIkbdSys;
}

/* Supexec: SYNCMODE ($FF820A) is a hardware register, unreadable from user
 * mode.  Seeds gSyncHz once at init; PAL vs NTSC machines power on to
 * different SYNCMODE values, so this must never be assumed. */
#ifdef VQ_PERF
static uint8_t gPerfHz = 50;   /* perf runs pin this rate; set by vquest.c */
#endif
static void read_syncmode(void) {
#ifdef VQ_PERF
    if (gPerfHz == 60) SYNCMODE &= ~0x02;
    else               SYNCMODE |= 0x02;
#endif
    gSyncHz = (SYNCMODE & 0x02) ? 50 : 60;
}

static void snd_setup(void);
static void snd_teardown(void);
static void marker_glyphs_init(void);

void backend_init(void) {
    snd_setup();
    if (!init_system()) {
        /* Returning would run the game without SegmentedLineSetup()/IKBD
         * installed; stop the sound timer and bail out instead. */
        snd_teardown();
        (void)Cconws("System initialization failed!\r\n");
        exit(1);
    }
    SegmentedLineSetup();
    marker_glyphs_init();
    /* Fetch KBDVECS pointer from user mode — calling Kbdvbase() (trap #14) from
     * inside Supexec risks re-entering XBIOS in a non-reentrant way on some TOS
     * versions.  We store the pointer so install_ikbdsys/restore_ikbdsys can use it
     * without making any trap calls in supervisor mode. */
    gKbdVecs  = Kbdvbase();
    gKeyState = 0;
    Supexec(read_syncmode);
    Supexec(install_ikbdsys);
}

/* Cold: init only, never runs per frame — compiles at the global -Os. */
void backend_draw_star(uint16_t x, uint16_t y) {
    atari_draw_star(gScreenBufferB, x, y);
}

/* Clear one bitplane in a screen buffer.
   plane_word: word index within each 8-byte interleaved group (0=plane0, 2=plane2, …).
   Inlined so constant plane_word folds into fixed offsets.  Unrolled, unlike
   clear_rect_plane2: clearing whole planes through that loop instead
   measured ~2k cycles a frame more, averaged over the intro. */
static inline void clear_plane(screen_t *buf, uint8_t plane_word) {
    uint16_t *p = buf->w + plane_word;
    uint8_t row;
    for (row = 0; row < SCREEN_HEIGHT; row++, p += SCREEN_BYTES_PER_ROW / 2) {
        p[ 0]=0; p[ 4]=0; p[ 8]=0; p[12]=0; p[16]=0;
        p[20]=0; p[24]=0; p[28]=0; p[32]=0; p[36]=0;
        p[40]=0; p[44]=0; p[48]=0; p[52]=0; p[56]=0;
        p[60]=0; p[64]=0; p[68]=0; p[72]=0; p[76]=0;
    }
}

/* Cold: only runs at the intro and race end, never per frame. */
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

/* Same interleave as clear_plane: plane 2 is word 2 of each 8-byte group,
   groups are 4 words apart, rows are SCREEN_BYTES_PER_ROW/2 words apart.
   x and w must be 16-px aligned (caller contract, see backend.h). */
static void clear_rect_plane2(screen_t *buf, int16_t x, int16_t y,
                              int16_t w, int16_t h) {
    uint16_t *p = buf->w + 2 + (x >> 4) * 4
                + (int32_t)y * (SCREEN_BYTES_PER_ROW / 2);
    int16_t row, col, nwords = (int16_t)(w >> 4);
    for (row = 0; row < h; row++, p += SCREEN_BYTES_PER_ROW / 2)
        for (col = 0; col < nwords; col++) p[col * 4] = 0;
}

void backend_hud_clear_rect(int16_t x, int16_t y, int16_t w, int16_t h) {
    clear_rect_plane2(gScreenBufferA, x, y, w, h);
    clear_rect_plane2(gScreenBufferB, x, y, w, h);
}

void backend_hud_note(const char *tag __attribute__((unused))) {}
void backend_debug_item(char label __attribute__((unused)), int16_t val __attribute__((unused))) {}

uint8_t backend_get_hz(void) { return gSyncHz; }

bool backend_hz_changed(void) {
    if (!gHzChanged) return false;
    gHzChanged = 0;
    return true;
}

/* Per-frame backend code (plane clears, line batches, present).  Compiled at
 * the global -Os like everything else: O3 pragma islands here measured +816 B
 * of text for no frame-time change (2026-10, VBL-exact perf_frames.sh) — the
 * time is in SegmentedLine and the asm clear below. */

/* Clear planes 0 and 1 of nrows rows from row0 using 32-bit stores: each
   8-byte group is [P0 2B][P1 2B][P2 2B][P3 2B]; the uint32_t at offset 0
   covers P0+P1.  Inline asm because GCC emits clr.l here, a read-modify-
   write (24 cycles/store on 68000) — move.l Dn,d16(An) is 16, cutting the
   clear from ~500 to ~340 cycles per row. */
static void clear_planes_01_rows(void *buf, int16_t row0, int16_t nrows) {
    uint8_t *p = (uint8_t *)buf + (int32_t)row0 * SCREEN_BYTES_PER_ROW;
    uint32_t zero = 0;
    int16_t  n = (int16_t)(nrows - 1);
    __asm__ volatile(
        "0:\n\t"
        "move.l %2,(%0)\n\t"
        "move.l %2,8(%0)\n\t"   "move.l %2,16(%0)\n\t"
        "move.l %2,24(%0)\n\t"  "move.l %2,32(%0)\n\t"
        "move.l %2,40(%0)\n\t"  "move.l %2,48(%0)\n\t"
        "move.l %2,56(%0)\n\t"  "move.l %2,64(%0)\n\t"
        "move.l %2,72(%0)\n\t"  "move.l %2,80(%0)\n\t"
        "move.l %2,88(%0)\n\t"  "move.l %2,96(%0)\n\t"
        "move.l %2,104(%0)\n\t" "move.l %2,112(%0)\n\t"
        "move.l %2,120(%0)\n\t" "move.l %2,128(%0)\n\t"
        "move.l %2,136(%0)\n\t" "move.l %2,144(%0)\n\t"
        "move.l %2,152(%0)\n\t"
        "lea 160(%0),%0\n\t"
        "dbra %1,0b"
        : "+a"(p), "+d"(n)
        : "d"(zero)
        : "memory");
}

/* Clear planes 0+1 of nrows rows from row0, word columns c0..c0+ncols-1
   (ncols 1..20), by a computed jump into the same unrolled store run: a
   row costs 22 + 16*ncols cycles instead of 338, for the narrow spans the
   alien batch leaves above the grid.  The entry skips the first 20-ncols
   stores, which are 4 bytes each (the .if checks it). */
static void clear_planes_01_cols(void *buf, int16_t row0, int16_t nrows,
                                 int16_t c0, int16_t ncols) {
    uint8_t *p = (uint8_t *)buf + (int32_t)row0 * SCREEN_BYTES_PER_ROW + (c0 << 3);
    uint32_t zero = 0;
    int16_t  n    = (int16_t)(nrows - 1);
    int16_t  skip = (int16_t)((20 - ncols) << 2);
    void    *entry;
    __asm__ volatile(
        "lea 1f(%%pc,%4.w),%2\n\t"
        "0:\n\t"
        "jmp (%2)\n"
        "1:\n\t"
        "move.l %3,152(%0)\n\t"
        "move.l %3,144(%0)\n\t" "move.l %3,136(%0)\n\t"
        "move.l %3,128(%0)\n\t" "move.l %3,120(%0)\n\t"
        "move.l %3,112(%0)\n\t" "move.l %3,104(%0)\n\t"
        "move.l %3,96(%0)\n\t"  "move.l %3,88(%0)\n\t"
        "move.l %3,80(%0)\n\t"  "move.l %3,72(%0)\n\t"
        "move.l %3,64(%0)\n\t"  "move.l %3,56(%0)\n\t"
        "move.l %3,48(%0)\n\t"  "move.l %3,40(%0)\n\t"
        "move.l %3,32(%0)\n\t"  "move.l %3,24(%0)\n\t"
        "move.l %3,16(%0)\n\t"  "move.l %3,8(%0)\n"
        "2:\n\t"
        "move.l %3,(%0)\n\t"
        ".if (2b - 1b) - 76\n\t"
        ".error \"clear_planes_01_cols: stores are not 4 bytes each\"\n\t"
        ".endif\n\t"
        "lea 160(%0),%0\n\t"
        "dbra %1,0b"
        : "+a"(p), "+d"(n), "=&a"(entry)
        : "d"(zero), "d"(skip)
        : "memory");
}

/* Per-buffer dirty ranges for planes 0+1: what lines touched since that
   buffer was last cleared, which backend_clear() erases.  Two rectangles:
   the world batch (grid, finish line, credits) as full-width rows, and the
   alien batch (aliens, missiles, ghost, mines, logo) as rows x word
   columns — it mostly holds small far objects, so the rows above the grid
   that only it reaches need only its columns.  The draw calls merge each
   batch's range (gLinesYMin/Max from draw.c; the alien batch's x scanned
   here) into the buffer they target.  The world rows start full-screen so
   the first clears wipe whatever init/intro left behind.  Y0 > Y1 means
   that rectangle is clean. */
static int16_t gDirtyY0[2] = {0, 0};
static int16_t gDirtyY1[2] = {SCREEN_HEIGHT - 1, SCREEN_HEIGHT - 1};
static int16_t gDirtyAY0[2] = {SCREEN_HEIGHT, SCREEN_HEIGHT};
static int16_t gDirtyAY1[2] = {-1, -1};
static int16_t gDirtyAX0[2] = {SCREEN_WIDTH, SCREEN_WIDTH};   /* pixels */
static int16_t gDirtyAX1[2] = {-1, -1};

static inline int dirty_slot(void) {
    return gDrawingBuffer == gScreenBufferA ? 0 : 1;
}

/* Grow the drawing buffer's world rows to cover y0..y1. */
static inline __attribute__((always_inline))
void dirty_rows(int16_t y0, int16_t y1) {
    int s = dirty_slot();
    if (y0 < gDirtyY0[s]) gDirtyY0[s] = y0;
    if (y1 > gDirtyY1[s]) gDirtyY1[s] = y1;
}

/* Grow the drawing buffer's alien rectangle to cover x0..x1 x y0..y1. */
static inline __attribute__((always_inline))
void dirty_alien(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    int s = dirty_slot();
    if (x0 < gDirtyAX0[s]) gDirtyAX0[s] = x0;
    if (x1 > gDirtyAX1[s]) gDirtyAX1[s] = x1;
    if (y0 < gDirtyAY0[s]) gDirtyAY0[s] = y0;
    if (y1 > gDirtyAY1[s]) gDirtyAY1[s] = y1;
}

/* Widen *lo..*hi to the x-extent of count lines. */
static inline __attribute__((always_inline))
void lines_x_range(const Line *l, int count,
                                 int16_t *lo, int16_t *hi) {
    for (; count > 0; count--, l++) {
        int16_t a = l->p0.x, b = l->p1.x;
        if (a > b) { int16_t t = a; a = b; b = t; }
        if (a < *lo) *lo = a;
        if (b > *hi) *hi = b;
    }
}

/* Rows y0..y1 (none if y0 > y1) of the alien columns c0..c0+nc-1. */
static void clear_alien_span(int16_t y0, int16_t y1, int16_t c0, int16_t nc) {
    if (y0 <= y1)
        clear_planes_01_cols(gDrawingBuffer, y0, (int16_t)(y1 - y0 + 1), c0, nc);
}

void backend_clear(void) {
    int s = dirty_slot();
    int16_t y0 = gDirtyY0[s], y1 = gDirtyY1[s];
    int16_t ay0 = gDirtyAY0[s], ay1 = gDirtyAY1[s];
    if (y0 <= y1)
        clear_planes_01_rows(gDrawingBuffer, y0, (int16_t)(y1 - y0 + 1));
    if (ay0 <= ay1) {   /* the alien rows the full-width ones did not cover */
        int16_t c0 = (int16_t)(gDirtyAX0[s] >> 4);
        int16_t nc = (int16_t)((gDirtyAX1[s] >> 4) - c0 + 1);
        if (y0 > y1) {
            clear_alien_span(ay0, ay1, c0, nc);
        } else {
            clear_alien_span(ay0, ay1 < y0 ? ay1 : (int16_t)(y0 - 1), c0, nc);
            clear_alien_span(ay0 > y1 ? ay0 : (int16_t)(y1 + 1), ay1, c0, nc);
        }
    }
    gDirtyY0[s]  = SCREEN_HEIGHT; gDirtyY1[s]  = -1;
    gDirtyAY0[s] = SCREEN_HEIGHT; gDirtyAY1[s] = -1;
    gDirtyAX0[s] = SCREEN_WIDTH;  gDirtyAX1[s] = -1;
}

/* Plane 1 is cleared every frame by backend_clear().  Lines are accumulated
   in draw.c's gLines[], then drawn in one pass.  A race batch is at most
   ~30 lines, so scanning its x-range (~80 cycles a line) is cheap next to
   the clearing it saves.  The gate's logo and text (~200 lines) span the
   screen anyway: past ALIEN_SCAN_MAX the batch is taken as full width
   unscanned. */
#define ALIEN_SCAN_MAX 64
void backend_draw_alien_lines(const Line *lines, int count) {
    int16_t lo = SCREEN_WIDTH, hi = -1;
    if (count > ALIEN_SCAN_MAX) { lo = 0; hi = SCREEN_WIDTH - 1; }
    else                        lines_x_range(lines, count, &lo, &hi);
    dirty_alien(lo, gLinesYMin, hi, gLinesYMax);
    SegmentedMultiLine(lines, (uint8_t *)gDrawingBuffer + 2);
}

/* The opponent marker's glyphs (backend.h), pre-rendered: 8 rows of one
   word, the glyph's 5 columns in bits 15-11.  SegmentedLine draws a segment
   shorter than 16 px with the same pixels whatever x & 15 is, so OR-ing
   these in at any x is pixel-identical to drawing the segments there
   (checked on Hatari for every glyph at all 16 sub-word offsets).  A blit
   is 8 word pairs where the lines were ~24 SegmentedLine calls, each done
   twice (plane 1, then plane 0 for the opponent colour). */
static uint16_t gMarkerGlyphs[MARKER_GLYPHS][8];

/* Cold: init only.  Rasterizes each glyph at x = 16 into rows 0-7 of the
   drawing buffer's plane 0 (blank: stars are plane 3, and the buffer is
   not displayed yet), reads the word column back and blanks it again. */
static void marker_glyphs_init(void) {
    uint16_t *col = gDrawingBuffer->w + 4;    /* x = 16..31, plane 0 */
    const Seg *seg;
    uint8_t g, r;
    for (g = 0; g < MARKER_GLYPHS; g++) {
        for (seg = kDigitSegs[g]; seg->x0 >= 0; seg++)
            SegmentedLine((uint16_t)(16 + seg->x0), (uint16_t)seg->y0,
                          (uint16_t)(16 + seg->x1), (uint16_t)seg->y1,
                          gDrawingBuffer);
        for (r = 0; r < 8; r++) {
            uint16_t *w = col + r * (SCREEN_BYTES_PER_ROW / 2);
            gMarkerGlyphs[g][r] = *w;
            *w = 0;
        }
    }
}

/* Word ORs into planes 0+1, no 32-bit shift: the glyph spills into the
   next word only past x & 15 == 11 (−1.5k cycles a cruise frame against
   shifting each row through a long). */
void backend_draw_marker_glyph(uint8_t g, int16_t x, int16_t y) {
    const uint16_t *bits = gMarkerGlyphs[g];
    uint16_t *row = gDrawingBuffer->w + (int32_t)y * (SCREEN_BYTES_PER_ROW / 2)
                  + ((x >> 4) << 2);
    uint8_t sh = (uint8_t)(x & 15), r;
    dirty_alien(x, y, (int16_t)(x + 4), (int16_t)(y + 7));
    if (sh <= 11) {   /* the 5 columns stay inside this word */
        for (r = 0; r < 8; r++, row += SCREEN_BYTES_PER_ROW / 2) {
            uint16_t b = (uint16_t)(bits[r] >> sh);
            row[0] |= b; row[1] |= b;
        }
    } else {
        for (r = 0; r < 8; r++, row += SCREEN_BYTES_PER_ROW / 2) {
            uint16_t hi = (uint16_t)(bits[r] >> sh);
            uint16_t lo = (uint16_t)(bits[r] << (16 - sh));
            row[0] |= hi; row[1] |= hi;   /* this word ...    */
            row[4] |= lo; row[5] |= lo;   /* ... and the next */
        }
    }
}

/* Remote-player lines are the tail of the alien batch: already in plane 1,
   already merged into its dirty rectangle.  One extra pass writes the identical
   pixels into plane 0, turning them into index 3 (yellow vs the bot, purple
   with a live serial peer). */
void backend_draw_remote_lines(const Line *lines, int count __attribute__((unused)),
                               bool bot) {
    gOpponentIsBot = bot;
    SegmentedMultiLine(lines, gDrawingBuffer);
}

void backend_draw_lines(const Line *lines, int count __attribute__((unused))) {
    /* SegmentedMultiLine stops at the x0 == 0 sentinel lines_seal() put at
       lines[count] */
    dirty_rows(gLinesYMin, gLinesYMax);
    SegmentedMultiLine(lines, gDrawingBuffer);
}

/* Gate credits raster cache (see backend.h): the plane-0 words of the
   credits' bounding box, captured from the first frame that draws them.
   Rasterizing the ~300 credit lines costs ~350k cycles per gate frame; the
   copy is ~1k word moves.  The box is ~89 rows x 12 words (~2.1 KB); a
   bigger layout simply isn't cached. */
#define CREDITS_CACHE_WORDS 1152
static uint16_t gCreditsCache[CREDITS_CACHE_WORDS];
static int16_t  gCreditsY0, gCreditsY1;      /* row span; Y1 < Y0 = none cached */
static int16_t  gCreditsC0, gCreditsCols;    /* first word column, column count */
static bool     gCreditsValid;

/* Copy the cached box between gCreditsCache and plane 0 of the drawing
   buffer, in either direction. */
static void credits_copy(bool capture) {
    uint16_t *c = gCreditsCache;
    uint16_t *row = gDrawingBuffer->w + (int32_t)gCreditsY0 * (SCREEN_BYTES_PER_ROW / 2)
                  + gCreditsC0 * 4;
    int16_t y, x;
    for (y = gCreditsY0; y <= gCreditsY1; y++, row += SCREEN_BYTES_PER_ROW / 2) {
        uint16_t *p = row;
        if (capture) for (x = 0; x < gCreditsCols; x++, p += 4) *c++ = *p;
        else         for (x = 0; x < gCreditsCols; x++, p += 4) *p = *c++;
    }
}

bool backend_credits_blit(void) {
    if (!gCreditsValid) return false;
    credits_copy(false);
    dirty_rows(gCreditsY0, gCreditsY1);
    return true;
}

/* Cold: draws, then captures, once. */
void backend_draw_credits(const Line *lines, int count) {
    int16_t x0 = SCREEN_WIDTH, x1 = -1;
    backend_draw_lines(lines, count);
    lines_x_range(lines, count, &x0, &x1);
    if (x1 < x0) return;
    gCreditsY0   = gLinesYMin;
    gCreditsY1   = gLinesYMax;
    gCreditsC0   = (int16_t)(x0 >> 4);
    gCreditsCols = (int16_t)((x1 >> 4) - gCreditsC0 + 1);
    if ((int32_t)(gCreditsY1 - gCreditsY0 + 1) * gCreditsCols > CREDITS_CACHE_WORDS)
        return;
    credits_copy(true);
    gCreditsValid = true;
}

#ifdef VQ_PERF
/* ── CPU-cost instrumentation (VQ_PERF builds only) ─────────────────────────
 * backend_present's Vsync() is replaced by a counted busy-wait on the TOS
 * VBL frame counter (_frclock, $466).  A frame that overruns its VBL does not
 * spin zero times — it spins until the NEXT VBL — so spare spins alone say
 * nothing once frames take more than one VBL.  Each frame therefore records
 * both the VBLs it spanned (frclock delta since the previous wait ended) and
 * the spins it burned; with the idle spins-per-VBL calibrated at exit, the
 * CPU work of a frame is  vbls * cal - spins  (in spin units), which is exact
 * however many VBLs the frame took.  Sums are kept per bucket (the GameState
 * vquest.c renders, PERF_BUCKET_INTRO for the title reveal), printed by
 * backend_cleanup and turned into cycles/frame by perf_frames.sh. */
#include <stdio.h>
#define PERF_BUCKETS 5
#define PERF_BUCKET_INTRO 4
static uint8_t  gPerfBucket = PERF_BUCKET_INTRO;   /* set by vquest.c */
static uint32_t gPerfFrames[PERF_BUCKETS], gPerfVbls[PERF_BUCKETS],
                gPerfSpins[PERF_BUCKETS];
static int32_t  gPerfPrevEnd;                     /* frclock after last wait */
static uint32_t gPerfLastSpins;

static void perf_wait_vbl(void)   /* Supexec: _frclock is protected memory */
{
    volatile int32_t *frclock = (volatile int32_t *)0x466;
    uint32_t spins = 0;
    int32_t  f     = *frclock;
    while (*frclock == f) spins++;
    gPerfLastSpins = spins;
    if (gPerfPrevEnd) {   /* the first call only opens the first frame */
        gPerfFrames[gPerfBucket]++;
        gPerfVbls[gPerfBucket]  += (uint32_t)(*frclock - gPerfPrevEnd);
        gPerfSpins[gPerfBucket] += spins;
    }
    gPerfPrevEnd = *frclock;
}

/* Spins in one idle VBL (timer interrupts included, as in a real frame). */
static uint32_t perf_calibrate(void)
{
    uint32_t sum = 0;
    int k;
    gPerfPrevEnd = 0;          /* stop accounting */
    Supexec(perf_wait_vbl);    /* align to a VBL edge */
    for (k = 0; k < 50; k++) {
        gPerfPrevEnd = 0;
        Supexec(perf_wait_vbl);
        sum += gPerfLastSpins;
    }
    return sum / 50;
}
#endif

void backend_present(void) {
    void *temp;
    update_palette();
    Setscreen(gDrawingBuffer, gDrawingBuffer, -1);
#ifdef VQ_PERF
    Supexec(perf_wait_vbl);
#else
    Vsync();
#endif
    temp           = gActiveBuffer;
    gActiveBuffer  = gDrawingBuffer;
    gDrawingBuffer = temp;
    gGlowFrame++;
}

uint8_t backend_get_keys(void) {
    return gKeyState;
}

uint16_t backend_ticks(void) { return gTicks; }

void backend_set_flash(bool on) {
    gFlash = on;
}

/* Cold: exit only, never runs per frame — compiles at the global -Os. */
void backend_cleanup(void) {
#ifdef VQ_PERF
    uint32_t cal = perf_calibrate();   /* sound timer still running: counted */
    int b;
#endif
    snd_teardown();
    Supexec(restore_ikbdsys);
    restore_system();
#ifdef VQ_PERF
    printf("PERF cal=%lu\r\n", (unsigned long)cal);
    for (b = 0; b < PERF_BUCKETS; b++)
        printf("PERF bucket=%d frames=%lu vbls=%lu spins=%lu\r\n", b,
               (unsigned long)gPerfFrames[b], (unsigned long)gPerfVbls[b],
               (unsigned long)gPerfSpins[b]);
#endif
}

/* ── Sound backend ──────────────────────────────────────────────────────────── */

#include "snd_data.h"   /* kZik*[] / kZik*Frames, baked by gen_snd.c */
_Static_assert(SND_DATA_REGS == YM_SOUND_REGS, "gen_snd.c and the player disagree");

/* A theme as gen_snd's --theme blocks: R<r> at frame f is
 * pool[idx[r * nbBlocks + f / 16] * 16 + f % 16]. */
typedef struct {
    const uint8_t *idx, *pool;
    uint16_t       nbFrames, nbBlocks;
} YmBlockTrack;

static const YmBlockTrack kSndThemes[] = {
    [SND_INTRO]    = { kZikIntroIdx,    kZikIntroPool,    kZikIntroFrames,    kZikIntroBlocks },
    [SND_MAIN]     = { kZikMainIdx,     kZikMainPool,     kZikMainFrames,     kZikMainBlocks },
    [SND_GAMEOVER] = { kZikGameoverIdx, kZikGameoverPool, kZikGameoverFrames, kZikGameoverBlocks },
};
static const YmTrack kSndEffects[] = {
    [SND_FIRE]     = { kZikFire,     kZikFireFrames,     0 },
    [SND_ENMYHIT]  = { kZikEnmyhit,  kZikEnmyhitFrames,  0 },
};

static const YmBlockTrack *sndTheme;
static uint16_t      sndThemeFrame;
static const uint8_t *sndThemeBlk[YM_SOUND_REGS];  /* each register's current block */
static YmTrack       sndCurrentSfx;
static int           sfxActive = 0;

static volatile int  sndPendingSlot = SND_INTRO;
static volatile int  sndPendingSfx  = -1;

static unsigned char sndOriginalKeyClick;

static void snd_silence(void)
{
    write_psg(7, 0b01111111);  /* disable all channels; bit 6 keeps Port A as output */
    write_psg(8, 0);
    write_psg(9, 0);
    write_psg(10, 0);
}

/* Write one tick with the sound effect overlaid on the theme, in one pass
 * straight to the PSG.  gen_snd baked, per effect frame, which registers
 * the effect takes (the channels it enables, their noise period and
 * envelope: see gen_snd.c's sfx_masks) and which R7 mixer bits it owns;
 * the theme keeps everything else.  R13 follows ym_write_frame's 0xFF
 * rule.  Unrolled: one bit test per register, no shifting mask.  Out of
 * line: it only runs while an effect plays, and inlined, its registers
 * made every tick save them. */
static __attribute__((noinline)) void snd_write_mixed(void)
{
    const uint8_t * const *t = sndThemeBlk;
    uint16_t o = sndThemeFrame & 15;
    const uint8_t *s = sndCurrentSfx.data + sndCurrentSfx.frame;
    uint16_t ns = sndCurrentSfx.nbFrames;
    const uint8_t *m = s + YM_SOUND_REGS * ns;    /* the mask rows */
    uint8_t lo = m[0], hi = m[ns], own = m[2 * ns], v;
#define MIX(r, bits, b) \
    v = ((bits) & (1u << (b))) ? *s : t[r][o];    \
    *PSG_REGISTER_INDEX = (r); *PSG_REGISTER_DATA = v; s += ns;
    MIX(0, lo, 0) MIX(1, lo, 1) MIX(2, lo, 2) MIX(3, lo, 3)
    MIX(4, lo, 4) MIX(5, lo, 5) MIX(6, lo, 6)
    v = (uint8_t)((t[7][o] & ~own) | (*s & own));
    *PSG_REGISTER_INDEX = 7; *PSG_REGISTER_DATA = v; s += ns;
    MIX(8, hi, 0) MIX(9, hi, 1) MIX(10, hi, 2) MIX(11, hi, 3) MIX(12, hi, 4)
#undef MIX
    v = (hi & 0x20) ? *s : t[13][o];
    if (v != 0xff) { *PSG_REGISTER_INDEX = 13; *PSG_REGISTER_DATA = v; }
}

static void __attribute__((interrupt)) timera_interrupt(void)
{
    int slot = sndPendingSlot;
    if (slot >= 0) {
        sndTheme      = &kSndThemes[slot];
        sndThemeFrame = 0;
        BARRIER();
        sndPendingSlot = -1;
    }
    slot = sndPendingSfx;
    if (slot >= 0) {
        sndCurrentSfx = kSndEffects[slot];
        sfxActive = 1;
        BARRIER();
        sndPendingSfx = -1;
    }

    if ((sndThemeFrame & 15) == 0) {             /* entering a new block */
        const uint8_t *ix = sndTheme->idx + (sndThemeFrame >> 4);
        uint16_t nb = sndTheme->nbBlocks;
        int16_t r;
        for (r = 0; r < YM_SOUND_REGS; r++, ix += nb)
            sndThemeBlk[r] = sndTheme->pool + ((uint16_t)*ix << 4);
    }
    if (likely(!sfxActive)) {
        const uint8_t * const *b = sndThemeBlk;
        uint16_t o = sndThemeFrame & 15;
        uint8_t  r;
        for (r = 0; r < YM_SOUND_REGS - 1; r++, b++) {
            *PSG_REGISTER_INDEX = r;
            *PSG_REGISTER_DATA  = (*b)[o];
        }
        if ((*b)[o] != 0xff) { *PSG_REGISTER_INDEX = 13; *PSG_REGISTER_DATA = (*b)[o]; }
    } else {
        snd_write_mixed();
        if (ym_advance(&sndCurrentSfx)) sfxActive = 0;
    }
    if (++sndThemeFrame >= sndTheme->nbFrames) sndThemeFrame = 0;
    gTicks++;

    *(SND_ISR_ADDRESS) &= SND_END_OF_INTERRUPT;
}

static void snd_supervisor_init(void) {
    sndOriginalKeyClick = *(conterm);
    snd_disable_key_click();
}
static void snd_restore_key_click(void) { *(conterm) = sndOriginalKeyClick; }

static void snd_play_supervisor(void)
{
    Jdisint(13);
    Xbtimer(0, 7, 246, timera_interrupt);
    Jenabint(13);
}

static void snd_stop_supervisor(void) {
    Jdisint(13);
    *(volatile uint8_t *)0xFFFFFA19 = 0;  /* TACR=0: stop Timer A so no tick fires after Jdisint */
    snd_silence();
}

static void snd_setup(void)
{
    Supexec(snd_supervisor_init);
    Supexec(snd_play_supervisor);
}

static void snd_teardown(void)
{
    Supexec(snd_stop_supervisor);
    Supexec(snd_restore_key_click);
}

void backend_snd_switch(int slot) { BARRIER(); sndPendingSlot = slot; }
void backend_snd_sfx(int slot)    { BARRIER(); sndPendingSfx  = slot; }
