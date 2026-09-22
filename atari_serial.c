/* ── Serial (RS-232 race mode, Atari: TOS AUX device) ─────────────────────────
 *
 * Platform-common serial for Atari builds; unity-included by main_gemtos.c
 * and main_ascii_tos.c before the backend.  Kept out of atari_common.h so the
 * loader does not carry it.
 *
 * AUX (BIOS device 1) is the MFP USART; TOS drives it interrupt-driven with
 * 256-byte iorec buffers, so Bconout just queues bytes and traps put us in
 * supervisor mode for free (the MFP registers bus-error from user mode).
 * The send/recv path arguments are unused: the link is the physical port.
 */

#include <osbind.h>
#include "serial.h"

#define SERIAL_DEV 1   /* BIOS device: AUX (RS-232) */

static uint8_t      gSerialSavedUCR;
static SerialFramer gFramer = SERIAL_FRAMER_INIT;

void serial_init(const char *send_path __attribute__((unused)),
                 const char *recv_path __attribute__((unused)))
{
    /* Baud code 1 = 9600; UCR 0x88: 8 data bits, 1 stop bit, no parity,
     * ÷16 async; flow 0 = none.  Rsconf returns old ucr:rsr:tsr:scr MSB→LSB. */
    int i;
    gSerialSavedUCR = (uint8_t)((uint32_t)Rsconf(1, 0, 0x88, -1, -1, -1) >> 24);
    /* Drop whatever TOS has already buffered.  On real hardware the peer
     * starts beaconing the moment their machine boots, so by the time this
     * one finishes loading, the 256-byte AUX iorec is full of packets that
     * are seconds to minutes old — plus whatever noise the port picked up
     * while the cable was being plugged in.  Without the flush the first
     * serial_recv() decodes an ancient packet, reports the peer live at a
     * stale lap/progress and starts the gate handshake from a lie.  Hatari
     * never shows this: its rs232 input starts empty and both emulated
     * machines are launched together.  Bounded so a stuck Bconstat cannot
     * hang the boot; a live peer only feeds 300 B/s, far slower than the
     * trap-per-byte drain, so the loop always empties the buffer first. */
    for (i = 0; i < 512 && Bconstat(SERIAL_DEV); i++)
        (void)Bconin(SERIAL_DEV);
    gFramer.n = 0;
}

void serial_cleanup(void)
{
    Rsconf(-1, 0, gSerialSavedUCR, -1, -1, -1);   /* baud left at 9600 (TOS default) */
}

void serial_send(const RemoteState *rs)
{
    uint8_t buf[SERIAL_PKT_LEN];
    int i;
    serial_pack(rs, buf);
    for (i = 0; i < SERIAL_PKT_LEN; i++)
        Bconout(SERIAL_DEV, buf[i]);
}

bool serial_recv(RemoteState *out)
{
    bool got = false, fire = false, kill = false, mine = false;
    /* Drain everything pending so a backlog can't add ghost latency. */
    while (Bconstat(SERIAL_DEV)) {
        if (serial_unframe(&gFramer, (uint8_t)Bconin(SERIAL_DEV), out)) {
            fire |= out->fire;
            kill |= out->kill;
            mine |= out->mine;
            got = true;
        }
    }
    if (got) { out->fire = fire; out->kill = kill; out->mine = mine; }
    return got;
}

/* Frame pacing is a host-side concern (hatari emulates real time); no-op. */
static inline void platform_frame_pace(void) {}
