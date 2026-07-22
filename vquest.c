#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "backend.h"
#include "serial.h"
#include "vquest.h"        /* FP_*, LUT_SIZE, Point3DInt, PhysicsState, … */

#include "tuning.h"       /* gameplay/simulation tuning constants */

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
            v = S16(v + (((i - 1) & 1) ? (b >> 4) : (b & 15)));
        }
        sinLUT[i]                              =  v;
        sinLUT[LUT_SIZE / 2 - i]               =  v;
        sinLUT[LUT_SIZE / 2 + i]               = S16(-v);
        sinLUT[(LUT_SIZE - i) & (LUT_SIZE-1)] = S16(-v);
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
    /* S16, not a bare cast: every caller is documented as keeping the >>10
     * product inside int16_t, and the debug builds now hold them to it. */
    return S16(((int32_t)a * b) >> FP_SHIFT);
}

#include "render.c"    /* 3-D rendering pipeline (unity include) */
#include "physics.c"   /* game logic and state machine (unity include) */

/* ── Per-frame composition (world plane, then alien plane) ────────────────── */

/* draw_gate_text — verdict + prompt for the between-laps gate screen.
 * Batch-append only (no lines_reset/present): it composes with the logo
 * inside draw_alien_plane's batch, unlike the old static wait screen. */
static void draw_gate_text(int8_t lap_result, bool gate_ready,
                           uint16_t alien_kills, uint16_t race_frames,
                           uint16_t best_lap_frames) {
    if (lap_result == LAP_NONE) {
        if (gate_ready)
            draw_text("GET READY", 113, 180, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
        else
            draw_text("PRESS FIRE TO START", 77, 180, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
        return;
    }

    if (lap_result == LAP_WON)
        draw_text("VICTORY", 122, 60, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
    else
        draw_text("DEFEAT", 127, 60, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);

    /* Race stats: race time (seconds), race kills, best lap.
     * Labels are 4-5 chars at FONT_SML_STEP=6px → 30px column, then a 6px gap,
     * then the number column.  Bottom-right corner: the only band clear of
     * the persistent HUD title (top 40 rows), the credits (x 68-256, y
     * 83-171), and the centred GET READY/PRESS FIRE prompt (ends ~x=220).
     * Worst case row is the "TIME" fallback when no lap has completed yet,
     * ending at x=315 — the rasterizer has no clipping, so keep everything
     * under x=319. */
    {
        int16_t lbl_x = 256, num_x = 292;  /* 256 + 5 chars * 6px + 6px gap */
        int16_t row1_y = 160, row2_y = 172, row3_y = 184;
        int8_t  ss = FONT_SML_SX, sy = FONT_SML_SY;
        int16_t sp = FONT_SML_STEP;
        int16_t secs = S16(race_frames / 50);

        draw_text("TIME",  lbl_x, row1_y, ss, sy, sp, 0);
        draw_number(secs, num_x, row1_y, ss, sy, sp);

        draw_text("KILLS", lbl_x, row2_y, ss, sy, sp, 0);
        draw_number((int16_t)alien_kills, num_x, row2_y, ss, sy, sp);

        draw_text("BEST",  lbl_x, row3_y, ss, sy, sp, 0);
        if (best_lap_frames > 0)
            draw_number(S16(best_lap_frames / 50), num_x, row3_y, ss, sy, sp);
        else
            draw_text("TIME", num_x, row3_y, ss, sy, sp, 0);  /* show TIME for current lap */
    }

    if (gate_ready)
        draw_text("GET READY", 113, 180, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
    else
        draw_text("PRESS FIRE", 107, 180, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
}

/* Debug overlay (D key): toggled in main, drawn in draw_world_plane (grid
 * plane, blue) so it reads distinctly from the alien-plane HUD readouts.
 * draw_world_plane doesn't take a GameState, hence the gDebugState mirror. */
static bool     gDebugOverlay;
static uint8_t  gDebugState;
static uint16_t gDebugLines;   /* alien plane's peak gNLines from the PREVIOUS
                                 * frame (draw_world_plane, where the overlay
                                 * now lives, runs before draw_alien_plane and
                                 * the two reset the shared buffer
                                 * independently); added to the current
                                 * frame's own gNLines for the 'N' readout so
                                 * it reports a whole-frame total, not just
                                 * whichever plane's buffer it was sampled
                                 * from */

/* Debug overlay (D key): label + signed value pairs.  draw_number renders
 * nothing for negative input, so the sign is a one-seg glyph here; values
 * shown must already be < 32768 in magnitude. */
static const Seg kSegMinus[] = { { 0,4, 3,4 }, { -1,0, 0,0 } };

static int16_t dbg_item(char label, int16_t val, int16_t x, int16_t y)
{
    char s[2] = { label, 0 };
    draw_text(s, x, y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
    x = S16(x + FONT_SML_STEP);
    if (val < 0) {
        font_draw(kSegMinus, x, y, FONT_SML_SX, FONT_SML_SY);
        x = S16(x + FONT_SML_STEP);
        val = S16(-val);
    }
    return S16(draw_number(val, x, y, FONT_SML_SX, FONT_SML_SY,
                           FONT_SML_STEP) + FONT_SML_STEP);
}

static inline void draw_world_plane(const RenderFlags *rf, const World *w,
                                    const RaceState *rs)
{
    lines_reset();
    render_grid(rf->grid, CRUISE_ALT, w->z_phase, w->ps.cam_x);
    if (gDebugOverlay) {
        int16_t x;
        /* 'X' has no glyph (draw.c never draws J/W/X) — 'C' (cam-x) stands
         * in for it. */
        x = dbg_item('S', (int16_t)gDebugState, 8, 44);
        x = dbg_item('L', (int16_t)w->lap, x, 44);
        x = dbg_item('Z', w->cam_zspeed, x, 44);
        x = dbg_item('C', w->ps.cam_x, x, 44);
        (void)dbg_item('F', w->finish_dist, x, 44);
        x = dbg_item('R', (int16_t)rs->remote.state, 8, 54);
        x = dbg_item('L', (int16_t)rs->remote.lap, x, 54);
        x = dbg_item('D', rs->peer_rel_z, x, 54);
        x = dbg_item('I', rs->remote_idle, x, 54);
        x = dbg_item('P', S16(rs->rx_count % 10000), x, 54);
        (void)dbg_item('N', S16(gNLines + gDebugLines), x, 54);
    }
    if (rf->credits) credits_render();
    lines_seal();
    backend_draw_lines(gLines, gNLines);
}

static inline void draw_alien_plane(const RenderFlags *rf, const World *w,
                                    const RaceState *rs)
{
    int16_t cam_x = w->ps.cam_x, cam_y = CRUISE_ALT;
    int i;
    uint16_t remote_start;
    lines_reset();
    render_logo(rf->gate, w->angleY, w->angleX);
    if (rf->gate) draw_gate_text(w->lap_result, w->gate_ready,
                                   w->alien_kills, w->race_frames,
                                   w->best_lap_frames);
    render_finish_line(rf->finish_line, w->finish_dist, cam_x, cam_y, w->z_phase);
    if (rf->aliens) {
        for (i = 0; i < ALIEN_COUNT; i++)
            if (w->aliens.alive[i]) draw_alien(w->aliens.x[i], w->aliens.z[i], cam_x);
        for (i = 0; i < MISSILE_COUNT; i++)
            if (w->missiles.alive[i]) draw_missile(w->missiles.vis_z[i]);
        /* mymines are never drawn (always behind the camera); only incoming
         * mines are a hazard to render.  Must land here, before
         * remote_start below, or it gets recoloured yellow. */
        for (i = 0; i < MINE_COUNT; i++)
            if (w->mines.alive[i]) draw_mine(w->mines.x[i], w->mines.z[i], cam_x);
    }
    /* Distance to opponent (top-right corner, world units). */
    if (rf->remote_player && rs->peer_rel_z != 0) {
        int16_t dist = S16(rs->peer_rel_z / FP_ONE);
        bool ahead = (dist > 0);
        int16_t dx = 276, dy = 6;
        if (dist < 0) dist = S16(-dist);
        if (dist > 99) dist = 99;
        font_draw(ahead ? seg_up : seg_dn, dx, dy, FONT_SML_SX, FONT_SML_SY);
        draw_number(dist, S16(dx + FONT_SML_STEP), dy,
                    FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP);
    }
    /* Current lap, beside the opponent-distance readout.  Must be appended
     * before remote_start below or it gets recoloured yellow (see the
     * comment on remote_start). */
    if (rf->aliens) {
        int16_t dx = 276, dy = 16;
        draw_text("LAP", dx, dy, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
        draw_number(w->lap, S16(dx + 3 * FONT_SML_STEP + FONT_SML_STEP), dy,
                    FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP);
        /* Mine ammo: one tick per remaining drop (mines_left is at most
         * MINES_PER_RACE=3), under the LAP readout.  Must be appended before
         * remote_start below or it gets recoloured yellow (same reason as
         * the LAP readout above). */
        {
            int16_t tx = 276;
            int mi;
            for (mi = 0; mi < (int)w->mines_left; mi++, tx = S16(tx + 5))
                append_line(tx, 26, tx, 29);
        }
    }

    /* Remote-player lines (ghost triangle + peer missiles) must stay last in
     * the batch: the tail slice is re-drawn into plane 0 below so its pixels
     * read as index 3 (planes 0+1, yellow) instead of the alien colour.  The
     * shared zero-sentinel terminates both the full batch and the slice. */
    remote_start = gNLines;
    if (rs->ghost_show)
        draw_remote_player(rs->remote.cam_x, rs->ghost_z, cam_x);
    if (rf->remote_player)
        for (i = 0; i < MISSILE_COUNT; i++)
            if (rs->rmissiles.alive[i])
                draw_remote_missile(rs->rmissiles.x[i], rs->rmissiles.z[i], cam_x);
    gDebugLines = gNLines;
    lines_seal();
    backend_draw_alien_lines(gLines, gNLines);
    if (gNLines > remote_start)
        backend_draw_remote_lines(gLines + remote_start,
                                  (int)(gNLines - remote_start));
}

int main(int argc, char *argv[]) {
    uint16_t min_frame = 0;
    uint16_t max_frame = 0;   /* 0 = no limit (replaces -1 sentinel) */
    World w = {
        .ps         = { CAM_X_INIT, 0, 0 },
        .cam_zspeed = CAM_ZSPEED_BASE,
        .round      = 1,
        .lap        = 1,   /* race_update's rel_depth() runs even at the gate,
                             * before the first race_start(); every other field
                             * not named here starts at zero/dead: LAP_NONE,
                             * parity 0, not ready */
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
        bool skip = false;

        /* Draw credits into plane 0 of both buffers. */
        lines_reset();
        credits_render();
        lines_seal();
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
            if (!drew || skip) continue;    /* both are spaces, or already skipping: no pause */
            for (j = 0; j < INTRO_LETTER_FRAMES; j++) {
                backend_present(0, 0);
                /* Any key (including ESC) stops the pauses; remaining
                 * letters still draw, just without the per-letter dwell. */
                if (backend_get_keys() != 0) { skip = true; break; }
            }
        }
        hud_draw_tally(1);
    }

    build_grid();

    /* Convert rad/frame speeds to LUT-index increments */
    w.angleYinc = (int16_t)(0.16 * FP_ONE);
    w.angleXinc = (int16_t)(-0.13 * FP_ONE);
    w.angleYinc = S16((int32_t)w.angleYinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));
    w.angleXinc = S16((int32_t)w.angleXinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));

    for (;;) {
        uint8_t keys = backend_get_keys() | PERF_KEYS;
        if ((keys & KEY_DEBUG) && !(w.prev_keys & KEY_DEBUG))
            gDebugOverlay = !gDebugOverlay;
        if (keys & KEY_QUIT) break;
        if (max_frame != 0 && w.frame > max_frame) break;

        /* Grid always scrolls */
        w.z_phase = S16(w.z_phase + w.cam_zspeed);
        if (w.z_phase >= GRID_ZSTEP) w.z_phase = S16(w.z_phase - GRID_ZSTEP);

        bool flash   = false;
        bool fired   = false;
        bool dropped = false;
        const RenderFlags *rf = &kStateFlags[state];

        int prev_state = state;
        switch (state) {
        case STATE_CRUISE: state = state_cruise(&w, &fired, &dropped, keys, rs.peer_finished); break;
        case STATE_CRASH:  state = state_crash(&w, &flash);                          break;
        case STATE_GATE:   state = state_gate(&w, keys, rs.peer_gate_ok);            break;
        }
        gDebugState = (uint8_t)state;
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

        /* Advance the player's own missiles (alien collisions) every frame. */
        update_missiles(w.cam_zspeed, &w.missiles, &w.aliens, &w.alien_kills);

        /* Remote (peer/bot) slot: state, peer missiles, kills, beacon, ghost.
         * player_won is true only on the CRUISE->GATE edge where we just
         * crossed the line first — it forces the bot to lose its lap
         * immediately instead of racing on to its own finish line (a real
         * peer applies the mirror-image peer_finished check on their own
         * machine and needs no such push). */
        bool player_won = state == STATE_GATE && prev_state == STATE_CRUISE &&
                           w.lap_result == LAP_WON;
        race_update(&rs, &state, rf->remote_player, &w, fired, dropped, player_won);

        apply_speed_modifiers(&w, &rs, state);

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
        w.prev_keys = keys;
        w.frame++;
        if (w.frame < min_frame) continue;
        backend_clear();
        draw_world_plane(rf, &w, &rs);
        draw_alien_plane(rf, &w, &rs);
        backend_present(w.angleY, w.angleX);
    }

    serial_cleanup();
    backend_cleanup();
    return 0;
}
