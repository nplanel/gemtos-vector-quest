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

static SerialFramer gFramer = SERIAL_FRAMER_INIT;

void serial_init(const char *send_path, const char *recv_path)
{
    if (send_path) gSendFd = open(send_path, O_RDWR | O_NONBLOCK);
    if (recv_path) gRecvFd = open(recv_path, O_RDWR | O_NONBLOCK);
    gFramer.n = 0xFF;
}

void serial_cleanup(void)
{
    if (gSendFd >= 0) { close(gSendFd); gSendFd = -1; }
    if (gRecvFd >= 0) { close(gRecvFd); gRecvFd = -1; }
}

void serial_send(const RemoteState *rs)
{
    uint8_t buf[SERIAL_PKT_LEN];
    if (gSendFd < 0) return;
    serial_pack(rs, buf);
    (void)write(gSendFd, buf, sizeof(buf));   /* EAGAIN/EPIPE silently dropped */
}

bool serial_recv(RemoteState *out)
{
    uint8_t buf[32];
    int n, i;
    bool got = false, fire = false, kill = false;
    if (gRecvFd < 0) return false;
    /* Drain everything available so a backlog can't add ghost latency. */
    while ((n = (int)read(gRecvFd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < n; i++)
            if (serial_unframe(&gFramer, buf[i], out)) {
                fire |= out->fire;
                kill |= out->kill;
                got = true;
            }
        if (n < (int)sizeof(buf)) break;
    }
    if (got) { out->fire = fire; out->kill = kill; }
    return got;
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
