#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <osbind.h>
#include <mintbind.h>
#include <mint/sysvars.h>
#include "backend.h"
#include "atari_common.h"

#define ST_LOW_REZ_MODE    0

#define COLOR_BACKGROUND   0
#define COLOR_FOREGROUND   1

/* from segline.s / clipline.s */
void SegmentedLineSetup(void);
void SegmentedLine(uint16_t x0, uint16_t y0,
                   uint16_t x1, uint16_t y1, void *buffer);
void SegmentedMultiLine(Line *lines, void *buffer);

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
static int      gFlash;

/* Raw IKBD ACIA data register — one byte per interrupt */
#define KBD_DATA (*(volatile uint8_t *)0xfffc02)

/* KEY_* bitmask maintained by the IKBD interrupt handler */
static volatile uint8_t gKeyState;
static long int (*gOldIkbdSys)(void);
static _KBDVECS *gKbdVecs;

/* 16-step triangle-wave glow tables (Atari ST 0xRGB, 3 bits per channel).
   Grid (plane 0) is static — no glow.
   alien  (plane 1):    (gGlowFrame >> 1) & 15 →  32-frame cycle (~0.64 s @ 50 fps)
   remote (planes 0+1): same 32-frame cycle, yellow
   stars  (plane 3):    (gGlowFrame >> 2) & 15 →  64-frame cycle (~1.28 s @ 50 fps) */
static const uint16_t kGlowAlien[16] = {
    0x411, 0x522, 0x522, 0x633, 0x744, 0x755, 0x755, 0x766,
    0x766, 0x755, 0x755, 0x744, 0x633, 0x522, 0x522, 0x411
};
static const uint16_t kGlowRemote[16] = {
    0x440, 0x550, 0x550, 0x660, 0x771, 0x772, 0x772, 0x773,
    0x773, 0x772, 0x772, 0x771, 0x660, 0x550, 0x550, 0x440
};
/* Rebuild all 16 palette entries from current glow phase and flash state.
   Priority: HUD (plane 2) > remote (planes 0+1) > alien (plane 1)
             > grid (plane 0) > stars (plane 3).
   Indices 3/11 (planes 0+1 both set) are the remote player — but the
   start/finish line shares them: its edges are snapped onto the scrolling
   grid's horizontal lines by design, so most of its pixels are also index 3.
   With only two per-frame-cleared planes there are exactly three dynamic
   colours; finish line and remote sharing the third (yellow) is accepted.
   Grid×alien line crossings add isolated index-3 pixels too.
   Called once at init and once per frame in backend_present(). */
/* update_palette runs once per present — O3 island in the -Os build. */
#pragma GCC push_options
#pragma GCC optimize("O3")
static void update_palette(void) {
    uint16_t alien  = kGlowAlien [(gGlowFrame >> 1) & 15];
    uint16_t remote = kGlowRemote[(gGlowFrame >> 1) & 15];
    uint16_t star   = kGlowStar  [(gGlowFrame >> 2) & 15];
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
#pragma GCC pop_options

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

    gFlash = 0;
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
        /* Decode aviation convention directly into key state:
           physical up (push fwd) → KEY_DOWN; physical down (pull) → KEY_UP */
        uint8_t m = 0;
        if (data & 0x01) m |= KEY_DOWN;
        if (data & 0x02) m |= KEY_UP;
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
        /* F1: switch between 50Hz and 60Hz */
        if (scan == SCAN_F1 && release)    SYNCMODE ^= 0x02;
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

static void snd_setup(void);
static void snd_teardown(void);

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
    /* Fetch KBDVECS pointer from user mode — calling Kbdvbase() (trap #14) from
     * inside Supexec risks re-entering XBIOS in a non-reentrant way on some TOS
     * versions.  We store the pointer so install_joyvec/restore_joyvec can use it
     * without making any trap calls in supervisor mode. */
    gKbdVecs  = Kbdvbase();
    gKeyState = 0;
    Supexec(install_ikbdsys);
}

/* Per-frame backend code (plane clears, line batches, present): O3 under
 * the global -Os build.  The sound backend below pops back to Os. */
#pragma GCC push_options
#pragma GCC optimize("O3")
void backend_draw_star(uint16_t x, uint16_t y) {
    atari_draw_star(gScreenBufferB, x, y);
}

/* Clear one bitplane in a screen buffer.
   plane_word: word index within each 8-byte interleaved group (0=plane0, 2=plane2, …).
   Inlined so constant plane_word folds into fixed offsets. */
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

/* Per-buffer dirty y-range for planes 0+1: the rows holding lines since that
   buffer was last cleared.  backend_clear() erases only this span; the draw
   calls below merge each batch's range (gLinesYMin/Max from draw.c) into the
   buffer they target.  Starts full-screen so the first clears wipe whatever
   init/intro left behind.  Y0 > Y1 means the buffer is clean. */
static int16_t gDirtyY0[2] = {0, 0};
static int16_t gDirtyY1[2] = {SCREEN_HEIGHT - 1, SCREEN_HEIGHT - 1};

static inline int dirty_slot(void) {
    return gDrawingBuffer == gScreenBufferA ? 0 : 1;
}

static void dirty_merge(void) {
    int s = dirty_slot();
    if (gLinesYMin < gDirtyY0[s]) gDirtyY0[s] = gLinesYMin;
    if (gLinesYMax > gDirtyY1[s]) gDirtyY1[s] = gLinesYMax;
}

void backend_clear(void) {
    int s = dirty_slot();
    if (gDirtyY0[s] <= gDirtyY1[s])
        clear_planes_01_rows(gDrawingBuffer, gDirtyY0[s],
                             (int16_t)(gDirtyY1[s] - gDirtyY0[s] + 1));
    gDirtyY0[s] = SCREEN_HEIGHT;
    gDirtyY1[s] = -1;
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

/* Plane 1 is cleared every frame by backend_clear() (clear_planes_01).
   Lines are accumulated in vquest.c's gAlienLines[], then drawn in one pass. */
void backend_draw_alien_lines(Line *lines, int count __attribute__((unused))) {
    dirty_merge();
    SegmentedMultiLine(lines, (uint8_t *)gDrawingBuffer + 2);
}

/* Remote-player lines are the tail of the alien batch: already in plane 1,
   already merged into the dirty range.  One extra pass writes the identical
   pixels into plane 0, turning them into index 3 (yellow). */
void backend_draw_remote_lines(Line *lines, int count __attribute__((unused))) {
    SegmentedMultiLine(lines, gDrawingBuffer);
}

void backend_draw_lines(Line *lines, int count __attribute__((unused))) {
    /* SegmentedMultiLine uses a zero-sentinel at lines[count],
       which render() sets before calling us */
    dirty_merge();
    SegmentedMultiLine(lines, gDrawingBuffer);
}

#ifdef VQ_PERF
/* ── Cycle-headroom instrumentation (VQ_PERF builds only) ───────────────────
 * backend_present's Vsync() is replaced by a counted busy-wait on the TOS
 * VBL frame counter (_frclock, $466): the spins burned until the next VBL
 * are the frame's spare capacity, in fixed ~40-cycle units (see the loop's
 * disassembly / SPIN_CYC in perf_frames.sh).  spins == 0 means the frame
 * missed its VBL deadline.  Totals are printed by backend_cleanup and
 * harvested from hatari's --conout stream by perf_frames.sh. */
#include <stdio.h>
static uint32_t gPerfSpins, gPerfFrames, gPerfOverruns;
static void perf_wait_vbl(void)   /* Supexec: _frclock is protected memory */
{
    volatile int32_t *frclock = (volatile int32_t *)0x466;
    uint32_t spins = 0;
    int32_t  f     = *frclock;
    while (*frclock == f) spins++;
    gPerfSpins += spins;
    gPerfFrames++;
    if (spins == 0) gPerfOverruns++;
}
#endif

void backend_present(int16_t angleY __attribute__((unused)),
                     int16_t angleX __attribute__((unused))) {
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

void backend_cleanup(void) {
    snd_teardown();
    Supexec(restore_ikbdsys);
    restore_system();
#ifdef VQ_PERF
    printf("PERF frames=%lu spins=%lu overruns=%lu\r\n",
           (unsigned long)gPerfFrames, (unsigned long)gPerfSpins,
           (unsigned long)gPerfOverruns);
#endif
}

uint8_t backend_get_keys(void) {
    return gKeyState;
}

int backend_check_input(void) { return (backend_get_keys() & KEY_QUIT) != 0; }

void backend_set_flash(int on) {
    gFlash = on;
}

/* Sound leaves the O3 island: timera_interrupt runs 50×/s, so even a 2×
 * slowdown is ~0.1% CPU, while O3's unrolling of the inlined ym_fill_frame/
 * ym_write_regs loops cost 1.2 kB (timera_interrupt alone: 1868 → 702 B). */
#pragma GCC pop_options
#pragma GCC push_options
#pragma GCC optimize("Os")

/* ── Sound backend ──────────────────────────────────────────────────────────── */

#define SND_NSLOTS     5

static YmTrack       sndTracks[SND_NSLOTS];
static YmTrack       sndCurrentTheme;
static YmTrack       sndCurrentSfx;
static int           sfxActive = 0;

static volatile int  sndPendingSlot = -1;
static volatile int  sndPendingSfx  = -1;

static unsigned char sndOriginalKeyClick;

static void snd_silence(void)
{
    write_psg(7, 0b01111111);  /* disable all channels; bit 6 keeps Port A as output */
    write_psg(8, 0);
    write_psg(9, 0);
    write_psg(10, 0);
}

static void __attribute__((interrupt)) timera_interrupt(void)
{
    int slot = sndPendingSlot;
    if (slot >= 0) {
        sndCurrentTheme = sndTracks[slot]; sndCurrentTheme.frame = 0;
        BARRIER();
        sndPendingSlot = -1;
    }

    int sfx = sndPendingSfx;
    if (sfx >= 0) {
        sndCurrentSfx = sndTracks[sfx]; sndCurrentSfx.frame = 0;
        sfxActive = 1;
        BARRIER();
        sndPendingSfx = -1;
    }

    unsigned char out[16];
    ym_fill_frame(&sndCurrentTheme, out, 16);

    if (sfxActive) {
        unsigned char sfxr[14];
        ym_fill_frame(&sndCurrentSfx, sfxr, 14);
        unsigned char sr7 = sfxr[7];

        int own_a = (sr7 & 0x09) != 0x09;
        int own_b = (sr7 & 0x12) != 0x12;
        int own_c = (sr7 & 0x24) != 0x24;

        if (own_a) { out[0]=sfxr[0]; out[1]=sfxr[1]; out[8]=sfxr[8]; }
        if (own_b) { out[2]=sfxr[2]; out[3]=sfxr[3]; out[9]=sfxr[9]; }
        if (own_c) { out[4]=sfxr[4]; out[5]=sfxr[5]; out[10]=sfxr[10]; }

        unsigned char sfx_bits = (own_a ? 0x09 : 0) | (own_b ? 0x12 : 0) | (own_c ? 0x24 : 0);
        out[7] = (out[7] & ~sfx_bits) | (sr7 & sfx_bits);

        if ((own_a && !(sr7 & 0x08)) || (own_b && !(sr7 & 0x10)) || (own_c && !(sr7 & 0x20)))
            out[6] = sfxr[6];

        if ((own_a && (sfxr[8]&0x10)) || (own_b && (sfxr[9]&0x10)) || (own_c && (sfxr[10]&0x10))) {
            out[11]=sfxr[11]; out[12]=sfxr[12]; out[13]=sfxr[13];
        }
    }

    ym_write_regs(out, 16);

    ym_advance(&sndCurrentTheme);
    if (sfxActive && ym_advance(&sndCurrentSfx)) sfxActive = 0;

    *(SND_ISR_ADDRESS) &= SND_END_OF_INTERRUPT;
}

static void snd_supervisor_init(void) {
    sndOriginalKeyClick = *(conterm);
    snd_disable_key_click();
}
static void snd_restore_key_click(void) { *(conterm) = sndOriginalKeyClick; }

#include "snd_data.h"
#include "lz4Unpack.h"

#define SND_RAW_TOTAL (kZikIntroRawSize + kZikMainRawSize + kZikFireRawSize + \
                       kZikGameoverRawSize + kZikEnmyhitRawSize)

static void snd_load(int s, const unsigned char *bytes, unsigned int len)
{
    sndTracks[s].data     = bytes + 0x3b;
    sndTracks[s].nbFrames = (uint16_t)((len - 0x3b - 4) / 16);
    sndTracks[s].frame    = 0;
}

/* Unpack one LZ4-compressed YM track into dst and register it in slot s.
 * Returns the unpacked size so the caller can advance its carve-out pointer. */
static long snd_unpack(int s, uint8_t *dst, const unsigned char *lz4)
{
    long len = lz4FrameUnpack(dst, lz4);
    snd_load(s, dst, (unsigned int)len);
    return len;
}

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
    uint8_t *buf = malloc(SND_RAW_TOTAL);
    if (!buf) {
        (void)Cconws("Sound buffer allocation failed!\r\n");
        exit(1);
    }
    buf += snd_unpack(SND_INTRO,    buf, kZikIntroLz4);
    buf += snd_unpack(SND_MAIN,     buf, kZikMainLz4);
    buf += snd_unpack(SND_FIRE,     buf, kZikFireLz4);
    buf += snd_unpack(SND_GAMEOVER, buf, kZikGameoverLz4);
    buf += snd_unpack(SND_ENMYHIT,  buf, kZikEnmyhitLz4);
    sndPendingSlot = SND_INTRO;
    sndPendingSfx  = -1;
    Supexec(snd_supervisor_init);
    Supexec(snd_play_supervisor);
}

static void snd_teardown(void)
{
    Supexec(snd_stop_supervisor);
    Supexec(snd_restore_key_click);
}

uint16_t backend_snd_switch(int slot) { BARRIER(); sndPendingSlot = slot; return (sndTracks[slot].nbFrames/2)-1; } // return the length of the music in game frames
void backend_snd_sfx(int slot)    { BARRIER(); sndPendingSfx  = slot; }

#pragma GCC pop_options
