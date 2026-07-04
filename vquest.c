#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "backend.h"
#include "serial.h"
#include "vquest.h"        /* FP_*, LUT_SIZE, Point3DInt, PhysicsState, … */

#include "gen_tables.h"   /* kSinQuarterNib[], kModelVertsPacked[], kModelEdges[] (generated) */

/* VQ_PERF builds force the ascii backend's autopilot keys (hold Up+Fire) so
 * hatari cycle-headroom runs are deterministic — see perf_frames.sh.  The
 * shipping build is unaffected (PERF_KEYS folds to 0). */
#ifdef VQ_PERF
#define PERF_KEYS ((uint8_t)(KEY_UP | KEY_FIRE))
#else
#define PERF_KEYS ((uint8_t)0)
#endif

/* Full sine table expanded at startup from the baked quarter wave — the
 * quarter is stored as nibble-packed deltas (kSinQuarterNib, two 0..4 deltas
 * per byte, sin[0]=0 implicit; see gen_tables.c) and integrated while
 * expanding by symmetry, all integer-only:
 *
 *   sin[LUT_SIZE/2 - i] =  sin[i]          (mirror around π/2)
 *   sin[LUT_SIZE/2 + i] = -sin[i]          (half-wave antisymmetry)
 *   sin[LUT_SIZE   - i] = -sin[i]
 *
 * No cosine table: cos(a) = sin(a + LUT_SIZE/4), folded into fastCos. */
static int16_t sinLUT[LUT_SIZE];

static void lut_init(void) {
    uint16_t i;
    int16_t  v = 0;
    for (i = 0; i <= LUT_SIZE / 4; i++) {
        if (i) {
            uint8_t b = kSinQuarterNib[(i - 1) >> 1];
            v = (int16_t)(v + (((i - 1) & 1) ? (b >> 4) : (b & 15)));
        }
        sinLUT[i]                              =  v;
        sinLUT[LUT_SIZE / 2 - i]               =  v;
        sinLUT[LUT_SIZE / 2 + i]               = (int16_t)(-v);
        sinLUT[(LUT_SIZE - i) & (LUT_SIZE-1)] = (int16_t)(-v);
    }
}

static inline int16_t fastSin(int16_t angle) {
    return sinLUT[angle & (LUT_SIZE-1)];
}

static inline int16_t fastCos(int16_t angle) {
    return sinLUT[(angle + LUT_SIZE / 4) & (LUT_SIZE-1)];
}

/* Fixed-point multiply: (a * b) >> FP_SHIFT.
 * GCC m68k emits muls.w (~70 cycles) + asr.l #10.
 * Exact integer result — no LUT quantization error. */
static inline int16_t mul_fp(int16_t a, int16_t b) {
    return (int16_t)(((int32_t)a * b) >> FP_SHIFT);
}

#include "render.c"    /* 3-D rendering pipeline (unity include) */
#include "physics.c"   /* game logic and state machine (unity include) */

/* ── Per-frame composition (world plane, then alien plane) ────────────────── */

static inline void draw_world_plane(const RenderFlags *rf, const World *w)
{
    lines_reset();
    render_grid(rf->grid, w->ps.cam_y, w->z_phase, w->ps.cam_x);
    render_arrows(rf->arrows, w->ps.cam_x, w->strip_x, w->strip_dist);
    if (rf->credits) credits_render();
    memset(&gLines[gNLines], 0, sizeof(Line));
    backend_draw_lines(gLines, gNLines);
}

static inline void draw_alien_plane(const RenderFlags *rf, const World *w,
                                    const RaceState *rs)
{
    int16_t cam_x = w->ps.cam_x, cam_y = w->ps.cam_y;
    int i;
    uint16_t remote_start;
    lines_reset();
    render_logo(rf->logo, w->angleY, w->angleX);
    render_takeoff_strip(rf->takeoff_strip, cam_x, cam_y,
                         w->takeoff_timer, w->cam_zspeed);
    render_landing_strip(rf->landing_strip, w->strip_dist, w->strip_x,
                         cam_x, cam_y, w->z_phase);
    if (rf->aliens) {
        for (i = 0; i < ALIEN_COUNT; i++)
            if (w->aliens.alive[i]) draw_alien(w->aliens.x[i], w->aliens.z[i], cam_x);
        for (i = 0; i < MISSILE_COUNT; i++)
            if (w->missiles.alive[i]) draw_missile(w->missiles.x[i], w->missiles.z[i], cam_x);
    }
    /* Remote-player lines (ghost triangle + peer missiles) must stay last in
     * the batch: the tail slice is re-drawn into plane 0 below so its pixels
     * read as index 3 (planes 0+1, yellow) instead of the alien colour.  The
     * shared zero-sentinel terminates both the full batch and the slice. */
    remote_start = gNLines;
    if (rs->ghost_show)
        draw_remote_player(rs->remote.cam_x, rs->ghost_z, rs->remote.alt,
                           cam_x, cam_y);
    if (rf->remote_player)
        for (i = 0; i < MISSILE_COUNT; i++)
            if (rs->rmissiles.alive[i])
                draw_remote_missile(rs->rmissiles.x[i], rs->rmissiles.z[i], cam_x);
    memset(&gLines[gNLines], 0, sizeof(Line));
    backend_draw_alien_lines(gLines, gNLines);
    if (gNLines > remote_start)
        backend_draw_remote_lines(gLines + remote_start,
                                  (int)(gNLines - remote_start));
}

static void draw_press_fire(void) {
    const Seg * const kPF[] = {
        seg_P, seg_R, seg_E, seg_S, seg_S, NULL,
        seg_F, seg_I, seg_R, seg_E, NULL,
        seg_T, seg_O, NULL,
        seg_S, seg_T, seg_A, seg_R, seg_T
    };
    lines_reset();
    draw_seg_array(kPF, 77, 180, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
    memset(&gLines[gNLines], 0, sizeof(Line));
    backend_draw_alien_lines(gLines, gNLines);   /* into drawing buffer */
    backend_present(0, 0);                        /* swap */
    backend_draw_alien_lines(gLines, gNLines);   /* into the other buffer */
}

int main(int argc, char *argv[]) {
    uint16_t min_frame = 0;
    uint16_t max_frame = 0;   /* 0 = no limit (replaces -1 sentinel) */
    World w = {
        .ps            = { CAM_Y_INIT, CAM_X_INIT, 0, 0, 0 },
        .cam_zspeed    = CAM_ZSPEED_BASE,
        .round         = 1,
        .takeoff_timer = TAKEOFF_FRAMES_BASE,
        /* everything else (angles, phase, frame, timers, strip, entities)
         * starts at zero/dead */
    };
    RaceState rs;                  /* remote (peer/bot) slot — see physics.c */
    GameState state = STATE_TAKEOFF;

    if (argc >= 2) min_frame = (uint16_t)atoi(argv[1]);
    if (argc >= 3) max_frame = (uint16_t)atoi(argv[2]);
    /* "nobot" keeps the remote slot empty without a peer — used by the
     * deterministic race-mode tests (env vars don't survive hatari). */
    race_init(&rs, !(argc >= 6 && strcmp(argv[5], "nobot") == 0));

    backend_init();
    serial_init(argc >= 4 ? argv[3] : NULL, argc >= 5 ? argv[4] : NULL);
    gLines = malloc((MAX_DRAW_LINES + 1) * sizeof(Line));
    lut_init();
    model_init();
    w.strip_x = next_strip_x(w.round, w.frame);

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
#define INTRO_NSTEPS (HUD_NCHARS > HUD_NSUB ? HUD_NCHARS : HUD_NSUB)
    {
        int8_t k = 0;

        /* Draw credits into plane 0 of both buffers. */
        lines_reset();
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
            if (k < HUD_NSUB)       drew |= hud_draw_subletter(k);
            k++;
            if (!drew) continue;    /* both are spaces: skip pause */
            for (j = 0; j < INTRO_LETTER_FRAMES; j++)
                backend_present(0, 0);
        }
        hud_draw_tally(1);
    }

    build_grid();

    /* Convert rad/frame speeds to LUT-index increments */
    w.angleYinc = (int16_t)(0.16 * FP_ONE);
    w.angleXinc = (int16_t)(-0.13 * FP_ONE);
    w.angleYinc = (int16_t)((int32_t)w.angleYinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));
    w.angleXinc = (int16_t)((int32_t)w.angleXinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));

    /* "PRESS FIRE TO START" — seed alien plane of both framebuffers once, then
     * wait.  Glow animates via palette on each backend_present(); no pixel redraw. */
    {
        draw_press_fire();
        for (;;) {
            uint8_t keys = backend_get_keys() | PERF_KEYS;
            if (keys & KEY_QUIT) { backend_cleanup(); return 0; }
            if (keys & KEY_FIRE) {
                backend_snd_switch(SND_MAIN);
                backend_snd_sfx(SND_FIRE);
                break;
            }
            backend_present(0, 0);
        }
    }
    for (;;) {
        uint8_t keys = backend_get_keys() | PERF_KEYS;
        if (keys & KEY_QUIT) break;
        if (max_frame != 0 && w.frame > max_frame) break;

        /* Grid always scrolls */
        w.z_phase = (int16_t)(w.z_phase + w.cam_zspeed);
        if (w.z_phase >= GRID_ZSTEP) w.z_phase = (int16_t)(w.z_phase - GRID_ZSTEP);

        bool flash = false;
        bool fired = false;
        const RenderFlags *rf = &kStateFlags[state];

        int prev_state = state;
        switch (state) {
        case STATE_TAKEOFF: state = state_takeoff(&w, keys);         break;
        case STATE_CRUISE:  state = state_cruise(&w, &fired, keys);  break;
        case STATE_LANDING: state = state_landing(&w, keys);         break;
        case STATE_CRASH:   state = state_crash(&w, &flash);         break;
        case STATE_SUCCESS: state = state_success(&w, keys);         break;
        }

        /* Alien contact: live alien crossing z=0 within FP_ONE laterally → crash */
        if ((state == STATE_CRUISE || state == STATE_LANDING) &&
            alien_hit_player(&w)) {
            state = STATE_CRASH;
        }

        /* Safety clamp: allow wider roam during cruise; grid uses cam_x only for
         * horizontal lines (world x fixed) so ±6 is safe.  Strip rendering guards
         * cam_x_rel separately below (strip can be up to STRIP_X_MAX away).
         * Clamped before the wire too: the 14-bit packet field relies on it. */
        if (w.ps.cam_x >  6 * FP_ONE) w.ps.cam_x =  (int16_t)(6 * FP_ONE);
        if (w.ps.cam_x < -6 * FP_ONE) w.ps.cam_x = -(int16_t)(6 * FP_ONE);
        /* Altitude ceiling (2× cruise altitude): the grid projections divide
         * cam_y*FOCAL by z as small as HLINE_ZMIN, which overflows divs16
         * past ~5.2*FP_ONE, and the rasterizer does not clip — an unbounded
         * climb wrapped cam_y through int16 and corrupted memory (machine
         * hang at ~35 s of held Up; found by the perf_frames.sh soak). */
        if (w.ps.cam_y >  4 * FP_ONE) w.ps.cam_y =  (int16_t)(4 * FP_ONE);

        /* Advance the player's own missiles (alien collisions) every frame. */
        update_missiles(w.cam_zspeed, &w.missiles, &w.aliens);

        /* Remote (peer/bot) slot: state, peer missiles, kills, beacon, ghost. */
        race_update(&rs, &state, rf->remote_player, &w, fired);

        /* Sound transitions last: a crash can come from the state machine,
         * an alien, or the peer's KILL — all of the above. */
        if (state == STATE_CRASH && prev_state != STATE_CRASH) {
            w.crash_timer = backend_snd_switch(SND_GAMEOVER);
        }
        if (state == STATE_TAKEOFF && prev_state == STATE_CRASH)
            backend_snd_switch(SND_MAIN);

        backend_set_flash(flash);
        w.frame++;
        if (w.frame < min_frame) continue;
        backend_clear();
        draw_world_plane(rf, &w);
        draw_alien_plane(rf, &w, &rs);
        backend_present(w.angleY, w.angleX);
    }

    serial_cleanup();
    backend_cleanup();
    return 0;
}
