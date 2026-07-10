#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stdbool.h>

#include "vquest.h"   /* RemoteState */

/* Platform-agnostic RS-232 serial interface for race mode.
 * Implemented in atari_serial.c (TOS AUX device) and posix_serial.c (named
 * pipes or regular files); benchmark/test backends may stub it instead.
 *
 * Wire protocol: 7-byte framed packet.  Every payload byte has bit 7 clear,
 * so the sync byte 0xAA (bit 7 set) is unambiguous mid-stream — the framer
 * resynchronizes on any byte with the high bit set, no escaping needed.
 *
 *   [0] 0xAA sync
 *   [1] status: bits 0-1 RS_* state, bit 2 FIRE event, bit 3 KILL,
 *               bit 4 FINISHED (crossed the line this lap, held),
 *               bit 5 lap parity (flips at every lap launch)
 *   [2] (cam_x + 8192) >> 7      cam_x is clamped to ±6144 by the game,
 *   [3] (cam_x + 8192) & 0x7F    so the biased value fits 14 bits
 *   [4] (progress >> 2) >> 7     per-lap progress in 4-unit steps so a
 *   [5] (progress >> 2) & 0x7F   31*FP_ONE course fits 14 bits
 *   [6] cam_y >> 5               altitude, clamped to 0..127 (32-unit steps)
 *
 * Pacing (see main loop): a packet is sent every 16th frame until a peer
 * packet has been received within the last REMOTE_TIMEOUT_FRAMES, then one
 * per frame (350 B/s at 50 Hz — well under 9600 baud).  Both sides beacon,
 * so pairing needs no role asymmetry. */

#define SERIAL_PKT_LEN 7
#define SERIAL_SYNC    0xAA

void  serial_init(const char *send_path, const char *recv_path);
void  serial_cleanup(void);
void  serial_send(const RemoteState *rs);  /* non-blocking best-effort */
bool  serial_recv(RemoteState *out);       /* true if ≥1 packet decoded this
                                            * call; position fields are the
                                            * newest packet's, fire/kill are
                                            * OR-ed across all of them */

/* ── Shared pack / unframe helpers (used by both transports) ─────────────── */

static inline void serial_pack(const RemoteState *rs, uint8_t buf[SERIAL_PKT_LEN])
{
    uint16_t x = (uint16_t)(rs->cam_x + 8192);
    uint16_t p = rs->progress >> 2;
    if (p > 0x3FFF) p = 0x3FFF;
    int16_t  a = rs->alt < 0 ? 0 : rs->alt;
    if (a > 127 << 5) a = 127 << 5;
    buf[0] = SERIAL_SYNC;
    buf[1] = (uint8_t)((rs->state & 3) | (rs->fire ? 4 : 0) | (rs->kill ? 8 : 0)
                       | (rs->finished ? 16 : 0) | (rs->lap ? 32 : 0));
    buf[2] = (uint8_t)((x >> 7) & 0x7F);
    buf[3] = (uint8_t)(x & 0x7F);
    buf[4] = (uint8_t)(p >> 7);
    buf[5] = (uint8_t)(p & 0x7F);
    buf[6] = (uint8_t)(a >> 5);
}

/* Byte-at-a-time deframer.  Feed every received byte; returns true exactly
 * when a complete packet has been decoded into *out. */
typedef struct {
    uint8_t buf[SERIAL_PKT_LEN - 1];
    uint8_t n;          /* payload bytes collected; 0xFF = waiting for sync */
} SerialFramer;

#define SERIAL_FRAMER_INIT { {0}, 0xFF }

static inline bool serial_unframe(SerialFramer *f, uint8_t b, RemoteState *out)
{
    if (b & 0x80) {                       /* only sync has bit 7 set */
        f->n = (b == SERIAL_SYNC) ? 0 : 0xFF;
        return false;
    }
    if (f->n == 0xFF) return false;       /* not synced */
    f->buf[f->n++] = b;
    if (f->n < SERIAL_PKT_LEN - 1) return false;
    f->n = 0xFF;
    out->state    = (uint8_t)(f->buf[0] & 3);
    out->fire     = (f->buf[0] & 4) != 0;
    out->kill     = (f->buf[0] & 8) != 0;
    out->finished = (f->buf[0] & 16) != 0;
    out->lap      = (uint8_t)((f->buf[0] >> 5) & 1);
    out->cam_x    = (int16_t)((((uint16_t)f->buf[1] << 7) | f->buf[2]) - 8192);
    /* Clamp to the course length: a corrupt-but-framed packet could otherwise
     * carry progress > 32767, which consumers' (int16_t)progress reads as
     * negative and flips the ghost's relative depth. */
    out->progress = progress_clamp((uint16_t)((((uint16_t)f->buf[3] << 7) | f->buf[4]) << 2));
    out->alt      = (int16_t)((uint16_t)f->buf[5] << 5);
    return true;
}

#endif /* SERIAL_H */
