#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <osbind.h>
#include <mintbind.h>
#include <mint/sysvars.h>

#include "backend.h"
#include "atari_common.h"

#define ST_LOW_REZ_MODE    0
#define SCREEN_SIZE_BYTES  (SCREEN_BYTES_PER_ROW * SCREEN_HEIGHT)

#define COLOR_BACKGROUND   0
#define COLOR_FOREGROUND   1

/* from segline.s / clipline.s */
void SegmentedLineSetup(void);
void SegmentedLine(uint16_t x0, uint16_t y0,
                   uint16_t x1, uint16_t y1, void *buffer);
void SegmentedMultiLine(Line *lines, void *buffer);

static void  *gScreenBufferA;
static void  *gScreenBufferB;
static void  *gActiveBuffer;
static void  *gDrawingBuffer;

static int           gOriginalRez = -1;
static uint16_t gOriginalPalette[16];

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
   alien (plane 1): (gGlowFrame >> 1) & 15 →  32-frame cycle (~0.64 s @ 50 fps)
   stars (plane 3): (gGlowFrame >> 2) & 15 →  64-frame cycle (~1.28 s @ 50 fps) */
static const uint16_t kGlowAlien[16] = {
    0x411, 0x522, 0x522, 0x633, 0x744, 0x755, 0x755, 0x766,
    0x766, 0x755, 0x755, 0x744, 0x633, 0x522, 0x522, 0x411
};
/* Rebuild all 16 palette entries from current glow phase and flash state.
   Priority: HUD (plane 2) > alien (plane 1) > grid (plane 0) > stars (plane 3).
   Called once at init and once per frame in backend_present(). */
static void update_palette(void) {
    uint16_t alien = kGlowAlien[(gGlowFrame >> 1) & 15];
    uint16_t star  = kGlowStar [(gGlowFrame >> 2) & 15];
    uint16_t bg    = gFlash ? PAL_FLASH : PAL_BG;
    /* static: Setpalette() stores the pointer and applies it at the next VBL,
     * so the array must outlive this stack frame. */
    static uint16_t pal[16];
    pal[0] = bg;      pal[1] = PAL_LINE; pal[2]  = alien;   pal[3]  = alien;
    pal[4] = PAL_HUD; pal[5] = PAL_LINE; pal[6]  = PAL_HUD; pal[7]  = PAL_HUD;
    pal[8] = star;    pal[9] = PAL_LINE; pal[10] = alien;   pal[11] = alien;
    pal[12]= PAL_HUD; pal[13]= PAL_LINE; pal[14] = PAL_HUD; pal[15] = PAL_HUD;
    Setpalette(pal);
}

static int init_system(void) {
    int i;
    void *raw_buffer;

    if (Getrez() == 2) { // hack mono => dump relocated prg
        FILE *f = fopen("VQUEST", "wb");
        if (!f) {
            printf("Error opening file for writing\n");
            return -1;
        }
        BASEPAGE *bp = (BASEPAGE*)Pexec(3, "VQUEST.TOS", NULL, NULL);
        fwrite(bp, 1, sizeof(BASEPAGE) + (bp->p_tlen + bp->p_dlen/* + bp->p_blen*/), f);
        fclose(f);

        printf("Dumped relocated program: TEXT=%ld bytes, DATA=%ld bytes, BSS=%ld bytes\r\n",
               bp->p_tlen, bp->p_dlen, bp->p_blen);
        printf("bp %p\r\n", bp);
        exit(0);
    }

    gOriginalRez = Getrez();
    if (gOriginalRez < 0) return 0;

    for (i = 0; i < 16; ++i)
        gOriginalPalette[i] = (uint16_t)Setcolor(i, -1);

    raw_buffer = (void *)Malloc((long)SCREEN_SIZE_BYTES * 2 + 256L);
    if (!raw_buffer) return 0;

    gScreenBufferA = (void *)(((uintptr_t)raw_buffer + 255) & ~(uintptr_t)255);
    gScreenBufferB = (void *)((uintptr_t)gScreenBufferA + SCREEN_SIZE_BYTES);

    gActiveBuffer  = gScreenBufferA;
    gDrawingBuffer = gScreenBufferB;

    gFlash = 0;
    gGlowFrame = 0;

    (void)Cursconf(0, 0);

    memset(gScreenBufferB, 0, SCREEN_SIZE_BYTES);
    /* Setscreen(ST_LOW_REZ_MODE) will reset gScreenBufferA
     * As such, memset() is optional, and it must happen before stars_init()
     * (or any screen drawing) below */
    Setscreen(gScreenBufferA, gScreenBufferA, ST_LOW_REZ_MODE);
    update_palette();

    stars_init();

    return 1;
}

static void restore_system(void) {
    int i;
    (void)Cursconf(1, 0);
    for (i = 0; i < 16; ++i)
        (void)Setcolor(i, gOriginalPalette[i]);
    if (gOriginalRez != -1)
        Setscreen(-1L, -1L, gOriginalRez);
}

/* Atari ST scan codes */
#define SCAN_ESC   0x01
#define SCAN_UP    0x48
#define SCAN_DOWN  0x50
#define SCAN_LEFT  0x4B
#define SCAN_RIGHT 0x4D
#define SCAN_SPACE 0x39

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
        (void)Cconws("System initialization failed!\r\n");
        return;
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

void backend_draw_star(uint16_t x, uint16_t y) {
    atari_draw_star((uint8_t *)gScreenBufferA, (uint8_t *)gScreenBufferB, x, y);
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

/* Plane 1 is cleared every frame by backend_clear() (clear_planes_01).
   Lines are accumulated in vquest.c's gAlienLines[], then drawn in one pass. */
void backend_draw_alien_lines(Line *lines, int count __attribute__((unused))) {
    SegmentedMultiLine(lines, (uint8_t *)gDrawingBuffer + 2);
}

void backend_draw_lines(Line *lines, int count __attribute__((unused))) {
    /* SegmentedMultiLine uses a zero-sentinel at lines[count],
       which render() sets before calling us */
    SegmentedMultiLine(lines, gDrawingBuffer);
}

void backend_present(int16_t angleY __attribute__((unused)),
                     int16_t angleX __attribute__((unused))) {
    void *temp;
    update_palette();
    Setscreen(gDrawingBuffer, gDrawingBuffer, -1);
    Vsync();
    temp           = gActiveBuffer;
    gActiveBuffer  = gDrawingBuffer;
    gDrawingBuffer = temp;
    gGlowFrame++;
}

void backend_cleanup(void) {
    snd_teardown();
    Supexec(restore_ikbdsys);
    (void)Cconws("End!\r\n");
    restore_system();
}

uint8_t backend_get_keys(void) {
    return gKeyState;
}

int backend_check_input(void) { return (backend_get_keys() & KEY_QUIT) != 0; }

void backend_set_flash(int on) {
    gFlash = on;
}

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
    write_psg(7, 0b00111111);
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

static void snd_load(int s, const unsigned char *bytes, unsigned int len)
{
    sndTracks[s].data     = bytes + 0x3b;
    sndTracks[s].nbFrames = (uint16_t)((len - 0x3b - 4) / 16);
    sndTracks[s].frame    = 0;
}

static void snd_play_supervisor(void)
{
    Jdisint(13);
    Xbtimer(0, 7, 246, timera_interrupt);
    Jenabint(13);
}

static void snd_stop_supervisor(void) { Jdisint(13); snd_silence(); }

static void snd_setup(void)
{
    snd_load(SND_INTRO,    kZikIntro,    sizeof(kZikIntro));
    snd_load(SND_MAIN,     kZikMain,     sizeof(kZikMain));
    snd_load(SND_FIRE,     kZikFire,     sizeof(kZikFire));
    snd_load(SND_GAMEOVER, kZikGameover, sizeof(kZikGameover));
    snd_load(SND_ENMYHIT,  kZikEnmyhit,  sizeof(kZikEnmyhit));
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

static uint16_t backend_snd_switch(int slot) { BARRIER(); sndPendingSlot = slot; return (sndTracks[slot].nbFrames/2)-1; } // return the length of the music in game frames
static void backend_snd_sfx(int slot)    { BARRIER(); sndPendingSfx  = slot; }
