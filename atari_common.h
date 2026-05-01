#ifndef ATARI_HW_H
#define ATARI_HW_H

#include <stdint.h>
#include <mint/sysvars.h>

/* ── Screen ─────────────────────────────────────────────────────────────────── */

#define SCREEN_BYTES_PER_ROW 160
#define PALETTE ((volatile uint16_t *)0xFF8240)

/* Write a star pixel to plane 3 of a screen buffer (Atari 4-plane layout). */
static inline void atari_draw_star(uint8_t *buf, uint16_t x, uint16_t y)
{
    const uint16_t byte = y * SCREEN_BYTES_PER_ROW + ((x >> 4) << 3) + 6;
    uint16_t *p = (uint16_t *)(buf + byte);
    *p |= (uint16_t)(0x8000u >> (x & 15u));
}

/* ── YM2149 PSG ─────────────────────────────────────────────────────────────── */

#define SND_ISR_ADDRESS      (volatile uint8_t *)0xFFFFFA0FL
#define SND_END_OF_INTERRUPT (~(1 << 5))
#define PSG_REGISTER_INDEX   (volatile uint8_t *)0xFF8800
#define PSG_REGISTER_DATA    (volatile uint8_t *)0xFF8802

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

/* Column-major YM frame data: data[frame + reg * nbFrames], nregs columns. */
typedef struct {
    const uint8_t *data;
    uint16_t       nbFrames;
    uint16_t       frame;
} YmTrack;

/* Fill out[0..nregs-1] with the current frame's register values. */
static inline void ym_fill_frame(const YmTrack *t, uint8_t *out, uint16_t nregs)
{
    const uint8_t *p = t->data + t->frame;
    for (uint16_t i = 0; i < nregs; i++) { out[i] = *p; p += t->nbFrames; }
}

/* Advance the frame counter. Returns 1 on wrap-around (loop completed). */
static inline uint8_t ym_advance(YmTrack *t)
{
    if (++t->frame >= t->nbFrames) { t->frame = 0; return 1; }
    return 0;
}

/* Write nregs PSG registers from regs[], skipping R13=0xFF (envelope sentinel). */
static inline void ym_write_regs(const uint8_t *regs, uint16_t nregs)
{
    for (uint16_t i = 0; i < nregs; i++) {
        if (i == 13 && regs[i] == 0xff) continue;
        /* R7 bit 6: Port A = output (floppy select); must not let music clear it */
        write_psg(i, (i == 7) ? (regs[i] | 0x40) : regs[i]);
    }
}

#endif /* ATARI_HW_H */
