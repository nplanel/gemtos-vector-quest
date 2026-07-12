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

/* draw_gate_text — verdict + prompt for the between-laps gate screen.
 * Batch-append only (no lines_reset/present): it composes with the logo
 * inside draw_alien_plane's batch, unlike the old static wait screen. */
static void draw_gate_text(int8_t lap_result, bool gate_ready,
                           uint16_t alien_kills, uint16_t lap_frames,
                           uint16_t best_lap_frames) {
    static const Seg * const kVictory[] = {
        seg_V, seg_I, seg_C, seg_T, seg_O, seg_R, seg_Y
    };
    static const Seg * const kDefeat[] = {
        seg_D, seg_E, seg_F, seg_E, seg_A, seg_T
    };
    static const Seg * const kGetReady[] = {
        seg_G, seg_E, seg_T, NULL, seg_R, seg_E, seg_A, seg_D, seg_Y
    };
    static const Seg * const kPressFire[] = {
        seg_P, seg_R, seg_E, seg_S, seg_S, NULL, seg_F, seg_I, seg_R, seg_E
    };
    static const Seg * const kTime[] = {
        seg_T, seg_I, seg_M, seg_E
    };
    static const Seg * const kKills[] = {
        seg_K, seg_I, seg_L, seg_L, seg_S
    };
    static const Seg * const kBest[] = {
        seg_B, seg_E, seg_S, seg_T
    };

    if (lap_result == LAP_NONE) {
        static const Seg * const kPF[] = {
            seg_P, seg_R, seg_E, seg_S, seg_S, NULL,
            seg_F, seg_I, seg_R, seg_E, NULL,
            seg_T, seg_O, NULL,
            seg_S, seg_T, seg_A, seg_R, seg_T
        };
        if (gate_ready)
            draw_seg_array(kGetReady, 113, 180, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
        else
            draw_seg_array(kPF, 77, 180, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
        return;
    }

    if (lap_result == LAP_WON)
        draw_seg_array(kVictory, 122, 60, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
    else
        draw_seg_array(kDefeat, 127, 60, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);

    /* Lap stats: time (seconds), kills, best lap.
     * Labels are 4-5 chars at FONT_SML_STEP=6px → 30px column, then a 6px gap,
     * then the right-aligned number column. */
    {
        int16_t lbl_x = 62, num_x = 98;  /* 62 + 5 chars * 6px + 6px gap */
        int16_t row1_y = 100, row2_y = 112, row3_y = 124;
        int8_t  ss = FONT_SML_SX, sy = FONT_SML_SY;
        int16_t sp = FONT_SML_STEP;
        int16_t secs = (int16_t)(lap_frames / 50);

        draw_seg_array(kTime,  lbl_x, row1_y, ss, sy, sp, 0);
        draw_number(secs, num_x, row1_y, ss, sy, sp);

        draw_seg_array(kKills, lbl_x, row2_y, ss, sy, sp, 0);
        draw_number((int16_t)alien_kills, num_x, row2_y, ss, sy, sp);

        draw_seg_array(kBest,  lbl_x, row3_y, ss, sy, sp, 0);
        if (best_lap_frames > 0)
            draw_number((int16_t)(best_lap_frames / 50), num_x, row3_y, ss, sy, sp);
        else
            draw_seg_array(kTime, num_x, row3_y, ss, sy, sp, 0);  /* show TIME for current lap */
    }

    if (gate_ready)
        draw_seg_array(kGetReady, 113, 180, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
    else
        draw_seg_array(kPressFire, 107, 180, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
}

static inline void draw_world_plane(const RenderFlags *rf, const World *w)
{
    lines_reset();
    render_grid(rf->grid, w->ps.cam_y, w->z_phase, w->ps.cam_x);
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
    render_logo(rf->gate, w->angleY, w->angleX);
    if (rf->gate) draw_gate_text(w->lap_result, w->gate_ready,
                                   w->alien_kills, w->lap_frames,
                                   w->best_lap_frames);
    render_finish_line(rf->finish_line, w->finish_dist, cam_x, cam_y, w->z_phase);
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

int main(int argc, char *argv[]) {
    uint16_t min_frame = 0;
    uint16_t max_frame = 0;   /* 0 = no limit (replaces -1 sentinel) */
    World w = {
        .ps         = { CRUISE_ALT, CAM_X_INIT, 0, 0 },
        .cam_zspeed = CAM_ZSPEED_BASE,
        .round      = 1,
        /* everything else (angles, phase, frame, timers, lap/gate fields,
         * entities) starts at zero/dead: LAP_NONE, parity 0, not ready */
    };
    RaceState rs;                  /* remote (peer/bot) slot — see physics.c */
    GameState state = STATE_GATE;
    int snd_slot = -1;              /* last slot passed to backend_snd_switch;
                                      * -1 = none yet (SND_INTRO still playing) */

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
        case STATE_CRUISE: state = state_cruise(&w, &fired, keys, rs.peer_finished); break;
        case STATE_CRASH:  state = state_crash(&w, &flash);                          break;
        case STATE_GATE:   state = state_gate(&w, keys, rs.peer_gate_ok);            break;
        }
        /* rs's fields above are last frame's — one frame of handshake skew is
         * fine and already absorbed by the RS_CRUISE clause of peer_gate_ok. */

        /* Alien contact: live alien crossing z=0 within FP_ONE laterally → crash */
        if (state == STATE_CRUISE && alien_hit_player(&w)) {
            state = STATE_CRASH;
        }

        /* Safety clamp: allow wider roam during cruise; grid uses cam_x only for
         * horizontal lines (world x fixed) so ±6 is safe.
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
        update_missiles(w.cam_zspeed, &w.missiles, &w.aliens, &w.alien_kills);

        /* Remote (peer/bot) slot: state, peer missiles, kills, beacon, ghost. */
        race_update(&rs, &state, rf->remote_player, &w, fired);

        /* Drafting: a small speed bonus when chasing close behind the opponent
         * (≤ 1 world unit, ~0.5 s at base speed).  Incentivises tight racing
         * and rewards the chaser for staying in the leader's slipstream. */
        if (state == STATE_CRUISE && rs.ghost_show &&
            rs.peer_rel_z > 0 && rs.peer_rel_z <= FP_ONE) {
            w.cam_zspeed = (int16_t)(w.cam_zspeed + 2);
        }

        /* Sound transitions last: a crash can come from the state machine,
         * an alien, or the peer's KILL — all of the above. */
        if (state == STATE_CRASH && prev_state != STATE_CRASH) {
            w.crash_timer = STUN_FRAMES;      /* stun: fixed length, music keeps
                                                * playing — hit sfx only */
            backend_snd_sfx(SND_ENMYHIT);
        }
        if (state == STATE_GATE && prev_state == STATE_CRUISE &&
            w.lap_result == LAP_LOST) {
            backend_snd_switch(SND_GAMEOVER); /* DEFEAT jingle at the gate */
            snd_slot = SND_GAMEOVER;
        }
        if (state == STATE_CRUISE && prev_state == STATE_GATE) {
            race_lap_reset(&rs);              /* clear peer FINISHED latch */
            /* backend_snd_switch always restarts its track from frame 0, so
             * gating on the last-switched slot avoids an audible restart of
             * the main theme on every winning lap while still recovering
             * from the defeat jingle above. */
            if (snd_slot != SND_MAIN) { backend_snd_switch(SND_MAIN); snd_slot = SND_MAIN; }
            backend_snd_sfx(SND_FIRE);
        }

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
