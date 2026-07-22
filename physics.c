/* physics.c — game state machine, entity logic, and physics for vquest.
 * Unity-included from vquest.c (not a separate TU).
 * Requires: game constants, PhysicsState, GameState, RenderFlags from vquest.h,
 *           rendering functions from render.c (included before this file).   */

/* Entity/cruise half of the file (through state_crash): raised back to O3
 * under the global -Os Atari build (see OPT_ATARI in the Makefile).  This
 * covers the per-entity loops (update_alien_spawns, field_scroll,
 * update_missiles, missiles_hit_ghost, mines_hit_ghost) and the per-frame
 * state_cruise/state_crash transitions; together with backend_gemtos.c's O3
 * regions it recovers most of -Ofast's frame time at a fraction of its size.
 * The gate/bot/race half below (race_start onward) is once-per-frame,
 * branch-bound code with no hot loops for O3 to win on, so it stays at the
 * global -Os (see the pop_options right after state_crash).  render.c
 * deliberately stays -Os throughout (its C code is divide-bound — O3 there
 * measured +4.4 kB for <1k cycles/frame). */
#pragma GCC push_options
#pragma GCC optimize("O3")

static const RenderFlags kStateFlags[] = {
/*                     grid   gate   finish aliens credits remote */
/* STATE_CRUISE */   { true,  false, true,  true,  false,  true  },
/* STATE_CRASH  */   { true,  false, false, false, false,  false },
/* STATE_GATE   */   { false, true,  false, false, true,   false },
};

/* LCG_STEP — one step of the shared LCG (multiplier 2053, addend 13849).
 * Used by update_alien_spawns() for deterministic pseudo-random positions. */
#define LCG_STEP(seed) ((uint16_t)((uint16_t)(seed) * 2053u + 13849u))

#define CAM_ZSPEED_MAX_L1   256   /* lap-1 ceiling                               */
#define CAM_ZSPEED_LAP_STEP  64   /* per-lap gain: 256 / 320 / 384; power of two */
#define DRAFT_ZSPEED_CAP     32   /* how far drafting may exceed the lap ceiling */
#define CATCHUP_REL_Z   (8 * FP_ONE)  /* opponent lead that arms the boost;
                                      * drafting's window is <= 1*FP_ONE so
                                      * the two can never both apply       */
#define CATCHUP_ZSPEED_STEP  8   /* per-frame gain; must exceed THROTTLE_STEP
                                  * (over-cap bleed) and THROTTLE_DECAY      */
#define CATCHUP_ZSPEED_CAP  32   /* how far past the lap ceiling it may push */

/* zspeed_max_for_lap — ceiling for a 1-based lap, capped at CAM_ZSPEED_MAX so
 * every constant tuned against it keeps its margin.  Callers pass LOCAL laps
 * only (w->lap, b->lap), never a wire-decoded one.
 * Lap times at 30720 units: 120 / 96 / 80 frames (2.4 / 1.92 / 1.6 s).
 * Saturates at CAM_ZSPEED_MAX from lap 3 on, so with LAPS_PER_RACE=5 the
 * per-lap ceilings are 256/320/384/384/384 — a deliberate flat tail, not a
 * bug: raising CAM_ZSPEED_MAX would need re-sizing every despawn/z-phase
 * assert and the wire's 14-bit position fields. */
static inline int16_t zspeed_max_for_lap(uint8_t lap) {
    int16_t z = S16(CAM_ZSPEED_MAX_L1 + ((lap - 1) << 6));
    return z > CAM_ZSPEED_MAX ? CAM_ZSPEED_MAX : z;
}

/* ALIEN_DESPAWN_Z must stay below -(top speed + draft) or alien_hit_player's
 * (0, -cam_zspeed] crossing window is cut short; z_phase's single-subtract wrap
 * in main needs top speed < GRID_ZSTEP. */
_Static_assert(ALIEN_DESPAWN_Z < -(CAM_ZSPEED_MAX + DRAFT_ZSPEED_CAP), "despawn too near");
_Static_assert(CAM_ZSPEED_MAX + DRAFT_ZSPEED_CAP < GRID_ZSTEP, "z_phase wrap breaks");
_Static_assert(CATCHUP_ZSPEED_CAP <= DRAFT_ZSPEED_CAP,
               "despawn/z-phase asserts above are sized to DRAFT_ZSPEED_CAP");

/* apply_lateral — update vel_x and cam_x from Left/Right keys.
 * steer/vel_x_max are compile-time constants at each call site;
 * GCC constant-folds all branches when inlined. */
static inline void apply_lateral(int16_t steer, int16_t vel_x_max,
                                  uint8_t keys, int16_t *vel_x, int16_t *cam_x)
{
    if (keys & KEY_LEFT)  *vel_x = S16(*vel_x - steer);
    if (keys & KEY_RIGHT) *vel_x = S16(*vel_x + steer);
    *vel_x = S16(*vel_x - (*vel_x >> DRAG_SHIFT));
    if (*vel_x >  vel_x_max) *vel_x =  vel_x_max;
    if (*vel_x < -vel_x_max) *vel_x = -vel_x_max;
    *cam_x = S16(*cam_x + *vel_x);
}

/* ── Alien / missile helpers ─────────────────────────────────────────────── */

/* alien_gap — spawn spacing for this round's density (decision 8): shrinks
 * every round to a floor, so later rounds pack more aliens into the same
 * GRID_ZFAR+ALIEN_SPAWN_LEAD window.
 *
 * round is unbounded (one increment per race), so (round-1)*ALIEN_GAP_STEP
 * leaves int16_t range at round 65 and comes back around to a *positive* gap
 * at round 129 — under -mshort that resets alien density to round-1 levels,
 * while the 32-bit Linux build clamps correctly.  Computed at 32 bits so both
 * agree; once per race, so the width costs nothing. */
static inline int16_t alien_gap(int16_t round) {
    int32_t gap = (int32_t)ALIEN_GAP_BASE - (int32_t)(round - 1) * ALIEN_GAP_STEP;
    return gap < ALIEN_GAP_MIN ? ALIEN_GAP_MIN : S16(gap);
}

/* world_progress — how far down the current lap we are: the per-lap race
 * coordinate shared with the peer.  Shared by state_cruise's crossing branch
 * and race_update's my_progress; both need the identical uint16-wraparound
 * subtraction (finish_dist can run negative for a frame right after
 * crossing, before race_start/the mid-race branch resets it). */
static inline uint16_t world_progress(const World *w) {
    return U16W((uint16_t)LANDING_APPROACH_DIST - (uint16_t)w->finish_dist);
}

/* update_alien_spawns — materialize scheduled aliens entering the window.
 * World position is f(round, k) only, so race peers build identical fields
 * regardless of their own progress.
 *
 * The schedule advances unconditionally.  It used to `break` WITHOUT
 * advancing next_alien_pos when every slot was busy, which broke exactly the
 * determinism the comment above claims: two peers reach a given course
 * position with different slot occupancy (they are at different progress), so
 * one stalled the schedule where the other did not and their fields diverged
 * for the rest of the race.  Peer missiles are simulated locally against the
 * local field, so that divergence was observable, not cosmetic.
 *
 * Which slot an alien lands in is not observable — only the set of live
 * aliens is — so the free-slot search stays.  What matters is that the search
 * never fails: the spawn window is GRID_ZFAR + ALIEN_SPAWN_LEAD = 10240 deep
 * and live aliens persist back to ALIEN_DESPAWN_Z = -512, so at the
 * ALIEN_GAP_MIN floor at most (10240 + 512) / 1280 = 9 are alive at once
 * against ALIEN_COUNT = 10 slots.  The assert holds that margin; keying the
 * slot off alien_seq % ALIEN_COUNT instead was tried and does NOT work, because
 * the lap crossing rebases alien_seq by aliens_per_lap, which is not a
 * multiple of ALIEN_COUNT for most gaps and shifts the mapping onto live
 * slots. */
static void update_alien_spawns(World *w, uint16_t my_progress) {
    AlienField *a = &w->aliens;
    int16_t gap = w->alien_gap;
    for (;;) {
        int16_t rel = S16W(w->next_alien_pos - my_progress);   /* modular course sub */
        if (rel > (int16_t)(GRID_ZFAR + ALIEN_SPAWN_LEAD)) break;
        if (rel > ALIEN_ZMIN) {          /* skip ones already behind us */
            int slot = -1, i;
            for (i = 0; i < ALIEN_COUNT; i++)
                if (!a->alive[i]) { slot = i; break; }
            assert(slot >= 0);
            if (slot >= 0) {
                /* Unsigned: round*37 leaves int16_t range around round 886,
                 * and signed overflow is UB even where the modular result
                 * would have been the one we want. */
                uint16_t r   = LCG_STEP(U16W((uint16_t)w->round * 37u
                                             + w->alien_seq * 13u));
                /* magnitude in [FP_ONE, 3*FP_ONE) — span 2*FP_ONE is a power of
                 * two, so a mask (one AND) replaces an expensive %; bits 1-11
                 * feed it while bit 15 stays reserved for the sign so the two
                 * are independent. */
                int16_t mag  = S16(FP_ONE + ((r >> 1) & (2 * FP_ONE - 1)));
                a->x[slot]     = (r & 0x8000) ? mag : S16(-mag);
                a->z[slot]     = rel;
                a->alive[slot] = true;
            }
        }
        w->alien_seq++;
        w->next_alien_pos = U16W(w->next_alien_pos + gap);
    }
}

/* field_hit_player — true if a live entity crossed z=0 this frame and is
 * within `tol` laterally.  Shared by alien_hit_player and the bot-mine
 * collision test in race_update (a real peer's mine hit arrives as their
 * KILL bit instead).  The z window (0, -cam_zspeed] covers exactly the
 * crossing frame so this does not re-trigger on subsequent frames after the
 * entity has passed.  Deactivates the hitting entry (mirrors
 * update_missiles' alien kill): entities don't advance while frozen in
 * STATE_CRASH, so a still-alive one would otherwise sit in this exact
 * window and re-trigger the instant the stun timer expires and the state
 * briefly reads STATE_CRUISE again, permanently locking the game in
 * STATE_CRASH.
 *
 * Takes a runtime `n` (not a compile-time entity count), so O3 can no
 * longer unroll this loop the way it could the old fixed-ALIEN_COUNT
 * version — a deliberate size-for-speed trade of maybe ~100 cycles/frame
 * against the ~160k budget. */
static bool field_hit_player(int16_t *x, int16_t *z, bool *alive, int n,
                             int16_t cam_x, int16_t cam_zspeed, int16_t tol)
{
    int i;
    for (i = 0; i < n; i++) {
        int16_t rel;
        if (!alive[i]) continue;
        if (z[i] > 0 || z[i] <= -cam_zspeed) continue;
        rel = S16(cam_x - x[i]);
        if (rel < 0) rel = S16(-rel);
        if (rel < tol) { alive[i] = false; return true; }
    }
    return false;
}

/* field_scroll — advance all live entities toward the camera each frame,
 * freeing any that have crossed the despawn depth so their slots recycle. */
static void field_scroll(int16_t *z, bool *alive, int n,
                         int16_t cam_zspeed, int16_t despawn_z)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!alive[i]) continue;
        z[i] = S16(z[i] - cam_zspeed);
        if (z[i] < despawn_z) { alive[i] = false; continue; }
    }
}

static bool alien_hit_player(World *w)
{
    return field_hit_player(w->aliens.x, w->aliens.z, w->aliens.alive, ALIEN_COUNT,
                            w->ps.cam_x, w->cam_zspeed, ALIEN_CRASH_TOL);
}

/* Scroll all live aliens toward the camera each frame, freeing any that have
 * passed us so their slots recycle for the rest of the lap. */
static void update_aliens(int16_t cam_zspeed, AlienField *a)
{
    field_scroll(a->z, a->alive, ALIEN_COUNT, cam_zspeed, ALIEN_DESPAWN_Z);
}

/* Fire: spawn one missile in the first free slot, rate-limited to one per
 * FIRE_COOLDOWN_FRAMES.  Returns true when a missile was actually spawned
 * (the FIRE event broadcast to the peer).  FIRE + KEY_DOWN drops a mine
 * instead (try_drop_mine) — the cooldown decrements before that check so it
 * keeps ticking on frames the mine gesture is used instead. */
#define FIRE_COOLDOWN_FRAMES 5
static bool try_fire_missile(World *w, uint8_t keys)
{
    MissileSet *m = &w->missiles;
    int i;
    if (w->ps.fire_cooldown > 0) { w->ps.fire_cooldown--; return false; }
    if (!(keys & KEY_FIRE) || (keys & KEY_DOWN)) return false;
    for (i = 0; i < MISSILE_COUNT; i++) {
        if (!m->alive[i]) {
            m->x[i]     = w->ps.cam_x;
            m->z[i]     = HLINE_ZMIN;
            m->vis_z[i] = HLINE_ZMIN;
            m->alive[i] = true;
            backend_snd_sfx(SND_FIRE);
            w->ps.fire_cooldown = FIRE_COOLDOWN_FRAMES;
            return true;
        }
    }
    return false;
}

/* Drop: mirrors try_fire_missile.  FIRE + KEY_DOWN places one mine in the
 * first free `mymines` slot at z=0 (our current position, keeping us
 * authoritative for the hit — see mines_hit_ghost), rate-limited to one per
 * MINE_DROP_COOLDOWN and ammo-limited to MINES_PER_RACE.  The cooldown
 * decrements before the key test, same shape as try_fire_missile, so it
 * keeps ticking on frames the FIRE gesture is used instead.  FIRE must be a
 * fresh press (edge, not the autofire-cooldown hold): otherwise a player
 * holding FIRE who taps DOWN to brake — a routine cruise action — would
 * silently spend a mine. */
static bool try_drop_mine(World *w, uint8_t keys)
{
    MineField *m = &w->mymines;
    int i;
    if (w->mine_cooldown > 0) { w->mine_cooldown--; return false; }
    if (w->mines_left == 0) return false;
    if (!(keys & KEY_FIRE) || !(keys & KEY_DOWN)) return false;
    if (w->prev_keys & KEY_FIRE) return false;   /* FIRE edge, not autofire hold */
    for (i = 0; i < MINE_COUNT; i++) {
        if (!m->alive[i]) {
            m->x[i]     = w->ps.cam_x;
            m->z[i]     = 0;
            m->alive[i] = true;
            w->mines_left--;
            w->mine_cooldown = MINE_DROP_COOLDOWN;
            return true;
        }
    }
    return false;
}

/* Advance all live missiles and test collision against all live aliens.
 * Takes the sets separately (not World) — race_update runs the peer's
 * missiles through the same sim against the same alien field. */
static void update_missiles(int16_t cam_zspeed, MissileSet *m, AlienField *a,
                            uint16_t *kills)
{
    int mi;
    int16_t missile_speed = S16(cam_zspeed * MISSILE_SPEED_FACTOR);
    for (mi = 0; mi < MISSILE_COUNT; mi++) {
        int ai;
        if (!m->alive[mi]) continue;
        m->z[mi] = S16(m->z[mi] + missile_speed);
        if (m->z[mi] > GRID_ZFAR) { m->alive[mi] = false; continue; }
        /* Visual depth: ×1.5/frame (shift, no mul), clamped so the next
         * ×1.5 can't overflow int16.  Fixed rate — draw_missile's bolt
         * flight looks the same at any cam_zspeed. */
        {
            int16_t v = S16(m->vis_z[mi] + (m->vis_z[mi] >> 1));
            m->vis_z[mi] = v > GRID_ZFAR ? GRID_ZFAR : v;
        }
        for (ai = 0; ai < ALIEN_COUNT; ai++) {
            int16_t rel, aim_tol;
            if (!a->alive[ai]) continue;
            if (a->z[ai] <= 0) continue;             /* already passed player */
            if (m->z[mi] < a->z[ai]) continue;
            if (m->z[mi] > a->z[ai] + missile_speed + cam_zspeed) continue;
            rel = S16(m->x[mi] - a->x[ai]);
            /* Tolerance matches alien visual half-width: max(ALIEN_SCALE_W, ALIEN_MIN_PIX*z)/FOCAL.
             * gcc folds the constants into asr+add sequences, no muls/divs emitted. */
            aim_tol = ALIEN_SCALE_W / FOCAL;                         /* 48, constant      */
            { int16_t t = a->z[ai] * ALIEN_MIN_PIX / FOCAL;         /* 3*z/128           */
              if (t > aim_tol) aim_tol = t; }
            if (rel > -aim_tol && rel < aim_tol) {
                a->alive[ai] = false;
                m->alive[mi] = false;
                backend_snd_sfx(SND_ENMYHIT);
                if (kills) (*kills)++;
                break;   /* spent: without this the dead missile kept scanning
                          * and could take out every alien sharing its z window
                          * (4*cam_zspeed wide, > one alien_gap at top speed),
                          * each with its own kill and hit sfx */
            }
        }
    }
}

/* missiles_hit_ghost — test all live missiles in a set against one ghost
 * target (the remote player, or ourselves for bot missiles) at camera-
 * relative depth rel_z.  Same z-window/tolerance shape as the alien branch
 * of update_missiles().  Kills the hitting missile and returns true.
 *
 * min_tol — lateral floor of the hit window in world units:
 *   ALIEN_SCALE_W/FOCAL (= alien parity) for our missiles vs the ghost,
 *   FP_ONE/4 (= alien_hit_player parity) for peer missiles crossing us at
 *   rel_z = 1 (their missiles fly forward, so they reach us only from behind).
 *
 * noinline: called twice per frame from main; -Ofast would inline both copies
 * for no measurable gain (call overhead ≈ 50 cycles vs the 160k frame budget). */
static __attribute__((noinline)) bool missiles_hit_ghost(int16_t cam_zspeed,
    MissileSet *m, int16_t ghost_x, int16_t rel_z, int16_t min_tol)
{
    int mi;
    int16_t missile_speed = S16(cam_zspeed * MISSILE_SPEED_FACTOR);
    if (rel_z <= 0 || rel_z > GRID_ZFAR) return false;
    for (mi = 0; mi < MISSILE_COUNT; mi++) {
        int16_t rel, aim_tol;
        if (!m->alive[mi]) continue;
        if (m->z[mi] < rel_z) continue;
        if (m->z[mi] > rel_z + missile_speed + cam_zspeed) continue;
        rel = S16(m->x[mi] - ghost_x);
        if (rel < 0) rel = S16(-rel);
        aim_tol = min_tol;
        { int16_t t = S16(rel_z * ALIEN_MIN_PIX / FOCAL);
          if (t > aim_tol) aim_tol = t; }
        if (rel < aim_tol) {
            m->alive[mi] = false;
            return true;
        }
    }
    return false;
}

/* mines_hit_ghost — our world-fixed mines vs a racer at camera-relative
 * depth rel_z.  Unlike missiles_hit_ghost there is no z window: the mine is
 * consumed the instant the target reaches it (d <= 0) whether or not it
 * connects, so it can neither re-trigger nor need a previous-frame sample.
 * Sampling dx one frame late costs at most CRUISE_VEL_X_MAX=48 units of
 * lateral error against a 256-unit tolerance.
 *
 * noinline: called once per frame from race_update; matches
 * missiles_hit_ghost's rationale (call overhead ≈ 50 cycles vs the 160k
 * frame budget). */
static __attribute__((noinline)) bool mines_hit_ghost(MineField *m,
    int16_t ghost_x, int16_t rel_z)
{
    int i; bool hit = false;
    if (rel_z > 0) return false;   /* our mines only threaten someone behind us;
                                    * also bounds d to int16 — both terms are then
                                    * in [-LANDING_APPROACH_DIST, 0] */
    for (i = 0; i < MINE_COUNT; i++) {
        int16_t d, dx;
        if (!m->alive[i]) continue;
        d = S16(m->z[i] - rel_z);
        if (d > 0) continue;                   /* still ahead of them */
        m->alive[i] = false;                   /* reached: consume regardless */
        dx = S16(m->x[i] - ghost_x);
        if (dx < 0) dx = S16(-dx);
        if (dx < MINE_HIT_TOL) hit = true;
    }
    return hit;
}

/* ── State update functions ───────────────────────────────────────────────── */

static GameState state_cruise(World *w, bool *fired, bool *dropped, uint8_t keys,
                              bool peer_finished)
{
    /* Throttle: Up/Down are free in cruise (no vertical control here).
     * Racing trade-off — faster reaches the finish line sooner but leaves
     * less time to dodge aliens and line up shots.
     * The ceiling is bled back toward every frame, not just while holding
     * Up: the old clamp lived only inside the KEY_UP branch, so drafting
     * (vquest.c) without throttling accumulated cam_zspeed with nothing
     * pulling it back — an unbounded-growth bug that outran
     * ALIEN_DESPAWN_Z/the z_phase wrap once past a few hundred frames.
     * Below the ceiling, cam_zspeed also bleeds toward CAM_ZSPEED_BASE
     * (not CAM_ZSPEED_MIN) when Up isn't held, so throttle is an active
     * control rather than cruise control: releasing Up settles back to
     * base speed, never all the way down to the brake floor.  Below base
     * (post-brake or post-crash) there is no automatic recovery — climbing
     * back is the player's Up key, or auto-acceleration would just
     * reappear through the back door. */
    {
        int16_t zmax = zspeed_max_for_lap(w->lap);
        if (w->cam_zspeed > zmax)
            w->cam_zspeed = S16(w->cam_zspeed - THROTTLE_STEP);
        else if (!(keys & KEY_UP) && w->cam_zspeed > CAM_ZSPEED_BASE)
            w->cam_zspeed = S16(w->cam_zspeed - THROTTLE_DECAY);
        if (keys & KEY_UP) {
            w->cam_zspeed = S16(w->cam_zspeed + THROTTLE_STEP);
            if (w->cam_zspeed > zmax) w->cam_zspeed = zmax;
        }
    }
    if (keys & KEY_DOWN) {
        w->cam_zspeed = S16(w->cam_zspeed - THROTTLE_STEP);
        if (w->cam_zspeed < CAM_ZSPEED_MIN) w->cam_zspeed = CAM_ZSPEED_MIN;
    }
    apply_lateral(CRUISE_STEER, CRUISE_VEL_X_MAX, keys, &w->ps.vel_x, &w->ps.cam_x);
    w->finish_dist = S16(w->finish_dist - w->cam_zspeed);
    /* Race ends the instant either side crosses: racing out a race you've
     * already lost (or making the winner wait for the loser) just feels
     * like standing around, so peer_finished ends it here too instead of
     * only once our own finish_dist reaches zero. */
    if (w->finish_dist <= 0 || peer_finished) {
        bool crossed = w->finish_dist <= 0;
        if (crossed) {                       /* per-lap timing, our own crossings only */
            w->lap_frames = U16W(w->frame - w->lap_start_frame);
            if (w->best_lap_frames == 0 || w->lap_frames < w->best_lap_frames)
                w->best_lap_frames = w->lap_frames;
            w->lap_start_frame = w->frame;
        }
        if (crossed && !peer_finished && w->lap < LAPS_PER_RACE) {
            /* Mid-race lap: stay in CRUISE.  += not =, to preserve the up-to-
             * 191-unit overshoot.  next_alien_pos drops by the same amount so
             * it and world_progress() fall together and every live alien's
             * camera-frame z needs no fixup; alien_seq drops by
             * aliens_per_lap to hold the schedule invariant
             *     next_alien_pos == ALIEN_Z_MARGIN + (alien_seq+1)*alien_gap
             * so the same course position always draws the same LCG
             * lateral.  Entities are deliberately NOT cleared: next lap's
             * aliens are already on screen (see race_start). */
            assert(w->next_alien_pos >= (uint16_t)LANDING_APPROACH_DIST + ALIEN_Z_MARGIN);
            w->lap++;
            w->finish_dist    = S16(w->finish_dist + LANDING_APPROACH_DIST);
            w->next_alien_pos = U16W(w->next_alien_pos - LANDING_APPROACH_DIST);
            w->alien_seq      = U16W(w->alien_seq - w->aliens_per_lap);
        } else {
            w->race_frames   = U16W(w->frame - w->race_start_frame);
            w->race_finished = true;
            w->lap_result    = peer_finished ? LAP_LOST : LAP_WON;
            w->gate_timer    = GATE_MIN_FRAMES;
            w->round++;
            hud_draw(w->round);              /* tally now counts RACES completed */
            return STATE_GATE;
        }
    }
    update_alien_spawns(w, world_progress(w));
    update_aliens(w->cam_zspeed, &w->aliens);
    /* Mines scroll from here too (frozen during STATE_CRASH along with
     * everything else state_cruise drives): a stunned leader's mines must
     * not keep drifting toward whoever is chasing them. */
    field_scroll(w->mines.z,   w->mines.alive,   MINE_COUNT, w->cam_zspeed, ALIEN_DESPAWN_Z);
    field_scroll(w->mymines.z, w->mymines.alive, MINE_COUNT, w->cam_zspeed, MINE_DESPAWN_Z);
    *fired   = try_fire_missile(w, keys);
    *dropped = try_drop_mine(w, keys);
    return STATE_CRUISE;
}

/* state_crash — the stun: crashing (alien contact or an enemy missile) costs
 * time and speed, not the lap.  finish_dist/aliens are frozen because only
 * state_cruise advances them — that IS the time penalty; on recovery
 * cam_zspeed drops by CRASH_ZSPEED_PENALTY (floored at CAM_ZSPEED_MIN, not
 * hard-reset to it) as the speed penalty.  No round/lap touches. */
static GameState state_crash(World *w, bool *flash)
{
    *flash = true;
    if (--w->crash_timer <= 0) {
        w->cam_zspeed = S16(w->cam_zspeed - CRASH_ZSPEED_PENALTY);
        if (w->cam_zspeed < CAM_ZSPEED_MIN) w->cam_zspeed = CAM_ZSPEED_MIN;
        return STATE_CRUISE;              /* same lap, progress kept */
    }
    return STATE_CRASH;
}

#pragma GCC pop_options

/* race_start — reset per-race state; called on every GATE→CRUISE launch.
 * Does NOT touch lap_result (the gate shows the last verdict until the
 * next crossing overwrites it).  Entities are cleared HERE ONLY: a mid-race
 * lap crossing (see the crossing branch in state_cruise) must not clear
 * them — the spawn window reaches GRID_ZFAR + ALIEN_SPAWN_LEAD = 10240 units
 * ahead, so the next lap's aliens are already on screen when you cross. */
static void race_start(World *w) {
    int i;
    w->lap              = 1;
    w->finish_dist       = LANDING_APPROACH_DIST;
    w->race_finished     = false;
    w->gate_ready        = false;
    w->race_parity      ^= 1;
    w->alien_kills       = 0;                 /* per RACE now, not per lap */
    w->race_start_frame  = w->frame;          /* feeds race_frames at the gate */
    w->lap_start_frame   = w->frame;
    w->cam_zspeed        = CAM_ZSPEED_BASE;   /* moved from the old zspeed_for_round call */
    w->alien_gap         = alien_gap(w->round);
    w->aliens_per_lap    = U16W(LANDING_APPROACH_DIST / w->alien_gap);  /* 1 divide/race */
    w->next_alien_pos    = U16W(ALIEN_Z_MARGIN + w->alien_gap);
    w->alien_seq         = 0;
    w->mines_left        = MINES_PER_RACE;
    w->mine_cooldown     = 0;
    for (i = 0; i < ALIEN_COUNT; i++)   w->aliens.alive[i]   = false;
    for (i = 0; i < MISSILE_COUNT; i++) w->missiles.alive[i] = false;
    for (i = 0; i < MINE_COUNT; i++) {
        w->mines.alive[i]   = false;
        w->mymines.alive[i] = false;
    }
}

/* state_gate — between-laps victory/defeat screen: spins the logo, latches
 * gate_ready (and the GET READY prompt) the instant FIRE is pressed — even
 * during the dwell, so a press is never silently swallowed — and launches
 * the next lap once the dwell has elapsed and both the local player and the
 * peer are ready (decision 4).  gate_ready alone is NOT broadcast as READY
 * (see race_update's my_rs) until the dwell has also elapsed, so holding
 * FIRE through the dwell can't make the bot/peer launch early. */
static GameState state_gate(World *w, uint8_t keys, bool peer_gate_ok)
{
    w->angleY = S16W(w->angleY + w->angleYinc);   /* spin the logo; wraps, masked by fastSin */
    w->angleX = S16W(w->angleX + w->angleXinc);
    if (w->gate_timer > 0) w->gate_timer--;
    if (keys & KEY_FIRE)  w->gate_ready = true;
    if (w->gate_timer <= 0 && w->gate_ready && peer_gate_ok) {
        race_start(w);
        return STATE_CRUISE;
    }
    return STATE_GATE;
}

/* ── Computer opponent ────────────────────────────────────────────────────── *
 * Drives the remote-player slot when no serial peer is heard, producing the
 * same RemoteState that serial_recv() fills — everything downstream (ghost,
 * peer missiles, kills) is source-agnostic.  Cost per frame: a handful of
 * int16 adds/compares; one 16-bit mul (LCG) every 32nd frame; the 8-alien
 * scan only on frames its fire cooldown has expired.                        */

#define BOT_WAIT_FRAMES     50         /* at the gate before READY (mirrors dwell)  */
#define BOT_STEER           24         /* max lateral step/frame toward lane   */
#define BOT_FIRE_COOLDOWN   20         /* slower trigger than the player's 5   */
#define BOT_AIM_TOL  ((int16_t)(FP_ONE / 4))  /* lateral window to take a shot */

/* Bot personality — re-rolled on every race launch (see bot_update's
 * RS_READY case) from an LCG seeded with the frame reached, affects
 * throttle, firing priority, gate dwell, and steering jitter.  Three
 * distinct styles so races feel different. */
#define BOT_AGGRESSIVE   0   /* pushes speed, shoots player first, short gate wait  */
#define BOT_DEFENSIVE    1   /* hangs back, shoots aliens first, long gate wait     */
#define BOT_UNPREDICTABLE 2  /* wide speed variance, random priority, quick jitters */

typedef struct {
    uint8_t  state;      /* RS_*                              */
    int16_t  timer;      /* RS_DEAD / RS_WAIT countdown        */
    int16_t  cam_x, target_x;
    uint16_t progress;
    int16_t  zspeed;
    int16_t  round;
    int8_t   cooldown;
    uint16_t lcg;
    bool     finished;
    uint8_t  race_parity; /* 1-bit, flips at every race launch (was `lap`)   */
    uint8_t  lap;         /* lap in race, 1..LAPS_PER_RACE                  */
    uint8_t  personality; /* BOT_AGGRESSIVE / BOT_DEFENSIVE / BOT_UNPREDICTABLE */
    uint8_t  gate_wait;   /* frames at the gate before READY (varies by personality) */
    uint8_t  mines_left;  /* drops remaining this race                      */
    int8_t   mine_cooldown;
} Bot;

static void bot_init(Bot *b) {
    b->round       = 1;
    b->cam_x       = CAM_X_INIT;
    b->target_x    = CAM_X_INIT;
    b->cooldown    = BOT_FIRE_COOLDOWN;
    b->lcg         = 0x5EED;
    b->state       = RS_WAIT;
    b->progress    = 0;
    b->finished    = false;
    b->race_parity = 0;
    b->lap         = 1;
    b->mines_left    = MINES_PER_RACE;
    b->mine_cooldown = 0;
    /* Personality is rolled on every RS_READY->RS_CRUISE launch (see
     * bot_update), not here: at this point the game has not run a single
     * frame yet, so there is no variation to seed the LCG from. */
    b->gate_wait   = BOT_WAIT_FRAMES;
    b->timer       = b->gate_wait;
}

/* bot_kill — our missile hit the bot: same stun as the player, progress and
 * round kept; it resumes RS_CRUISE mid-lap on expiry. */
static void bot_kill(Bot *b) {
    b->state = RS_DEAD;
    b->timer = STUN_FRAMES;
}

/* bot_update — advance the bot one frame and emit its RemoteState.
 * aliens->z[] is in the *local* camera frame; my_progress - bot progress
 * converts it to the bot's frame for aiming (aliens are world-fixed).
 * player_going: local player is RS_READY or RS_CRUISE — gates the bot's own
 * launch out of RS_READY, mirroring the human gate handshake.
 * force_finish: the player just won this lap — a real peer would apply the
 * mirror-image peer_finished check on their own machine and end their round
 * immediately too, so the bot (which runs no such independent simulation)
 * needs this external push to lose immediately instead of racing on to its
 * own finish line.
 * noinline: once per frame; keeping it out of main saves ~1KB of text. */
static __attribute__((noinline)) void bot_update(Bot *b, RemoteState *out,
    const AlienField *aliens, uint16_t my_progress, uint8_t my_lap, int16_t my_cam_x,
    uint16_t frame, bool player_going, bool force_finish)
{
    bool fire = false;
    bool mine = false;
    if (force_finish && b->state != RS_WAIT && b->state != RS_READY) {
        b->round++;
        b->finished = true;
        b->state    = RS_WAIT;
        b->timer    = b->gate_wait;
    }
    switch (b->state) {
    case RS_DEAD:
        if (--b->timer <= 0) {
            b->state = RS_CRUISE;   /* resume the lap where it was */
            /* Same speed penalty as the player's state_crash, or the bot
             * would be strictly advantaged by a hit (it otherwise applies
             * none at all). */
            b->zspeed = S16(b->zspeed - CRASH_ZSPEED_PENALTY);
            if (b->zspeed < CAM_ZSPEED_MIN) b->zspeed = CAM_ZSPEED_MIN;
        }
        break;
    case RS_WAIT:
        if (--b->timer <= 0) b->state = RS_READY;
        break;
    case RS_READY:
        if (player_going) {
            /* Re-rolled on every RS_READY->RS_CRUISE launch, not just the
             * first: folds in the frame count reached (which varies with the
             * player's own reaction time at the gate, unlike the fixed
             * boot-time seed) so personality differs race to race instead of
             * settling on one b->lcg % 3 result for the whole session. The
             * gate_wait this sets governs the *next* dwell, so the one just
             * used to reach this launch was rolled by the previous race —
             * acceptable skew. */
            b->lcg = LCG_STEP(U16W(b->lcg ^ frame));
            b->personality = (uint8_t)(b->lcg % 3);
            switch (b->personality) {
            case BOT_AGGRESSIVE:   b->gate_wait = 35; break;
            case BOT_DEFENSIVE:    b->gate_wait = 65; break;
            default:               b->gate_wait = BOT_WAIT_FRAMES; break;
            }
            b->state       = RS_CRUISE;
            b->progress    = 0;
            b->finished    = false;
            b->race_parity ^= 1;
            b->lap         = 1;
            b->zspeed      = CAM_ZSPEED_BASE;
            b->mines_left    = MINES_PER_RACE;
            b->mine_cooldown = 0;
        }
        break;
    case RS_CRUISE: {
        int16_t d;
        {
            /* Re-roll period: aggressive/unpredictable change lane faster. */
            uint16_t reroll_mask = (b->personality == BOT_DEFENSIVE) ? 63u : 31u;
            if ((frame & reroll_mask) == 0) {
                int16_t zmax = zspeed_max_for_lap(b->lap);
                if (rel_depth(my_lap, my_progress, b->lap, b->progress)
                        >= CATCHUP_REL_Z)
                    zmax = S16(zmax + CATCHUP_ZSPEED_CAP);
                b->lcg = LCG_STEP(b->lcg);
                switch (b->personality) {
                case BOT_AGGRESSIVE:
                    b->zspeed = S16(zmax - 48 + (b->lcg & 63));
                    break;
                case BOT_DEFENSIVE:
                    b->zspeed = S16(zmax - 128 + (b->lcg & 63));
                    break;
                default: /* BOT_UNPREDICTABLE: true min..max spread, not a
                          * narrow band around nominal. */
                    b->zspeed = S16(CAM_ZSPEED_MIN +
                        (b->lcg % U16W(zmax - CAM_ZSPEED_MIN + 1)));
                    break;
                }
                if (b->zspeed < CAM_ZSPEED_MIN) b->zspeed = CAM_ZSPEED_MIN;
                if (b->zspeed > zmax) b->zspeed = zmax;
                b->target_x = S16(((b->lcg >> 4) & 0x0FFF) - 2 * FP_ONE);
            }
        }
        b->progress = U16W(b->progress + b->zspeed);
        if (b->progress >= LANDING_APPROACH_DIST) {
            if (b->lap < LAPS_PER_RACE) {
                /* Mid-race lap: preserve the overshoot (-=, not =0), same
                 * reason as state_cruise's crossing branch. */
                b->lap++;
                b->progress = U16W(b->progress - LANDING_APPROACH_DIST);
            } else {
                b->round++;
                b->finished = true;
                b->state    = RS_WAIT;
                b->timer    = b->gate_wait;
                break;
            }
        }
        d = S16(b->target_x - b->cam_x);
        if (d >  BOT_STEER) d =  BOT_STEER;
        if (d < -BOT_STEER) d = -BOT_STEER;
        b->cam_x = S16(b->cam_x + d);
        {
            int16_t me_rel = rel_depth(my_lap, my_progress, b->lap, b->progress);
            int16_t dx     = S16(my_cam_x - b->cam_x);
            if (dx < 0) dx = S16(-dx);
            /* Fire priority and cooldown vary by personality:
             *   Aggressive:    player first, short cooldown
             *   Defensive:     aliens first, long cooldown
             *   Unpredictable: random priority, moderate cooldown
             * me_rel feeds mine-dropping below too, so the missile cooldown
             * gate only skips the targeting/fire logic, not this whole
             * block. */
            if (b->cooldown > 0) {
                b->cooldown--;
            } else {
                bool player_ok = (me_rel > REMOTE_Z_NEAR && me_rel < GRID_ZFAR
                                  && dx < BOT_AIM_TOL);
                bool alien_ok = false;
                if (b->personality == BOT_DEFENSIVE ||
                    (b->personality == BOT_UNPREDICTABLE && (b->lcg & 1))) {
                    /* Aliens first, fall back to player. */
                    int i;
                    for (i = 0; i < ALIEN_COUNT; i++) {
                        int32_t az; int16_t ax;
                        if (!aliens->alive[i]) continue;
                        az = (int32_t)aliens->z[i] + me_rel;
                        if (az < ALIEN_ZMIN || az > GRID_ZFAR) continue;
                        ax = S16(aliens->x[i] - b->cam_x);
                        if (ax < 0) ax = S16(-ax);
                        if (ax < BOT_AIM_TOL) { alien_ok = true; break; }
                    }
                    fire = alien_ok || player_ok;
                } else {
                    /* Player first (aggressive, or unpredictable's even-LCG frame). */
                    fire = player_ok;
                    if (!fire) {
                        int i;
                        for (i = 0; i < ALIEN_COUNT; i++) {
                            int32_t az; int16_t ax;
                            if (!aliens->alive[i]) continue;
                            az = (int32_t)aliens->z[i] + me_rel;
                            if (az < ALIEN_ZMIN || az > GRID_ZFAR) continue;
                            ax = S16(aliens->x[i] - b->cam_x);
                            if (ax < 0) ax = S16(-ax);
                            if (ax < BOT_AIM_TOL) { fire = true; break; }
                        }
                    }
                }
                if (fire) {
                    switch (b->personality) {
                    case BOT_AGGRESSIVE:   b->cooldown = 14; break;
                    case BOT_DEFENSIVE:    b->cooldown = 26; break;
                    default:               b->cooldown = BOT_FIRE_COOLDOWN; break;
                    }
                }
            }
            /* Mine: dropped blind when we're leading (me_rel < 0 — we can
             * neither see nor shoot a chaser behind us) and they're within
             * guessing range, on its own cooldown while ammo remains.  Same
             * blind proximity guess a human relies on the opponent-distance
             * HUD for.  Also what gives the headless tests a mine to
             * collide with, since the ascii autopilot never presses
             * KEY_DOWN.  Personality tuning is a natural follow-up. */
            if (b->mine_cooldown > 0) {
                b->mine_cooldown--;
            } else if (b->mines_left > 0 && me_rel < 0 && me_rel > -3 * FP_ONE) {
                mine = true;
                b->mines_left--;
                b->mine_cooldown = MINE_DROP_COOLDOWN;
            }
        }
        break; }
    }
    out->state       = b->state;
    out->fire        = fire;
    out->kill        = false;   /* bot hits on us are detected locally */
    out->mine        = mine;
    out->cam_x       = b->cam_x;
    /* progress is reported only while racing; combined with a stale `lap`
     * this would be ambiguous, but at the gate `lap` reads the race-end
     * value (see race_start) so rel_depth() saturates and the ghost hides
     * (and drafting is off) rather than reporting a bogus depth. */
    out->progress    = (b->state == RS_CRUISE) ? b->progress : 0;
    out->finished    = b->finished;
    out->race_parity = b->race_parity;
    out->lap         = b->lap;
}


/* ── Race / multiplayer slot ─────────────────────────────────────────────── */

/* Frames without a peer packet before the bot takes over the remote slot
 * (~1s).  Peers send every frame once paired, so any stall past this is a
 * real loss; the first real packet hands control back to the wire peer. */
#define REMOTE_TIMEOUT_FRAMES 50
/* Consecutive packets carrying the KILL bit after our missile hits the peer,
 * so a lost byte cannot drop the kill (receiver edge-triggers on it). */
#define KILL_REPEAT 8

typedef struct {
    RemoteState remote;            /* last known peer/bot state            */
    int16_t  remote_idle;          /* saturating; starts timed-out         */
    MissileSet rmissiles;          /* peer missiles, local frame           */
    int16_t  kill_pending;         /* outgoing KILL packets left to send   */
    bool     kill_latched;         /* edge detector for incoming KILL      */
    Bot      bot;
    bool     bot_enabled;
    bool     ghost_show;           /* per-frame outputs for the renderer   */
    int16_t  ghost_z;
    int16_t  peer_rel_z;           /* rel_depth(peer, us): ghost, drafting, missiles, mines */
    bool     peer_finished;        /* latched: peer crossed their line this lap (decision 5) */
    bool     peer_gate_ok;         /* per-frame output: gate may release next frame */
    bool     remote_live;          /* per-frame: fresh peer or active bot   */
    uint16_t rx_count;             /* packets decoded this session (debug)  */
} RaceState;

static void race_init(RaceState *rs, bool bot_enabled) {
    memset(rs, 0, sizeof(*rs));
    rs->remote_idle = REMOTE_TIMEOUT_FRAMES;
    rs->bot_enabled = bot_enabled;
    bot_init(&rs->bot);
}

/* race_lap_reset — clear the peer FINISHED latch; called by main on every
 * GATE→CRUISE transition (the same frame race_start() runs). */
static void race_lap_reset(RaceState *rs) { rs->peer_finished = false; }

/* noinline is load-bearing for size: gcc's jump threading duplicates the
 * region of main()'s loop that this call sits in (specialising paths through
 * the state-machine compares — the call sequence appears twice in the
 * disassembly), so inlined code here is paid for twice.  One out-of-line
 * body + a call per frame is a net −1.6 kB of text; a plain static would be
 * re-inlined at -Ofast, so the attribute is required. */
static __attribute__((noinline))
void race_update(RaceState *rs, GameState *state, bool remote_player_flag,
    World *w, bool fired, bool dropped, bool player_won)
{
    const PhysicsState *ps = &w->ps;
    int16_t cam_zspeed     = w->cam_zspeed;

    /* Per-lap race coordinate shared with the peer: how far down the course
     * we are.  0 while not racing so both players start each lap level;
     * capped at the course length.  finish_dist can run negative for a
     * frame right after crossing (before race_start/the mid-race branch
     * resets it), so 30720 - finish_dist can exceed INT16_MAX: the subtraction is done in
     * uint16 (defined wraparound, same sub.w) instead of signed 16-bit int,
     * which would be UB under -mshort.
     * Emergent behaviour: combined with a `lap` field that keeps its
     * race-end value while not racing, a peer sitting at the gate reports
     * (lap=LAPS_PER_RACE, progress=0) — rel_depth() then saturates against
     * whichever lap we're on, so the ghost hides and drafting is off, which
     * is what we want, but it is accidental rather than checked here. */
    uint16_t my_progress = (*state == STATE_CRUISE)
        ? progress_clamp(world_progress(w)) : 0;

    /* Peer missiles run through the same sim against the same deterministic
     * alien field, so both machines agree on alien kills. */
    update_missiles(cam_zspeed, &rs->rmissiles, &w->aliens, NULL);

    /* Outgoing wire state, computed once (also feeds the bot's own gate
     * handshake below).  RS_READY requires the dwell to have elapsed too
     * (not just gate_ready, which latches on FIRE immediately for display) —
     * otherwise holding FIRE through our own dwell would broadcast READY
     * early and let the bot/peer launch before we're actually able to. */
    uint8_t my_rs = *state == STATE_CRUISE ? RS_CRUISE
                  : *state == STATE_CRASH  ? RS_DEAD
                  : (w->gate_ready && w->gate_timer <= 0) ? RS_READY : RS_WAIT;

    /* Remote slot: a serial peer when one is talking, the bot otherwise. */
    RemoteState rs_in;
    bool got = serial_recv(&rs_in);
    if (got)                                          { rs->remote_idle = 0; rs->rx_count++; }
    else if (rs->remote_idle < REMOTE_TIMEOUT_FRAMES) rs->remote_idle++;
    bool bot_active = rs->bot_enabled && rs->remote_idle >= REMOTE_TIMEOUT_FRAMES;
    rs->remote_live = rs->remote_idle < REMOTE_TIMEOUT_FRAMES || bot_active;
    if (!got && bot_active) {
        /* player_going: the local player is at the gate ready, or has JUST
         * launched the same mutual lap (progress < LAP_JOIN_MAX) — mirrors
         * peer_gate_ok's own RS_CRUISE clause below.  Without the progress
         * qualifier, "player is RS_CRUISE" is true for the player's ENTIRE
         * lap, so a bot that finishes first (i.e. every time the player is
         * defeated) would see it immediately upon reaching RS_READY and
         * launch its next lap solo, a full lap ahead of the still-racing
         * player — who then sits at the gate for an extra bot lap-cycle
         * before the desync-recovery fallback resyncs them. */
        bot_update(&rs->bot, &rs_in, &w->aliens,
                   my_progress, w->lap, ps->cam_x, w->frame,
                   my_rs == RS_READY ||
                   (my_rs == RS_CRUISE && w->lap == 1 && my_progress < (uint16_t)LAP_JOIN_MAX),
                   player_won);
        got = true;
    }
    if (got) rs->remote = rs_in;

    /* Recomputed every frame, not just on a packet: my_progress moves even
     * when the peer is silent.  Single source for the ghost, peer muzzle_z,
     * our missile-vs-ghost test, incoming mine placement, drafting, and the
     * HUD. */
    rs->peer_rel_z = rel_depth(rs->remote.lap, rs->remote.progress, w->lap, my_progress);

    if (got) {
        /* Latch FINISHED only for a packet tagged with our current race
         * parity: stale in-flight packets from the peer's previous
         * (already-won-or-lost) race must not poison this race's verdict. */
        if (rs->remote.finished && rs->remote.race_parity == w->race_parity)
            rs->peer_finished = true;
        if (rs->remote.fire && rs->remote.state != RS_DEAD) {
            int16_t muzzle_z = S16(rs->peer_rel_z + HLINE_ZMIN);
            int i;
            for (i = 0; i < MISSILE_COUNT; i++)
                if (!rs->rmissiles.alive[i]) {
                    rs->rmissiles.x[i]     = rs->remote.cam_x;
                    rs->rmissiles.z[i]     = muzzle_z;
                    /* HLINE_ZMIN, not muzzle_z: draw_remote_missile never
                     * reads vis_z, but update_missiles advances it ×1.5 every
                     * frame for every set it is given.  A peer behind us has
                     * peer_rel_z down to -LANDING_APPROACH_DIST, so muzzle_z
                     * reaches -30699 and the ×1.5 left int16_t range on the
                     * next frame.  Seeding it the way try_fire_missile does
                     * makes "vis_z stays in [HLINE_ZMIN, GRID_ZFAR]" hold for
                     * every MissileSet by construction. */
                    rs->rmissiles.vis_z[i] = HLINE_ZMIN;
                    rs->rmissiles.alive[i] = true;
                    break;
                }
        }
        /* Incoming mine (render-only event — see the mine authority model
         * in the race redesign plan: our hit detection against it is
         * bot-only below, a real peer's hit arrives as their KILL bit).
         * Spawns at their current depth; peer_rel_z > 0 excludes someone
         * behind us (can never matter) and < LANDING_APPROACH_DIST excludes
         * the clamp, where the true depth is unknown. */
        if (rs->remote.mine && rs->peer_rel_z > 0 &&
            rs->peer_rel_z < LANDING_APPROACH_DIST) {
            int i;
            for (i = 0; i < MINE_COUNT; i++)
                if (!w->mines.alive[i]) {
                    w->mines.x[i]     = rs->remote.cam_x;
                    w->mines.z[i]     = rs->peer_rel_z;
                    w->mines.alive[i] = true;
                    break;
                }
        }
        /* Their missile hit us (shooter-authoritative, edge-triggered). */
        if (rs->remote.kill && !rs->kill_latched && *state == STATE_CRUISE)
            *state = STATE_CRASH;
        rs->kill_latched = rs->remote.kill;
    }

    /* Bot shots at us are resolved locally (no wire to carry a KILL):
     * its missiles fly forward in our frame and cross us at z≈0. */
    if (bot_active && *state == STATE_CRUISE &&
        missiles_hit_ghost(cam_zspeed, &rs->rmissiles,
                           ps->cam_x, 1, (int16_t)(FP_ONE / 4)))
        *state = STATE_CRASH;

    /* Bot mines are resolved locally too, same reason — field_hit_player's
     * (0, -cam_zspeed] crossing window is exactly the alien-collision shape,
     * reused here via the shared helper. */
    if (bot_active && *state == STATE_CRUISE &&
        field_hit_player(w->mines.x, w->mines.z, w->mines.alive, MINE_COUNT,
                         ps->cam_x, cam_zspeed, MINE_HIT_TOL))
        *state = STATE_CRASH;

    /* Our missiles/mines vs the ghost: shooter detects the hit and tells the
     * peer via the KILL bit (the bot is killed directly).  The local
     * RS_DEAD override hides the ghost until its own state catches up. */
    if ((rs->remote_idle < REMOTE_TIMEOUT_FRAMES || bot_active) &&
        rs->remote.state != RS_DEAD) {
        bool hit = missiles_hit_ghost(cam_zspeed, &w->missiles,
                           rs->remote.cam_x, rs->peer_rel_z,
                           (int16_t)(ALIEN_SCALE_W / FOCAL));
        hit |= mines_hit_ghost(&w->mymines, rs->remote.cam_x, rs->peer_rel_z);
        if (hit) {
            backend_snd_sfx(SND_ENMYHIT);
            if (bot_active) bot_kill(&rs->bot);
            else            rs->kill_pending = KILL_REPEAT;
            rs->remote.state = RS_DEAD;
        }
    }

    /* Beacon until a peer is heard: sending costs 7 BIOS traps on Atari,
     * so single-player only pays them every 16th frame.  Both sides
     * beacon, so pairing completes within ~0.3s; once paired, send every
     * frame for smooth ghosting (and fall back to beaconing if the peer
     * goes quiet past the ghost timeout). */
    if (rs->remote_idle < REMOTE_TIMEOUT_FRAMES || (w->frame & 15) == 0) {
        RemoteState out;
        out.state       = my_rs;
        out.fire        = fired;
        out.kill        = rs->kill_pending > 0;
        out.finished    = w->race_finished;
        out.mine        = dropped;
        out.race_parity = w->race_parity;
        out.lap         = w->lap;
        out.cam_x       = ps->cam_x;
        out.progress    = my_progress;
        serial_send(&out);
        if (rs->kill_pending > 0) rs->kill_pending--;
    }

    /* Ghost placement: race-relative depth.  Drawn only when strictly ahead
     * of us — only the chaser sees the leader, never the reverse — and no
     * further than GRID_ZFAR, so visibility matches missiles_hit_ghost()'s
     * (0, GRID_ZFAR] window exactly: a ghost on screen is always a hittable
     * target.  Without the upper bound the ghost used to sit pinned at the
     * far clip plane, looking reachable, while a progress gap that wide made
     * it permanently unkillable.  A peer less than REMOTE_Z_NEAR ahead is
     * still drawn pinned at REMOTE_Z_NEAR, which bounds the focal_rcp
     * divide in draw_remote_player(). */
    bool peer_on  = (rs->remote_idle < REMOTE_TIMEOUT_FRAMES || rs->bot_enabled) &&
                    rs->remote.state != RS_DEAD;
    rs->ghost_show = peer_on && rs->peer_rel_z > 0 && rs->peer_rel_z <= GRID_ZFAR &&
                     remote_player_flag;
    rs->ghost_z    = CLAMP(rs->peer_rel_z, REMOTE_Z_NEAR, GRID_ZFAR);

    /* Gate-release handshake (decision 4): consumed next frame by
     * state_gate via main's rs.peer_gate_ok.  The RS_CRUISE clause exists
     * ONLY to absorb the 1-2-frame skew when the peer released the gate
     * first and never re-sends READY; the progress qualifier is what makes
     * it safe — without it, a winner holding FIRE at the gate would see the
     * still-racing loser's RS_CRUISE and relaunch alone, desynchronizing
     * all future laps.  A freshly launched peer is seen within a packet or
     * two (progress <= a few hundred); a mid-lap peer is always far past
     * LAP_JOIN_MAX. */
    rs->peer_gate_ok =
        (rs->remote_idle >= REMOTE_TIMEOUT_FRAMES && !rs->bot_enabled) ||
        rs->remote.state == RS_READY ||
        (rs->remote.state == RS_CRUISE && rs->remote.lap == 1 &&
         rs->remote.progress < (uint16_t)LAP_JOIN_MAX);
}

/* apply_speed_modifiers — the three adjustments main() makes to cam_zspeed
 * after race_update, in the order that leaves the anti-cheat clamp the final
 * word.  All three are cruise-only, so the state test is hoisted here.
 *
 * noinline for the same reason as race_update above: gcc's jump threading
 * duplicates the region of main()'s loop these sit in, so anything inlined
 * here is paid for twice in text. */
static void apply_speed_modifiers(World *w, const RaceState *rs, GameState state)
{
    if (state != STATE_CRUISE) return;

    /* Drafting: a small speed bonus when chasing close behind the opponent
     * (≤ 1 world unit, ~0.16 s at base speed).  Incentivises tight racing
     * and rewards the chaser for staying in the leader's slipstream.
     * Capped at DRAFT_ZSPEED_CAP over this lap's ceiling: state_cruise's
     * bleed-off (physics.c) pulls any excess back down every frame, so
     * the bonus is a temporary slipstream, not a permanent one — without
     * the cap it grew unbounded on a player drafting without throttling
     * (nothing else clamped cam_zspeed on that path).  The per-frame gain
     * must exceed THROTTLE_STEP (the bleed-off) or the two cancel and the
     * cap is never reached. */
    if (rs->ghost_show && rs->peer_rel_z > 0 && rs->peer_rel_z <= FP_ONE) {
        int16_t cap = S16(zspeed_max_for_lap(w->lap) + DRAFT_ZSPEED_CAP);
        w->cam_zspeed = S16(w->cam_zspeed + 12);
        if (w->cam_zspeed > cap) w->cam_zspeed = cap;
    }

    /* Catch-up: the mirror of drafting — a racer dropped well behind
     * (>= CATCHUP_REL_Z) gains speed toward zmax + CATCHUP_ZSPEED_CAP,
     * and state_cruise's bleed pulls the excess back once the gap
     * closes.  Gain must exceed the bleed or they cancel (same rule as
     * drafting above). */
    if (rs->remote_live && rs->peer_rel_z >= CATCHUP_REL_Z) {
        int16_t cap = S16(zspeed_max_for_lap(w->lap) + CATCHUP_ZSPEED_CAP);
        w->cam_zspeed = S16(w->cam_zspeed + CATCHUP_ZSPEED_STEP);
        if (w->cam_zspeed > cap) w->cam_zspeed = cap;
    }

    /* Anti-cheat: aliens/mines never spawn past GRID_XHALF (the visible
     * ground grid's edge), so parking outside it would dodge every
     * hazard forever.  A hard clamp, not a drain: falling behind while
     * off-grid arms the catch-up boost above, which would outrun a mere
     * per-frame subtraction (verified — a drain-based first attempt let
     * cam_zspeed climb right back up once the self-inflicted gap armed
     * catch-up).  Placed last, after every other cam_zspeed adjustment
     * this frame, so it is unconditionally the final word: no combination
     * of drafting/catch-up/throttle can leave the ship faster than
     * CAM_ZSPEED_MIN while off-grid. */
    if (w->ps.cam_x > GRID_XHALF || w->ps.cam_x < -GRID_XHALF)
        w->cam_zspeed = CAM_ZSPEED_MIN;
}
