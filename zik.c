#include <sys/types.h>
#include <mint/osbind.h>
#include <mint/sysvars.h>

#define INTERRUPTION_SERVICE_ADDRESS (volatile __uint8_t *)0xFFFFFA0FL
#define END_OF_INTERRUPT_TIMER_A  (~(1 << 5))

#define PSG_REGISTER_INDEX_ADDRESS (volatile __uint8_t *)0xFF8800
#define PSG_REGISTER_DATA_ADDRESS  (volatile __uint8_t *)0xFF8802

#define ZIK_INTRO  0
#define ZIK_MAIN   1
#define ZIK_FIRE   2
#define ZIK_NSLOTS 3

#define ZIK_MODE_THEME 0
#define ZIK_MODE_SFX   1

#define BARRIER() __asm__ volatile("" ::: "memory")

typedef struct {
    const unsigned char *data;
    unsigned int         nbFrames;
} ZikTrack;

/* ── Shared between init and ISR ──────────────────────────────────────────── */
static ZikTrack      zikTracks[ZIK_NSLOTS];

/* ── Shared between main and ISR (volatile) ───────────────────────────────── */
static volatile int  zikPendingSlot = -1;  /* >= 0 → switch theme to slot    */
static volatile int  zikPendingSfx  = -1;  /* >= 0 → play SFX from slot      */

/* ── Shared between disable/restore helpers ───────────────────────────────── */
static unsigned char originalKeyClick;

/* ── PSG helpers ──────────────────────────────────────────────────────────── */
static inline void write_PSG(__uint8_t reg, __uint8_t val)
{
    (*PSG_REGISTER_INDEX_ADDRESS) = reg;
    (*PSG_REGISTER_DATA_ADDRESS)  = val;
}

void soundOff(void)
{
    write_PSG(7, 0b00111111);
}

/* ── Interrupt handler ────────────────────────────────────────────────────── */
static void __attribute__((interrupt)) timera_interrupt(void)
{
    /* ISR-private state — static locals, never touched by main code */
    static const unsigned char *data    = NULL;
    static unsigned int         nbf     = 0;
    static unsigned int         frame   = 0;
    static int                  mode    = ZIK_MODE_THEME;
    static const unsigned char *bgData  = NULL;
    static unsigned int         bgNbf   = 0;
    static unsigned int         bgFrame = 0;

    /* read shared flags once */
    int slot = zikPendingSlot;
    int sfx  = zikPendingSfx;

    /* work in non-static locals so the compiler can use registers freely */
    const unsigned char *d = data;
    unsigned int n = nbf, f = frame, m = mode;

    /* 1. pending theme switch */
    if (slot >= 0) {
        d = zikTracks[slot].data;
        n = zikTracks[slot].nbFrames;
        f = 0;
        m = ZIK_MODE_THEME;
        BARRIER();
        zikPendingSlot = -1;
    }

    /* 2. pending SFX — save theme, start SFX */
    if (sfx >= 0) {
        if (m != ZIK_MODE_SFX) {
            unsigned int sfx_nf = zikTracks[sfx].nbFrames;
            bgData  = d;
            bgNbf   = n;
            bgFrame = n ? (f + sfx_nf) % n : 0;
            m = ZIK_MODE_SFX;
        }
        d = zikTracks[sfx].data;
        n = zikTracks[sfx].nbFrames;
        f = 0;
        BARRIER();
        zikPendingSfx = -1;
    }

    /* 3. advance frame; restore theme when SFX loops */
    if (f >= n) {
        if (m == ZIK_MODE_SFX) {
            d = bgData;
            n = bgNbf;
            f = bgFrame;
            m = ZIK_MODE_THEME;
        } else {
            f = 0;
        }
    } else {
        f++;
    }

    /* write back into static locals */
    data = d; nbf = n; frame = f; mode = m;

    /* 4. write PSG */
    const unsigned char *addr = d + f;
    for (int i = 0; i < 16; i++) {
        write_PSG(i, *addr);
        addr += n;
    }

    *(INTERRUPTION_SERVICE_ADDRESS) &= END_OF_INTERRUPT_TIMER_A;
}

/* ── Key-click helpers ────────────────────────────────────────────────────── */
static void disableKeyClick(void) {
    originalKeyClick = *(conterm);
    *(conterm) = 0b11111110 & originalKeyClick;
}
static void restoreKeyClick(void) { *(conterm) = originalKeyClick; }

/* ── Embedded YM data ─────────────────────────────────────────────────────── */
#include "sound/zik_data.h"

static void zik_load(int s, const unsigned char *bytes, unsigned int len)
{
    zikTracks[s].data     = bytes + 0x3b;
    zikTracks[s].nbFrames = (len - 0x3b - 4) / 16;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void zik_init(void)
{
    zik_load(ZIK_INTRO, kZikIntro, sizeof(kZikIntro));
    zik_load(ZIK_MAIN,  kZikMain,  sizeof(kZikMain));
    zik_load(ZIK_FIRE,  kZikFire,  sizeof(kZikFire));

    zikPendingSlot = ZIK_INTRO;   /* ISR initialises playback on first tick */
    zikPendingSfx  = -1;

    Supexec(disableKeyClick);
}

void zik_play(void)
{
    void play(void) {
        Jdisint(13);
        Xbtimer(0, 7, 246, timera_interrupt);
        Jenabint(13);
    }
    Supexec(play);
}

void zik_switch(int slot) { BARRIER(); zikPendingSlot = slot; }
void zik_sfx(int slot)    { BARRIER(); zikPendingSfx  = slot; }

void zik_stop(void)
{
    void stop(void) { Jdisint(13); soundOff(); }
    Supexec(stop);
}

void zik_cleanup(void) { Supexec(restoreKeyClick); }
