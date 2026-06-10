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

static uint8_t gSerialSavedUCR;

typedef enum { RECV_SYNC, RECV_HI, RECV_LO } RecvState;
static RecvState gRecvState = RECV_SYNC;
static uint8_t   gRecvHiByte;

void serial_init(const char *send_path __attribute__((unused)),
                 const char *recv_path __attribute__((unused)))
{
    /* Baud code 1 = 9600; UCR 0x88: 8 data bits, 1 stop bit, no parity,
     * ÷16 async; flow 0 = none.  Rsconf returns old ucr:rsr:tsr:scr MSB→LSB. */
    gSerialSavedUCR = (uint8_t)((uint32_t)Rsconf(1, 0, 0x88, -1, -1, -1) >> 24);
    gRecvState = RECV_SYNC;
}

void serial_cleanup(void)
{
    Rsconf(-1, 0, gSerialSavedUCR, -1, -1, -1);   /* baud left at 9600 (TOS default) */
}

void serial_send(int16_t cam_x)
{
    Bconout(SERIAL_DEV, 0xAA);
    Bconout(SERIAL_DEV, (uint8_t)((uint16_t)cam_x >> 8));
    Bconout(SERIAL_DEV, (uint8_t)cam_x);
}

bool serial_recv(int16_t *cam_x)
{
    while (Bconstat(SERIAL_DEV)) {
        uint8_t b = (uint8_t)Bconin(SERIAL_DEV);
        switch (gRecvState) {
        case RECV_SYNC:
            if (b == 0xAA) gRecvState = RECV_HI;
            break;
        case RECV_HI:
            gRecvHiByte = b;
            gRecvState  = RECV_LO;
            break;
        case RECV_LO:
            *cam_x = (int16_t)((uint16_t)gRecvHiByte << 8 | b);
            gRecvState = RECV_SYNC;
            return true;
        }
    }
    return false;
}

/* Frame pacing is a host-side concern (hatari emulates real time); no-op. */
static inline void platform_frame_pace(void) {}
