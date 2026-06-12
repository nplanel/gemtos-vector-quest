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
    gSerialSavedUCR = (uint8_t)((uint32_t)Rsconf(1, 0, 0x88, -1, -1, -1) >> 24);
    gFramer.n = 0xFF;
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
    bool got = false, fire = false, kill = false;
    /* Drain everything pending so a backlog can't add ghost latency. */
    while (Bconstat(SERIAL_DEV)) {
        if (serial_unframe(&gFramer, (uint8_t)Bconin(SERIAL_DEV), out)) {
            fire |= out->fire;
            kill |= out->kill;
            got = true;
        }
    }
    if (got) { out->fire = fire; out->kill = kill; }
    return got;
}

/* Frame pacing is a host-side concern (hatari emulates real time); no-op. */
static inline void platform_frame_pace(void) {}
