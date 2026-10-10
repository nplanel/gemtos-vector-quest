#ifndef GLOW_H
#define GLOW_H

#include <stdint.h>
#include <stdbool.h>

/* 16-step triangle-wave glow tables (Atari ST 0xRGB, 3 bits per channel),
   shared by the ST and SDL backends and the loader.  Grid (plane 0) is
   static — no glow.  frame is the backend's per-present counter.
   alien  (plane 1):    (frame >> 1) & 15 →  32-frame cycle (~0.64 s @ 50 fps)
   remote (planes 0+1): same 32-frame cycle, yellow vs the bot / purple with a
                         live serial peer — see glow_remote()
   stars  (plane 3):    (frame >> 2) & 15 →  64-frame cycle (~1.28 s @ 50 fps) */
static const uint16_t kGlowAlien[16] = {
    0x411, 0x522, 0x522, 0x633, 0x744, 0x755, 0x755, 0x766,
    0x766, 0x755, 0x755, 0x744, 0x633, 0x522, 0x522, 0x411
};
static const uint16_t kGlowRemote[16] = {   /* yellow: the bot */
    0x440, 0x550, 0x550, 0x660, 0x771, 0x772, 0x772, 0x773,
    0x773, 0x772, 0x772, 0x771, 0x660, 0x550, 0x550, 0x440
};
static const uint16_t kGlowStar[16] = {
    0x222, 0x333, 0x333, 0x444, 0x555, 0x555, 0x666, 0x666,
    0x666, 0x666, 0x555, 0x555, 0x444, 0x333, 0x333, 0x222
};

static inline uint16_t glow_alien(uint16_t frame) { return kGlowAlien[(frame >> 1) & 15]; }
static inline uint16_t glow_star(uint16_t frame)  { return kGlowStar [(frame >> 2) & 15]; }

/* A live serial peer glows purple: the bot's yellow with each entry's G and
   B nibbles swapped. */
static inline uint16_t glow_remote(uint16_t frame, bool bot)
{
    uint16_t c = kGlowRemote[(frame >> 1) & 15];
    return bot ? c : (uint16_t)((c & 0xF00) | ((c & 0x0F0) >> 4) | ((c & 0x00F) << 4));
}

#endif /* GLOW_H */
