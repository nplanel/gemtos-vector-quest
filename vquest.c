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

/* Quarter sine wave, expanded at startup from the baked nibble-packed deltas
 * (kSinQuarterNib, two 0..4 deltas per byte, sin[0]=0 implicit; see
 * gen_tables.c).  Integer-only.
 *
 * Only the first quadrant is stored — 513 entries, 1,026 B — and fastSin folds
 * the rest in:
 *   sin[LUT_SIZE/2 - i] =  sin[i]          (mirror around pi/2)
 *   sin[LUT_SIZE/2 + i] = -sin[i]          (half-wave antisymmetry)
 * The full 2,048-entry table it replaces cost 4,096 B of bss to serve four
 * lookups per frame from render_logo, the only caller, on the gate screen
 * only.  No cosine table: cos(a) = sin(a + LUT_SIZE/4), folded into fastCos. */
static int16_t sinQuarter[LUT_SIZE / 4 + 1];

static void lut_init(void) {
    uint16_t i;
    int16_t  v = 0;
    sinQuarter[0] = 0;
    for (i = 1; i <= LUT_SIZE / 4; i++) {
        uint8_t b = kSinQuarterNib[(i - 1) >> 1];
        v = S16(v + (((i - 1) & 1) ? (b >> 4) : (b & 15)));
        sinQuarter[i] = v;
    }
}

static inline int16_t fastSin(int16_t angle) {
    uint16_t a = (uint16_t)angle & (LUT_SIZE - 1);
    uint16_t h = a & (LUT_SIZE / 2 - 1);            /* index within the half wave */
    int16_t  v = sinQuarter[h > LUT_SIZE / 4 ? (uint16_t)(LUT_SIZE / 2 - h) : h];
    return (a & (LUT_SIZE / 2)) ? S16(-v) : v;      /* second half is negated */
}

static inline int16_t fastCos(int16_t angle) {
    return fastSin(S16W(angle + LUT_SIZE / 4));
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

/* Gate prompt, picked once per frame by draw_alien_plane. */
#define PROMPT_NONE  0   /* dwell not over yet, or armed and about to launch */
#define PROMPT_FIRE  1
#define PROMPT_PEER  2   /* 2 players: no live peer, or armed and the peer isn't ready */

/* draw_gate_prompt — the centred bottom prompt.  No "GET READY" step: once
 * both sides are ready the 3-2-1-GO countdown takes over immediately.
 * ("LOOKING", not "WAITING": the font has no W.) */
static void draw_gate_prompt(uint8_t prompt, bool first) {
    if (prompt == PROMPT_PEER)
        draw_text("LOOKING FOR PEER", 77, 180, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
    else if (prompt == PROMPT_FIRE) {
        if (first)
            draw_text("PRESS FIRE TO START", 77, 180, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
        else
            draw_text("PRESS FIRE", 107, 180, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
    }
}

/* draw_gate_text — verdict + prompt for the between-laps gate screen.
 * Batch-append only (no lines_reset/present): it composes with the logo
 * inside draw_alien_plane's batch, unlike the old static wait screen. */
static void draw_gate_text(int8_t race_result, uint8_t prompt,
                           uint16_t alien_kills, uint16_t race_frames,
                           uint16_t best_lap_frames) {
    draw_gate_prompt(prompt, race_result == RACE_NONE);
    if (race_result == RACE_NONE) return;

    if (race_result == RACE_WON)
        draw_text("VICTORY", 122, 60, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);
    else
        draw_text("DEFEAT", 127, 60, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 6);

    /* Race stats: race time (seconds), race kills, best lap.
     * Labels are 4-5 chars at FONT_SML_STEP=6px → 30px column, then a 6px gap,
     * then the number column.  Bottom-right corner: the only band clear of
     * the persistent HUD title (top 40 rows), the credits (x 68-256, y
     * 83-171), and the centred gate prompt (ends ~x=242).
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
}

/* draw_countdown_text — 3/2/1/GO overlaid on the frozen track before
 * launch (STATE_COUNTDOWN).  Four fixed phases, plain compares
 * (COUNTDOWN_FRAMES is only ever 4 steps, no divide needed).  FONT_BIG
 * glyph width 16px at sx=4: single digit centred on x=160 at ~152, "GO"
 * (2 glyphs, span 36) at ~142. */
#define COUNTDOWN_X_DIGIT 152
#define COUNTDOWN_X_GO    142
#define COUNTDOWN_Y         80
static void draw_countdown_text(int16_t remaining) {
    if (remaining > 3 * COUNTDOWN_STEP_FRAMES)
        draw_text("3",  COUNTDOWN_X_DIGIT, COUNTDOWN_Y, FONT_BIG_SX, FONT_BIG_SY, FONT_BIG_STEP, 6);
    else if (remaining > 2 * COUNTDOWN_STEP_FRAMES)
        draw_text("2",  COUNTDOWN_X_DIGIT, COUNTDOWN_Y, FONT_BIG_SX, FONT_BIG_SY, FONT_BIG_STEP, 6);
    else if (remaining > 1 * COUNTDOWN_STEP_FRAMES)
        draw_text("1",  COUNTDOWN_X_DIGIT, COUNTDOWN_Y, FONT_BIG_SX, FONT_BIG_SY, FONT_BIG_STEP, 6);
    else
        draw_text("GO", COUNTDOWN_X_GO, COUNTDOWN_Y, FONT_BIG_SX, FONT_BIG_SY, FONT_BIG_STEP, 6);
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
 * shown must already be < 32768 in magnitude.  Also forwarded verbatim to
 * backend_debug_item(), a no-op everywhere except the ascii backend, which
 * prints it as a "DBG label val" line so tests can read internal state
 * without parsing rendered glyph coordinates. */
static const Seg kSegMinus[] = { { 0,4, 3,4 }, { -1,0, 0,0 } };

static int16_t dbg_item(char label, int16_t val, int16_t x, int16_t y)
{
    char s[2] = { label, 0 };
    backend_debug_item(label, val);
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
        /* Wire health, real-hardware triage (see serial.h): P climbing with
         * B and E at zero is a clean link; B climbing means received bytes
         * are being lost (MFP overrun / iorec full), E climbing means they
         * arrive corrupted (cable, ground, length).  S is the dead-reckoned
         * peer speed, non-zero whenever the ghost is being extrapolated. */
        x = dbg_item('B', S16(gSerialShort  % 10000), 8, 64);
        x = dbg_item('E', S16(gSerialBadSum % 10000), x, 64);
        (void)dbg_item('S', rs->peer_speed, x, 64);
    }
    if (unlikely(rf->credits)) credits_render();
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
    if (unlikely(rf->gate)) {
        uint8_t prompt =
            w->gate_timer > 0                                   ? PROMPT_NONE
          : !rs->bot_enabled && (!rs->remote_live || w->gate_ready) ? PROMPT_PEER
          : w->gate_ready                                       ? PROMPT_NONE
          :                                                       PROMPT_FIRE;
        draw_gate_text(w->race_result, prompt, w->alien_kills, w->race_frames,
                       w->best_lap_frames);
    }
    render_finish_line(rf->finish_line, w->finish_dist, cam_x, cam_y, w->z_phase);
    if (likely(rf->aliens)) {
        for (i = 0; i < ALIEN_COUNT; i++)
            if (w->aliens.alive[i]) draw_alien(w->aliens.x[i], w->aliens.z[i], cam_x);
        for (i = 0; i < MISSILE_COUNT; i++)
            if (w->missiles.alive[i]) draw_missile(w->missiles.vis_z[i]);
    }
    /* 3/2/1/GO overlaid on the frozen track (STATE_COUNTDOWN only).  The
     * lap/mine readout is not drawn here: it lives on the HUD plane
     * (hud_draw_race, redrawn by main() only when it changes). */
    if (unlikely(rf->countdown)) draw_countdown_text(w->countdown_timer);

    /* Opponent-coloured tail slice: logo caption, mines, ghost triangle,
     * peer missiles and the opponent alignment/range gauge must stay last in
     * the batch.  The slice is re-drawn into plane 0 below so its pixels
     * read as index 3 (planes 0+1, the opponent's glow — yellow vs the bot,
     * purple over serial) instead of the alien colour.  The shared
     * zero-sentinel terminates both the full batch and the slice.  mymines
     * are never drawn (always behind the camera); only incoming mines are a
     * hazard to render. */
    remote_start = gNLines;
    render_logo_caption(rf->gate);
    /* Opponent alignment/range gauge — a chevron that slides to show the
     * opponent's lateral offset, above the horizon (leader) or bottom (chaser),
     * with the range in world units.  Replaces the old static top-right
     * readout.  Coloured like the opponent (this slice) so the gauge matches
     * who you're racing.  Shown only while the opponent is racing (not
     * during the 3/2/1/GO countdown, where its lap/progress are last race's
     * and rel_depth saturates to a bogus 30), and hidden while the ghost
     * itself is on screen: the gauge stands in for an opponent you cannot
     * see. */
    if (rf->remote_player && rs->remote_live && rs->peer_rel_z != 0 &&
        (rs->remote.state == RS_CRUISE || rs->remote.state == RS_DEAD) &&
        !rs->ghost_show) {
        bool ahead = (rs->peer_rel_z > 0);
        int16_t dist = S16((ahead ? rs->peer_rel_z : S16(-rs->peer_rel_z)) / FP_ONE);
        draw_opponent_marker(S16(rs->remote.cam_x - cam_x), dist, ahead);
    }
    if (likely(rf->aliens))
        for (i = 0; i < MINE_COUNT; i++)
            if (w->mines.alive[i]) draw_mine(w->mines.x[i], w->mines.z[i], cam_x);
    if (rs->ghost_show)
        draw_remote_player(rs->remote.cam_x, rs->ghost_z, cam_x);
    if (likely(rf->remote_player))
        for (i = 0; i < MISSILE_COUNT; i++)
            if (rs->rmissiles.alive[i])
                draw_remote_missile(rs->rmissiles.x[i], rs->rmissiles.z[i], cam_x);
    gDebugLines = gNLines;
    lines_seal();
    backend_draw_alien_lines(gLines, gNLines);
    if (gNLines > remote_start)
        backend_draw_remote_lines(gLines + remote_start,
                                  (int)(gNLines - remote_start),
                                  rs->bot_enabled);
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
                             * not named here starts at zero/dead: RACE_NONE,
                             * parity 0, not ready */
    };
    RaceState rs;                  /* remote (peer/bot) slot — see physics.c */
    GameState state = STATE_GATE;
    int snd_slot = -1;              /* last slot passed to backend_snd_switch;
                                      * -1 = none yet (SND_INTRO still playing) */

    if (argc >= 2) min_frame = (uint16_t)atoi(argv[1]);
    if (argc >= 3) max_frame = (uint16_t)atoi(argv[2]);
    /* Boots in 1 player (bot).  "nobot" boots in 2 players with the
     * test-only peerless gate escape — used by the deterministic race-mode
     * tests (env vars don't survive hatari). */
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

        backend_hud_begin();
        hud_draw_mode(rs.bot_enabled, rs.link_state);
        hud_draw_hz(backend_get_hz());
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
    }

    /* Convert rad/frame speeds to LUT-index increments */
    w.angleYinc = (int16_t)(0.16 * FP_ONE);
    w.angleXinc = (int16_t)(-0.13 * FP_ONE);
    w.angleYinc = S16((int32_t)w.angleYinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));
    w.angleXinc = S16((int32_t)w.angleXinc * LUT_SIZE / (2L * FP_ONE * 31415 / 10000));

    for (;;) {
        uint8_t keys = backend_get_keys() | PERF_KEYS;
        if ((keys & KEY_DEBUG) && !(w.prev_keys & KEY_DEBUG))
            gDebugOverlay = !gDebugOverlay;
        /* F1 picks 1 or 2 players at the gate; the race then sticks to it.
         * A switch disarms FIRE: a press made while LOOKING FOR PEER must not
         * carry over and launch the other mode behind the player's back. */
        if (unlikely((keys & KEY_MODE) && !(w.prev_keys & KEY_MODE) &&
                     state == STATE_GATE)) {
            race_set_mode(&rs, !rs.bot_enabled);
            w.gate_ready = false;
            hud_draw_mode(rs.bot_enabled, rs.link_state);
        }
        if (keys & KEY_QUIT) break;
        if (unlikely(max_frame != 0 && w.frame > max_frame)) break;

        if (unlikely(backend_hz_changed())) hud_draw_hz(backend_get_hz());

        /* Grid always scrolls */
        w.z_phase = S16(w.z_phase + w.cam_zspeed);
        if (w.z_phase >= GRID_ZSTEP) w.z_phase = S16(w.z_phase - GRID_ZSTEP);

        bool flash   = false;
        bool fired   = false;
        bool dropped = false;
        const RenderFlags *rf = &kStateFlags[state];

        int prev_state = state;
        switch (state) {
        case STATE_CRUISE:    state = state_cruise(&w, &fired, &dropped, keys, rs.peer_finished,
                                                    rs.remote.cam_x, rs.peer_rel_z, rs.remote_live); break;
        case STATE_CRASH:     state = state_crash(&w, &flash);                          break;
        case STATE_GATE:      state = state_gate(&w, keys, rs.peer_gate_ok);            break;
        case STATE_COUNTDOWN: state = state_countdown(&w);                              break;
        }
        gDebugState = (uint8_t)state;
        /* rs's fields above are last frame's — one frame of handshake skew is
         * fine and already absorbed by the RS_CRUISE clause of peer_gate_ok. */

        /* Alien contact: live alien crossing z=0 within FP_ONE laterally → crash */
        if (unlikely(state == STATE_CRUISE && alien_hit_player(&w))) {
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
                           w.race_result == RACE_WON;
        race_update(&rs, &state, rf->remote_player, &w, fired, dropped, player_won);
        if (unlikely(rs.link_changed)) hud_draw_mode(rs.bot_enabled, rs.link_state);

        apply_speed_modifiers(&w, &rs, state);

        /* Sound transitions last: a crash can come from the state machine,
         * an alien, or the peer's KILL — all of the above. */
        if (unlikely(state == STATE_CRASH && prev_state != STATE_CRASH)) {
            w.crash_timer = STUN_FRAMES;      /* stun: fixed length, music keeps
                                                * playing — hit sfx only */
            backend_snd_sfx(SND_ENMYHIT);
        }
        if (unlikely(state == STATE_GATE && prev_state == STATE_CRUISE &&
                     w.race_result == RACE_LOST)) {
            backend_snd_switch(SND_GAMEOVER); /* DEFEAT jingle at the gate */
            snd_slot = SND_GAMEOVER;
        }
        if (unlikely(state == STATE_COUNTDOWN && prev_state == STATE_GATE)) {
            /* This is the launch edge (race_start() just ran): GATE always
             * transitions through COUNTDOWN before CRUISE, so this is the
             * edge to watch for, not GATE->CRUISE directly — that one never
             * fires and would leave the peer FINISHED latch below stuck,
             * instantly losing every race after the first. */
            race_lap_reset(&rs);              /* clear peer FINISHED latch */
            /* backend_snd_switch always restarts its track from frame 0, so
             * gating on the last-switched slot avoids an audible restart of
             * the main theme on every winning lap while still recovering
             * from the defeat jingle above. */
            if (snd_slot != SND_MAIN) { backend_snd_switch(SND_MAIN); snd_slot = SND_MAIN; }
            backend_snd_sfx(SND_FIRE);
        }

        hud_update_race(state != STATE_GATE, w.lap, w.mines_left);

        backend_set_flash(flash);
        w.prev_keys = keys;
        w.frame++;
        if (unlikely(w.frame < min_frame)) continue;
        backend_clear();
        draw_world_plane(rf, &w, &rs);
        draw_alien_plane(rf, &w, &rs);
#ifdef VQ_PERF
        gPerfBucket = (uint8_t)state;   /* cost this frame under its GameState */
#endif
        backend_present(w.angleY, w.angleX);
    }

    serial_cleanup();
    backend_cleanup();
    return 0;
}
