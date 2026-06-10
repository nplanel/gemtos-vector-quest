/* ── Serial (RS-232 race mode, POSIX: named-pipe or regular-file pair) ────────
 *
 * Platform-common serial for Linux builds; unity-included by main_sdl.c and
 * main_ascii.c before the backend.  Paths come from argv (vquest.c main):
 * named pipes for live play, regular files for deterministic tests.
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include "serial.h"

static int gSendFd = -1;
static int gRecvFd = -1;

typedef enum { RECV_SYNC, RECV_HI, RECV_LO } RecvState;
static RecvState gRecvState = RECV_SYNC;
static uint8_t   gRecvHiByte;

void serial_init(const char *send_path, const char *recv_path)
{
    if (send_path) gSendFd = open(send_path, O_RDWR | O_NONBLOCK);
    if (recv_path) gRecvFd = open(recv_path, O_RDWR | O_NONBLOCK);
    gRecvState = RECV_SYNC;
}

void serial_cleanup(void)
{
    if (gSendFd >= 0) { close(gSendFd); gSendFd = -1; }
    if (gRecvFd >= 0) { close(gRecvFd); gRecvFd = -1; }
}

void serial_send(int16_t cam_x)
{
    uint8_t buf[3];
    if (gSendFd < 0) return;
    buf[0] = 0xAA;
    buf[1] = (uint8_t)((uint16_t)cam_x >> 8);
    buf[2] = (uint8_t)cam_x;
    (void)write(gSendFd, buf, 3);   /* EAGAIN/EPIPE silently dropped */
}

bool serial_recv(int16_t *cam_x)
{
    uint8_t buf[16];
    int n, i;
    if (gRecvFd < 0) return false;
    n = (int)read(gRecvFd, buf, sizeof(buf));
    if (n <= 0) return false;
    for (i = 0; i < n; i++) {
        uint8_t b = buf[i];
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

/* Optional pacing for free-running text backends: VQ_FRAME_MS=<ms> sleeps that
 * long each frame so two live instances stay near lockstep.  Unset = full
 * speed (deterministic tests). */
static inline void platform_frame_pace(void)
{
    static long us = -1;
    if (us < 0) {
        const char *ms = getenv("VQ_FRAME_MS");
        us = ms ? atol(ms) * 1000 : 0;
    }
    if (us > 0) usleep((useconds_t)us);
}
