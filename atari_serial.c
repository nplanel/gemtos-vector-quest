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
 *
 * Receive bypasses the BIOS: the RX interrupt only ever advances the input
 * iorec's tail and Bconin only its head, so reading the ring directly is the
 * same single-producer/single-consumer protocol without a Bconstat+Bconin
 * trap pair per byte (13 traps for one packet).  The iorec lives in TOS RAM
 * above $800, readable from user mode.  Valid because flow control is off
 * (Rsconf below): with RTS/CTS or XON/XOFF, Bconin also re-opens the flow at
 * the low-water mark, which this path would skip.
 */

#include <osbind.h>
#include "serial.h"

#define SERIAL_DEV 1   /* BIOS device: AUX (RS-232) */

static uint8_t      gSerialSavedUCR;
static _IOREC      *gAuxIn;    /* RS-232 input ring (Iorec(IO_SERIAL)) */
static SerialFramer gFramer = SERIAL_FRAMER_INIT;

void serial_init(const char *send_path __attribute__((unused)),
                 const char *recv_path __attribute__((unused)))
{
    /* Baud code 1 = 9600; UCR 0x88: 8 data bits, 1 stop bit, no parity,
     * ÷16 async; flow 0 = none.  Rsconf returns old ucr:rsr:tsr:scr MSB→LSB. */
    gSerialSavedUCR = (uint8_t)((uint32_t)Rsconf(1, 0, 0x88, -1, -1, -1) >> 24);
    gAuxIn = (_IOREC *)Iorec(IO_SERIAL);
    serial_flush();
}

void serial_flush(void)
{
    /* Drop whatever TOS has already buffered.  On real hardware the peer
     * starts beaconing the moment their machine boots, so by the time this
     * one finishes loading, the 256-byte AUX iorec is full of packets that
     * are seconds to minutes old — plus whatever noise the port picked up
     * while the cable was being plugged in.  Without the flush the first
     * serial_recv() decodes an ancient packet, reports the peer live at a
     * stale lap/progress and starts the gate handshake from a lie.  Hatari
     * never shows this: its rs232 input starts empty and both emulated
     * machines are launched together.  Also called when switching to 2
     * players: in 1-player mode nothing reads the port, so the same
     * staleness builds up mid-session.  Head = tail is one word store, so it
     * cannot race the RX interrupt (which only writes the tail). */
    gAuxIn->ibufhd = gAuxIn->ibuftl;
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
    _IOREC  *io = gAuxIn;
    int16_t  hd = io->ibufhd;
    bool got = false, fire = false, kill = false, mine = false;
    /* Drain everything pending so a backlog can't add ghost latency.  Same
     * index step as TOS's Bconin: head names the last byte consumed. */
    while (hd != io->ibuftl) {
        if (++hd >= io->ibufsiz) hd = 0;
        if (serial_unframe(&gFramer, (uint8_t)io->ibuf[hd], out)) {
            fire |= out->fire;
            kill |= out->kill;
            mine |= out->mine;
            got = true;
        }
    }
    io->ibufhd = hd;   /* release the consumed bytes to the RX interrupt */
    if (got) { out->fire = fire; out->kill = kill; out->mine = mine; }
    return got;
}

/* Frame pacing is a host-side concern (hatari emulates real time); no-op. */
static inline void platform_frame_pace(void) {}
