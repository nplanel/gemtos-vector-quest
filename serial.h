#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stdbool.h>

/* Platform-agnostic RS-232 serial interface for race mode.
 * Implemented in atari_serial.c (TOS AUX device) and posix_serial.c (named
 * pipes or regular files); benchmark/test backends may stub it instead.
 *
 * Wire protocol: 3-byte framed packet per frame:
 *   [ 0xAA | cam_x_hi | cam_x_lo ]
 * 0xAA is safe as sync: cam_x is clamped to ±6144 (±0x1800),
 * so its high byte is always in [0x00..0x18] or [0xE8..0xFF], never 0xAA. */

void  serial_init(const char *send_path, const char *recv_path);
void  serial_cleanup(void);
void  serial_send(int16_t cam_x);   /* non-blocking best-effort */
bool  serial_recv(int16_t *cam_x);  /* true if a new value was received this call */

#endif /* SERIAL_H */
