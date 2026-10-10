#ifndef ATARI_HW_H
#define ATARI_HW_H

#include <stdint.h>
#include <mint/sysvars.h>

/* ── Screen ─────────────────────────────────────────────────────────────────── */

#define SCREEN_BYTES_PER_ROW 160
#define SCREEN_SIZE (SCREEN_BYTES_PER_ROW * 200)   /* 32000 B — full ST low-res screen */
#define PALETTE ((volatile uint16_t *)0xFF8240)
#define SYNCMODE (*((volatile uint8_t *)0xFF820A))

/* Canonical framebuffer type.  C 6.5p7 permits accessing an object through a
 * union type that includes the member type, so poking the same screen as
 * bytes / words / longs is strict-aliasing-clean — no -fno-strict-aliasing,
 * no may_alias attribute, and the wide (or.w / move.l) ops are preserved. */
typedef union {
    uint8_t  b[SCREEN_SIZE];
    uint16_t w[SCREEN_SIZE / 2];
    uint32_t l[SCREEN_SIZE / 4];
} screen_t;

/* Write a star pixel to plane 3 of a screen buffer (Atari 4-plane layout).
 * byte is always even (6 + multiples of 8 + y*160), so byte>>1 is exact. */
static inline void atari_draw_star(screen_t *buf, uint16_t x, uint16_t y)
{
    const uint16_t byte = y * SCREEN_BYTES_PER_ROW + ((x >> 4) << 3) + 6;
    buf->w[byte >> 1] |= (uint16_t)(0x8000u >> (x & 15u));
}

/* ── YM2149 PSG ─────────────────────────────────────────────────────────────── */

#define SND_ISR_ADDRESS      (volatile uint8_t *)0xFFFFFA0FL
#define SND_END_OF_INTERRUPT (~(1 << 5))
/* Sign-extended short addresses ($FFFF8800 = $FF8800 on the 24-bit bus): gcc
 * can then use the 16-bit absolute mode, shorter and 4 cycles faster. */
#define PSG_REGISTER_INDEX   (volatile uint8_t *)0xFFFF8800
#define PSG_REGISTER_DATA    (volatile uint8_t *)0xFFFF8802

#define BARRIER() __asm__ volatile("" ::: "memory")

static inline void write_psg(uint8_t reg, uint8_t val)
{
    (*PSG_REGISTER_INDEX) = reg;
    (*PSG_REGISTER_DATA)  = val;
}

static const uint16_t kGlowStar[16] = {
    0x222, 0x333, 0x333, 0x444, 0x555, 0x555, 0x666, 0x666,
    0x666, 0x666, 0x555, 0x555, 0x444, 0x333, 0x333, 0x222
};

/* Disable key-click noise. Call from supervisor mode (e.g. via Supexec).
   Callers that need to restore the original value must save *(conterm) first. */
static inline void snd_disable_key_click(void)
{
    *(conterm) &= ~1u;
}

/* ── YM track playback ──────────────────────────────────────────────────────── */

/* PSG registers a YM frame drives: R0-R13 only.  YM files also store R14/R15
 * (I/O ports A/B), but Port A is the floppy drive/side select and RS-232
 * RTS/DTR — writing it every tick would select both drives and pin the
 * handshake lines behind TOS's back.  gen_snd.c drops those columns. */
#define YM_SOUND_REGS 14

/* A track as baked by gen_snd.c: column-major, data[reg * nbFrames + frame]
 * for R0..R13, with R7 bit 6 (Port A = output) already set in every frame. */
typedef struct {
    const uint8_t *data;
    uint16_t       nbFrames;
    uint16_t       frame;
} YmTrack;

/* Write the current frame straight from the track to R0..R13 — the common
 * case, with no buffer in between.  R13 = 0xFF is the YM "envelope shape
 * unchanged" marker: any write to R13 restarts the envelope, so it is
 * skipped. */
static inline void ym_write_frame(const YmTrack *t)
{
    const uint8_t *p = t->data + t->frame;
    uint16_t n = t->nbFrames;
    uint8_t  r;
    for (r = 0; r < YM_SOUND_REGS - 1; r++, p += n) {
        *PSG_REGISTER_INDEX = r;
        *PSG_REGISTER_DATA  = *p;
    }
    if (*p != 0xff) { *PSG_REGISTER_INDEX = 13; *PSG_REGISTER_DATA = *p; }
}

/* Advance the frame counter. Returns 1 on wrap-around (loop completed). */
static inline uint8_t ym_advance(YmTrack *t)
{
    if (++t->frame >= t->nbFrames) { t->frame = 0; return 1; }
    return 0;
}

#endif /* ATARI_HW_H */
