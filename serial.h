#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stdbool.h>

#include "vquest.h"   /* RemoteState */

/* Platform-agnostic RS-232 serial interface for race mode.
 * Implemented in atari_serial.c (TOS AUX device) and posix_serial.c (named
 * pipes or regular files); benchmark/test backends may stub it instead.
 *
 * Wire protocol: 6-byte framed packet, 37 payload bits + 5-bit checksum.  The
 * frame marker is folded into byte 0's bit 7 (bits 0-6 there are real payload,
 * unlike the old dedicated 0xAA sync byte), so every OTHER byte has bit 7
 * clear and is unambiguous mid-stream.
 *
 *   [0] bit7=1 (marker); bits 0-1 RS_* state, bit 2 FIRE event, bit 3 KILL,
 *               bit 4 FINISHED (finished the RACE, held),
 *               bit 5 RACE parity (flips at every race launch),
 *               bit 6 MINE event
 *   [1] (cam_x + 8192) >> 7      cam_x is clamped to ±6144 by the game,
 *   [2] (cam_x + 8192) & 0x7F    so the biased value fits 14 bits
 *   [3] bits 0-2 lap-1 (0..7 -> laps 1..8), bits 3-6 (progress>>2) bits 12-9
 *   [4] (progress>>2) bits 8-2   per-lap progress in 4-unit steps so the
 *   [5] bits 5-6 (progress>>2) bits 1-0, bits 0-4 checksum
 *
 * Checksum is the 8-bit sum of buf[0..4] plus buf[5] with its low 5 bits
 * masked off, truncated to 5 bits; buf[0]'s marker bit is excluded (ORed in
 * after the sum) and buf[5]'s checksum bits are excluded (masked before the
 * sum) so both sides sum the same 37 payload bits either way.
 *
 * Resync: a byte with bit 7 set unconditionally (re)starts a packet at n=1;
 * n==0 means not synced and continuation bytes are dropped until a marker
 * arrives; a completed packet returns to n==0.  Self-synchronizing because on
 * a clean stream exactly every 6th byte has bit 7 set.  A garbage decode needs
 * a spurious marker AND a lost real marker to coincide, and the checksum
 * gates that too.
 *
 * Pacing (see main loop): a packet is sent every 16th frame until a peer
 * packet has been received within the last REMOTE_TIMEOUT_FRAMES, then one
 * per frame (300 B/s at 50 Hz — well under 9600 baud).  Both sides beacon,
 * so pairing needs no role asymmetry. */

#define SERIAL_PKT_LEN 6

/* progress is 13 bits on the wire; the course length must fit. */
_Static_assert((LAP_LENGTH >> 2) < (1 << 13),
               "progress field is 13 bits on the wire");

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
    uint16_t x = (uint16_t)(rs->cam_x + 8192) & 0x3FFF;   /* 14 bits */
    uint16_t p = (uint16_t)(rs->progress >> 2) & 0x1FFF;  /* 13 bits */
    uint8_t  s;
    buf[0] = (uint8_t)((rs->state & 3) | (rs->fire ? 4 : 0) | (rs->kill ? 8 : 0)
                       | (rs->finished ? 16 : 0) | (rs->race_parity ? 32 : 0)
                       | (rs->mine ? 64 : 0));
    buf[1] = (uint8_t)((x >> 7) & 0x7F);
    buf[2] = (uint8_t)(x & 0x7F);
    buf[3] = (uint8_t)(((rs->lap - 1) & 7) | (((p >> 9) & 0x0F) << 3));
    buf[4] = (uint8_t)((p >> 2) & 0x7F);
    buf[5] = (uint8_t)((p & 3) << 5);
    s = (uint8_t)(buf[0] + buf[1] + buf[2] + buf[3] + buf[4] + buf[5]);
    buf[5] |= (uint8_t)(s & 0x1F);
    buf[0] |= 0x80;                       /* marker LAST: excluded from the sum */
}

/* Byte-at-a-time deframer.  Feed every received byte; returns true exactly
 * when a complete packet has been decoded into *out. */
typedef struct {
    uint8_t buf[SERIAL_PKT_LEN];
    uint8_t n;      /* 0 = awaiting a marker; 1..LEN-1 = collecting */
} SerialFramer;

#define SERIAL_FRAMER_INIT { {0}, 0 }

/* Wire-error counters, for the debug overlay (press D).  They exist because
 * the two failure modes look identical from the game side — a stuttering
 * ghost — but have opposite fixes, and neither ever happens under hatari
 * (the emulator's rs232 is a byte pipe: no baud timing, no line noise, no
 * MFP receive overrun).  On real hardware:
 *   gSerialShort  — a marker byte arrived mid-packet, i.e. the bytes between
 *                   it and the previous marker were lost.  Rising counts mean
 *                   the receiver is missing bytes: MFP USART overrun (an
 *                   interrupt held off longer than the 1.04 ms byte time at
 *                   9600 baud) or a full TOS iorec.
 *   gSerialBadSum — a full 6-byte frame failed the 5-bit checksum, i.e. bytes
 *                   arrived but were corrupted.  Rising counts mean the line
 *                   itself is bad: cable, grounding, or length.
 * Both zero while the ghost stutters means the link is clean and the peer is
 * simply sending slower than we render — that is what the dead reckoning in
 * race_update() covers. */
static uint16_t gSerialShort  __attribute__((unused));
static uint16_t gSerialBadSum __attribute__((unused));

static inline bool serial_unframe(SerialFramer *f, uint8_t b, RemoteState *out)
{
    uint8_t s; uint16_t x, p;
    if (b & 0x80) {
        if (unlikely(f->n)) gSerialShort++;       /* truncated: bytes lost */
        f->buf[0] = (uint8_t)(b & 0x7F); f->n = 1; return false;
    }
    if (f->n == 0) return false;                  /* not synced: drop */
    f->buf[f->n++] = b;
    if (f->n < SERIAL_PKT_LEN) return false;
    f->n = 0;                                     /* await next marker */
    s = (uint8_t)(f->buf[0] + f->buf[1] + f->buf[2] + f->buf[3] + f->buf[4]
                  + (f->buf[5] & 0x60));
    if (unlikely((s & 0x1F) != (f->buf[5] & 0x1F))) { gSerialBadSum++; return false; }
    out->state       = (uint8_t)(f->buf[0] & 3);
    out->fire        = (f->buf[0] & 4)  != 0;
    out->kill        = (f->buf[0] & 8)  != 0;
    out->finished    = (f->buf[0] & 16) != 0;
    out->race_parity = (uint8_t)((f->buf[0] >> 5) & 1);
    out->mine        = (f->buf[0] & 64) != 0;
    x = (uint16_t)(((uint16_t)f->buf[1] << 7) | f->buf[2]);
    out->cam_x       = (int16_t)(x - 8192);
    out->lap         = (uint8_t)((f->buf[3] & 7) + 1);
    /* Clamp to the course length: a corrupt-but-framed packet could otherwise
     * carry progress > 32767, which consumers' (int16_t)progress reads as
     * negative and flips the ghost's relative depth. */
    p = (uint16_t)((((uint16_t)(f->buf[3] >> 3) & 0x0F) << 9)
                 | ((uint16_t)f->buf[4] << 2) | ((f->buf[5] >> 5) & 3));
    out->progress    = progress_clamp((uint16_t)(p << 2));
    return true;
}

#endif /* SERIAL_H */
