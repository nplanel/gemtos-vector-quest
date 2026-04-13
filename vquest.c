#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#include "backend.h"
#include "vquest.h"        /* Point3DFloat, Point3DInt, Point3DLong */
#include "vquest_model.h"  /* vquest_vertices[], vquest_edges[] */

/* Perspective/Model Constants */
#define LOGO_SCALE          (3.0f/230.0f)

#define FP_SHIFT 10
#define FP_ONE (1 << FP_SHIFT)

#define LUT_SIZE 2048

static int16_t *sinLUT;
static int16_t *cosLUT;

/* LUT built incrementally during the intro animation to hide the cost.
 *
 * Quarter-wave symmetry: only sinLUT[0..LUT_SIZE/4] is computed via the
 * floating-point recurrence (LUT_SIZE/4+1 = 513 entries, 4× fewer double
 * multiplications than filling all 2048 directly).  The remaining three
 * quarters are derived by mirror/negate, and cosLUT is a quarter-shifted
 * copy of sinLUT — all cheap integer memory operations.
 *
 *   sin[LUT_SIZE/2 - i] =  sin[i]          (mirror around π/2)
 *   sin[LUT_SIZE/2 + i] = -sin[i]          (half-wave antisymmetry)
 *   cos[i]              =  sin[(i + LUT_SIZE/4) & (LUT_SIZE-1)]
 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static double   lut_s = 0.0, lut_c = 1.0;  /* running sin/cos(i * 2π/LUT_SIZE) */
static double   lut_ds, lut_dc;             /* sin/cos of one angular step       */
static uint16_t lut_idx = 0;               /* next sinLUT entry to fill (0..LUT_SIZE/4+1) */

static void lut_init(void) {
    sinLUT = malloc(LUT_SIZE * sizeof(int16_t));
    cosLUT = malloc(LUT_SIZE * sizeof(int16_t));
    lut_ds = sinf(2.0 * M_PI / LUT_SIZE);
    lut_dc = cosf(2.0 * M_PI / LUT_SIZE);
}

static void lut_fill_chunk(uint16_t n) {
    if (lut_idx >= LUT_SIZE) return;
    /* Each recurrence step fills up to 8 entries across all 4 quarters of
     * both LUTs simultaneously — no separate bulk-copy phase. */
    while (n-- && lut_idx <= LUT_SIZE / 4) {
        uint16_t i = lut_idx;
        int16_t vs = (int16_t)(lut_s * FP_ONE + 0.5f);
        int16_t vc = (int16_t)(lut_c * FP_ONE + 0.5f);
        sinLUT[i]                =  vs;
        sinLUT[LUT_SIZE / 2 + i] = (int16_t)(-vs);
        cosLUT[i]                =  vc;
        cosLUT[LUT_SIZE / 2 + i] = (int16_t)(-vc);
        if (i > 0 && i < LUT_SIZE / 4) {
            sinLUT[LUT_SIZE / 2 - i] =  vs;
            sinLUT[LUT_SIZE - i]      = (int16_t)(-vs);
            cosLUT[LUT_SIZE / 2 - i] = (int16_t)(-vc);
            cosLUT[LUT_SIZE - i]      =  vc;
        }
        double ns = lut_s * lut_dc + lut_c * lut_ds;
        lut_c     = lut_c * lut_dc - lut_s * lut_ds;
        lut_s     = ns;
        lut_idx++;
    }
    if (lut_idx > LUT_SIZE / 4) lut_idx = LUT_SIZE;
}

static inline int16_t fastSin(int16_t angle) {
    return sinLUT[angle & (LUT_SIZE-1)];
}

static inline int16_t fastCos(int16_t angle) {
    return cosLUT[angle & (LUT_SIZE-1)];
}

/* Fixed-point multiply: (a * b) >> FP_SHIFT.
 * GCC m68k emits muls.w (~70 cycles) + asr.l #10.
 * Exact integer result — no LUT quantization error. */
static inline int16_t mul_fp(int16_t a, int16_t b) {
    return (int16_t)(((int32_t)a * b) >> FP_SHIFT);
}

#include "render.c"    /* 3-D rendering pipeline (unity include) */
#include "physics.c"   /* game logic and state machine (unity include) */

int main(int argc, char *argv[]) {
    int16_t angleY = 0, angleX = 0;
    int16_t angleYinc, angleXinc;
    PhysicsState ps = { CAM_Y_INIT, CAM_X_INIT, 0, 0 };
    int16_t z_phase = 0;
    uint16_t frame         = 0;
    uint16_t min_frame     = 0;
    uint16_t max_frame     = 0;   /* 0 = no limit (replaces -1 sentinel) */
    int16_t crash_timer   = 0;
    int16_t strip_dist    = 0;   /* z-distance to landing strip; counts down each frame */
    int16_t strip_x       = 0;   /* lateral world position of landing strip             */
    int16_t alien_x[ALIEN_COUNT]       = {0};
    int16_t alien_z[ALIEN_COUNT]       = {0};
    bool    alien_alive[ALIEN_COUNT]   = {0};
    int16_t missile_x[MISSILE_COUNT]   = {0};
    int16_t missile_z[MISSILE_COUNT]   = {0};
    bool    missile_alive[MISSILE_COUNT] = {0};
    int16_t cam_zspeed    = CAM_ZSPEED_BASE;
    int16_t round         = 1;
    int16_t takeoff_timer = TAKEOFF_FRAMES_BASE;
    GameState state       = STATE_TAKEOFF;

    if (argc >= 2) min_frame = (uint16_t)atoi(argv[1]);
    if (argc >= 3) max_frame = (uint16_t)atoi(argv[2]);

    gLines = malloc((MAX_DRAW_LINES + 1) * sizeof(Line));
    lut_init();
    backend_init();
    strip_x = next_strip_x(round, frame);


    /* Intro: reveal title + subtitle one letter at a time; any key skips.
     *
     * Credits live on the grid plane (plane 0, blue).  On Atari the display is
     * double-buffered: we draw credits to the current drawing buffer, present
     * (swap), then draw again so both physical buffers carry the credits.
     * After that the intro loop just calls backend_present() — no backend_clear()
     * — so plane 0 is never wiped and the credits persist without any redraw.
     * HUD letters (plane 2, green) use backend_hud_line() which draws directly
     * into both buffers on Atari and draws immediately on SDL, so they too
     * accumulate without a clear cycle.
     * Credits auto-disappear when the main game loop's first backend_clear()
     * wipes plane 0+1 (happens on the first game frame). */
#define INTRO_LETTER_FRAMES 6
#define INTRO_NSTEPS (HUD_NCHARS > HUD_NSUB_CHARS ? HUD_NCHARS : HUD_NSUB_CHARS)
#define LUT_CHUNK 5   /* recurrence steps per intro frame: ceil((LUT_SIZE/4+1) / (INTRO_NSTEPS*INTRO_LETTER_FRAMES)) */
    {
        int8_t k = 0;

        /* Draw credits into plane 0 of both buffers. */
        gNLines = 0;
        credits_render();
        memset(&gLines[gNLines], 0, sizeof(Line));   /* sentinel for SegmentedMultiLine */
        backend_clear();
        backend_draw_lines(gLines, gNLines);
        backend_present(0, 0);                       /* swap: other buffer is now drawing */
        backend_draw_lines(gLines, gNLines);         /* same credits into the other buffer */

        hud_begin();
        while (k < INTRO_NSTEPS) {
            int8_t j;
            int drew = 0;
            if (k < HUD_NCHARS)     drew |= hud_draw_letter(k);
            if (k < HUD_NSUB_CHARS) drew |= hud_draw_subletter(k);
            k++;
            if (!drew) continue;    /* both are spaces: skip pause */
            for (j = 0; j < INTRO_LETTER_FRAMES; j++) {
                lut_fill_chunk(LUT_CHUNK);
                backend_present(0, 0);
            }
        }
        hud_draw_tally(1);
    }
    lut_fill_chunk(LUT_SIZE);   /* finish if intro was skipped or had many spaces */

    model_init();
    build_grid();

    /* Convert rad/frame speeds to LUT-index increments */
    angleYinc = (int16_t)(0.08 * FP_ONE);
    angleXinc = (int16_t)(0.13 * FP_ONE);
    angleYinc = (int16_t)((int32_t)angleYinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));
    angleXinc = (int16_t)((int32_t)angleXinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));

    for (;;) {
        uint8_t keys = backend_get_keys();
        if (keys & KEY_QUIT) break;
        if (max_frame != 0 && frame > max_frame) break;

        /* Grid always scrolls */
        z_phase = (int16_t)(z_phase + cam_zspeed);
        if (z_phase >= GRID_ZSTEP) z_phase = (int16_t)(z_phase - GRID_ZSTEP);

        bool flash = false;
        const RenderFlags *rf = &kStateFlags[state];

        switch (state) {
        case STATE_TAKEOFF:
            state = state_takeoff(&takeoff_timer, &ps,
                                  &strip_dist, &crash_timer, round,
                                  alien_x, alien_z, alien_alive, missile_alive, keys);
            break;
        case STATE_CRUISE:
            state = state_cruise(&strip_dist, &ps, cam_zspeed,
                                 alien_z, alien_alive,
                                 missile_x, missile_z, missile_alive, keys);
            break;
        case STATE_LANDING:
            state = state_landing(&ps,
                                  &strip_dist, &strip_x, &round, &cam_zspeed,
                                  &crash_timer, alien_z, alien_alive, keys, frame);
            break;
        case STATE_CRASH:
            state = state_crash(&flash, &crash_timer, &round,
                                &takeoff_timer, &cam_zspeed, &strip_x,
                                &ps, frame);
            break;
        case STATE_SUCCESS:
            state = state_success(&angleY, &angleX, &crash_timer,
                                  &ps,
                                  &takeoff_timer, angleYinc, angleXinc, keys);
            break;
        }

        /* Alien contact: live alien crossing z=0 within FP_ONE laterally → crash */
        if ((state == STATE_CRUISE || state == STATE_LANDING) &&
            alien_hit_player(ps.cam_x, cam_zspeed, alien_x, alien_z, alien_alive)) {
            crash_timer = CRASH_FLASH_FRAMES;
            state = STATE_CRASH;
        }

        /* Advance missiles every frame so they expire at the horizon regardless of state */
        update_missiles(cam_zspeed, missile_x, missile_z, missile_alive,
                        alien_x,   alien_z,   alien_alive);

        /* Safety clamp: allow wider roam during cruise; grid uses cam_x only for
         * horizontal lines (world x fixed) so ±6 is safe.  Strip rendering guards
         * cam_x_rel separately below (strip can be up to STRIP_X_MAX away). */
        if (ps.cam_x >  6 * FP_ONE) ps.cam_x =  (int16_t)(6 * FP_ONE);
        if (ps.cam_x < -6 * FP_ONE) ps.cam_x = -(int16_t)(6 * FP_ONE);

        backend_set_flash(flash);
        frame++;
        if (frame < min_frame) continue;
        backend_clear();
        draw_world_plane(rf, ps.cam_y, z_phase, ps.cam_x,
                         strip_dist, strip_x);
        draw_alien_plane(rf->logo, angleY, angleX,
                         rf->aliens, alien_x, alien_z, alien_alive,
                         missile_x, missile_z, missile_alive, ps.cam_x,
                         rf->takeoff_strip, rf->landing_strip,
                         takeoff_timer, strip_dist, strip_x, ps.cam_y, z_phase,
                         cam_zspeed);
        backend_present(angleY, angleX);
    }

    backend_cleanup();
    return 0;
}
