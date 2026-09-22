/*
 * ASCII debug backend — prints structured frame data to stdout.
 * Pure standard C: compiles for both Linux (gcc) and Atari (m68k-atari-mint-gcc).
 * serial_* and platform_frame_pace() come from the platform-common file the
 * unity wrapper includes first (posix_serial.c or atari_serial.c).
 *
 * The backend has no input device, so it autopilots: FIRE is always held —
 * it skips the intro, releases every gate (once dwell/handshake allow), and
 * fires missiles in cruise.  UP is deliberately NOT held: there is no vertical
 * control anymore, and holding it would pin max throttle, so the bot (which
 * rerolls its throttle, avg ~80 vs our constant 64) could never pull ahead —
 * the ghost/kill tests depend on the bot being able to lead.
 * On POSIX, VQ_FRAME_MS=<ms> paces frames (see posix_serial.c).
 * VQ_AUTOPILOT_OFFGRID=1 additionally holds RIGHT+UP, steering hard off the
 * playable grid — the off-grid anti-cheat regression check in test_race.sh
 * (Part 1e) is the only user; getenv() is a no-op on a real TOS run with no
 * environment, so this is inert there.
 * VQ_DEBUG_OVERLAY=1 latches the debug overlay on (see backend_get_keys),
 * so DBG lines below carry internal state (cam_zspeed etc.) the same Part 1e
 * check reads to verify the off-grid clamp directly instead of inferring it
 * from race-completion timing.
 *
 * Output format (one block per frame that cleared or drew anything —
 * pacing-only presents are counted but not printed; DBG = one debug-overlay
 * label/value pair (only present with the overlay latched on); ALINE =
 * alien-plane line, where aliens, missiles and the remote race player are
 * drawn; RLINE = the yellow tail slice's extra plane-0 copy (logo caption,
 * mines, ghost, peer missiles — the same lines also appear as ALINEs):
 *
 *   FRAME 42
 *   ANGLES angleY=512 angleX=321
 *   DBG Z 128
 *   ...
 *   LINES 288
 *   BBOX x=12..308 y=45..155
 *   LINE 160,100 200,120
 *   ...
 *   ALINES 9
 *   ALINE 160,90 150,110
 *   ...
 *   RLINES 3
 *   RLINE 160,95 157,105
 *   ...
 *   END_FRAME
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "backend.h"

static int     gFrameCount;
static bool    gFrameDirty;   /* cleared/drawn since the last printed block */
static int16_t gFrameAngleY;
static int16_t gFrameAngleX;
static int     gLineCount;
static Line    gAsciiLines[MAX_DRAW_LINES];
static int     gALineCount;
static Line    gAsciiALines[MAX_DRAW_LINES];
static int     gRLineCount;
static Line    gAsciiRLines[MAX_DRAW_LINES];

/* Debug-overlay items (dbg_item, vquest.c) are emitted mid-frame, before
 * backend_present() prints this frame's FRAME header — buffer them like the
 * line lists above and flush in present order, or they'd print attached to
 * the previous frame's block. */
#define MAX_DEBUG_ITEMS 16
typedef struct { char label; int16_t val; } DebugItem;
static int       gDebugItemCount;
static DebugItem gDebugItems[MAX_DEBUG_ITEMS];

/* bounding box of rendered lines */
static int16_t gBboxMinX, gBboxMaxX, gBboxMinY, gBboxMaxY;

static uint8_t gAutopilotExtraKeys;   /* see VQ_AUTOPILOT_OFFGRID above */
static bool    gDebugOverlayPulse;    /* see VQ_DEBUG_OVERLAY below */

void backend_init(void) {
    gFrameCount = 0;
    gAutopilotExtraKeys = getenv("VQ_AUTOPILOT_OFFGRID") ? (KEY_RIGHT | KEY_UP) : 0;
    gDebugOverlayPulse = getenv("VQ_DEBUG_OVERLAY") != NULL;
    printf("BACKEND=ascii\n");
    fflush(stdout);
    stars_init();
}

void backend_draw_star(uint16_t x __attribute__((unused)),
                       uint16_t y __attribute__((unused))) {}

void backend_hud_begin(void) {}
void backend_hud_line(int16_t x0 __attribute__((unused)), int16_t y0 __attribute__((unused)),
                      int16_t x1 __attribute__((unused)), int16_t y1 __attribute__((unused))) {}
void backend_hud_clear_rect(int16_t x __attribute__((unused)), int16_t y __attribute__((unused)),
                            int16_t w __attribute__((unused)), int16_t h __attribute__((unused))) {}

void backend_hud_note(const char *tag) { printf("LINK %s\n", tag); fflush(stdout); }

void backend_debug_item(char label, int16_t val) {
    if (gDebugItemCount < MAX_DEBUG_ITEMS)
        gDebugItems[gDebugItemCount++] = (DebugItem){ label, val };
}

void backend_clear(void) {
    gFrameDirty = true;
    gLineCount  = 0;
    gALineCount = 0;
    gRLineCount = 0;
    gDebugItemCount = 0;
    gBboxMinX   =  32767;
    gBboxMaxX   = -32767;
    gBboxMinY   =  32767;
    gBboxMaxY   = -32767;
}

static int16_t min_s(int16_t a, int16_t b) { return a < b ? a : b; }
static int16_t max_s(int16_t a, int16_t b) { return a > b ? a : b; }

void backend_draw_lines(Line *lines, int count) {
    int i;
    gFrameDirty = true;
    for (i = 0; i < count && gLineCount < MAX_DRAW_LINES; ++i) {
        gAsciiLines[gLineCount++] = lines[i];
        gBboxMinX = min_s(gBboxMinX, min_s(lines[i].p0.x, lines[i].p1.x));
        gBboxMaxX = max_s(gBboxMaxX, max_s(lines[i].p0.x, lines[i].p1.x));
        gBboxMinY = min_s(gBboxMinY, min_s(lines[i].p0.y, lines[i].p1.y));
        gBboxMaxY = max_s(gBboxMaxY, max_s(lines[i].p0.y, lines[i].p1.y));
    }
}

void backend_draw_alien_lines(Line *lines, int count) {
    int i;
    gFrameDirty = true;
    for (i = 0; i < count && gALineCount < MAX_DRAW_LINES; ++i)
        gAsciiALines[gALineCount++] = lines[i];
}

void backend_draw_remote_lines(Line *lines, int count, bool bot __attribute__((unused))) {
    int i;
    gFrameDirty = true;
    for (i = 0; i < count && gRLineCount < MAX_DRAW_LINES; ++i)
        gAsciiRLines[gRLineCount++] = lines[i];
}

void backend_present(int16_t angleY, int16_t angleX) {
    int i;

    /* Pacing-only present (intro glow animation, press-fire wait): nothing
     * was cleared or drawn, the block would be a byte-for-byte repeat of the
     * previous one.  Skipping it matters on TOS: the intro alone is ~110
     * such presents × ~8 KB of credits re-print — 98% of the bytes pushed
     * through hatari's emulated VT52 console (32 KB screen scroll per line)
     * during a test run.  The frame counter still advances so printed FRAME
     * numbers are unchanged. */
    if (!gFrameDirty) {
        ++gFrameCount;
        platform_frame_pace();
        return;
    }
    gFrameDirty  = false;
    gFrameAngleY = angleY;
    gFrameAngleX = angleX;

    printf("FRAME %d\n", gFrameCount);
    printf("ANGLES angleY=%d angleX=%d\n", (int)gFrameAngleY, (int)gFrameAngleX);
    for (i = 0; i < gDebugItemCount; ++i)
        printf("DBG %c %d\n", gDebugItems[i].label, (int)gDebugItems[i].val);
    printf("LINES %d\n", gLineCount);
    if (gLineCount > 0)
        printf("BBOX x=%d..%d y=%d..%d\n",
               (int)gBboxMinX, (int)gBboxMaxX,
               (int)gBboxMinY, (int)gBboxMaxY);
    for (i = 0; i < gLineCount; ++i)
        printf("LINE %d,%d %d,%d\n",
               (int)gAsciiLines[i].p0.x, (int)gAsciiLines[i].p0.y,
               (int)gAsciiLines[i].p1.x, (int)gAsciiLines[i].p1.y);
    printf("ALINES %d\n", gALineCount);
    for (i = 0; i < gALineCount; ++i)
        printf("ALINE %d,%d %d,%d\n",
               (int)gAsciiALines[i].p0.x, (int)gAsciiALines[i].p0.y,
               (int)gAsciiALines[i].p1.x, (int)gAsciiALines[i].p1.y);
    printf("RLINES %d\n", gRLineCount);
    for (i = 0; i < gRLineCount; ++i)
        printf("RLINE %d,%d %d,%d\n",
               (int)gAsciiRLines[i].p0.x, (int)gAsciiRLines[i].p0.y,
               (int)gAsciiRLines[i].p1.x, (int)gAsciiRLines[i].p1.y);
    printf("END_FRAME\n");
    fflush(stdout);

    ++gFrameCount;
    platform_frame_pace();
}

void backend_cleanup(void) {
    printf("DONE frames=%d\n", gFrameCount);
    fflush(stdout);
}

/* No input device: autopilot.  FIRE skips the intro, releases every gate,
 * and fires missiles in cruise; UP is deliberately NOT held — see the
 * file-header comment (the bot must be able to out-pace a constant-throttle
 * autopilot for the ghost/kill tests to see a leader).
 *
 * VQ_DEBUG_OVERLAY=1 pulses KEY_DEBUG on the very first frame to latch the
 * debug overlay on (main's KEY_DEBUG handling is a rising-edge toggle, so one
 * frame is enough) — used by test_race.sh to read cam_zspeed etc. via the
 * "DBG" lines backend_debug_item() emits, instead of inferring internal
 * state from race-completion timing. */
uint8_t backend_get_keys(void) {
    uint8_t keys = (uint8_t)(KEY_FIRE | gAutopilotExtraKeys);
    /* The intro's key-poll (vquest.c, skip-on-any-key) calls this at most
     * once before the main loop starts, and doesn't act on KEY_DEBUG at all
     * — only the main loop's rising-edge check does. So the pulse has to
     * land on a call the main loop actually sees, not the intro's; waiting
     * for the 2nd call guarantees that regardless of whether the intro
     * consumed the 1st. */
    if (gDebugOverlayPulse) {
        static uint8_t calls;
        if (++calls == 2) { keys = (uint8_t)(keys | KEY_DEBUG); gDebugOverlayPulse = false; }
    }
    return keys;
}
void    backend_set_flash(int on __attribute__((unused))) {}

uint16_t backend_snd_switch(int slot) { (void)slot; return 0; }
void backend_snd_sfx(int slot)    { (void)slot; }
