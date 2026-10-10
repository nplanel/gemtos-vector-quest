/* physics.c — game state machine, entity logic, and physics for vquest.
 * Unity-included from vquest.c (not a separate TU), followed by bot.c (the
 * computer opponent) and race.c (the remote-player slot).
 * Requires: game constants, PhysicsState, GameState, World from vquest.h,
 *           rendering functions from render.c (included before this file).   */

/* Optimisation note: this file (like bot.c and race.c) compiles at the global -Os (Makefile OPT).
 * Raising it to O3 measured +6.4 kB of text for ~3k cycles per cruise frame
 * (2026-10, VBL-exact `make perf`) — not worth it.  Re-measure before
 * reintroducing a pragma island here. */

/* LCG_STEP — one step of the shared LCG (multiplier 2053, addend 13849).
 * Used by update_alien_spawns() for deterministic pseudo-random positions. */
#define LCG_STEP(seed) ((uint16_t)((uint16_t)(seed) * 2053u + 13849u))

#define CAM_ZSPEED_MAX_L1   256   /* lap-1 ceiling                               */
#define CAM_ZSPEED_LAP_STEP  64   /* per-lap gain: 256 / 320 / 384; power of two */
#define DRAFT_ZSPEED_CAP     32   /* how far drafting may exceed the lap ceiling */
#define DRAFT_ZSPEED_STEP    12   /* per-frame drafting gain; > THROTTLE_STEP    */
#define CATCHUP_REL_Z   (3 * FP_ONE)  /* opponent lead that arms the boost.
                                      * Well inside GRID_ZFAR (8*FP_ONE, the
                                      * far clip): the boost closes the gap
                                      * until the opponent sits mid-grid and
                                      * clearly shootable, not merely at the
                                      * horizon where it first appears.  Above
                                      * drafting's <= 1*FP_ONE window, so the
                                      * two can never both apply, and the 1..3
                                      * unit neutral band between them is where
                                      * a trailing racer settles for the kill. */
#define CATCHUP_ZSPEED_STEP 12   /* per-frame gain; must exceed THROTTLE_STEP
                                  * (over-cap bleed) and THROTTLE_DECAY, and
                                  * big enough to reach the cap in a few frames
                                  * so the boost bites straight after a stun   */
#define CATCHUP_ZSPEED_CAP  96   /* how far past the *leader's* lap ceiling it
                                  * may push (see apply_speed_modifiers).  Peak
                                  * speed CAM_ZSPEED_MAX+96 = 480 stays under
                                  * the 512 despawn margin.  Closes ~94 px/frame
                                  * on a level-lap leader, so a stun's ~7.5-unit
                                  * deficit recovers in well under a lap.       */
#define CATCHUP_TAPER_SHIFT  4   /* boost = (gap - CATCHUP_REL_Z) >> this, so it
                                  * fades out as the gap closes (see the catch-up
                                  * block in apply_speed_modifiers for why) */

/* catchup_boost — how far above the lap ceiling a racer's catch-up target may
 * sit, given `gap` (their rel_depth deficit).  Caller must ensure
 * gap >= CATCHUP_REL_Z; shared by the player's continuous per-frame boost
 * (apply_speed_modifiers) and the bot's re-rolled-every-32-frames one, so the
 * two settle into the same shootable-not-overtaking behaviour. */
static inline int16_t catchup_boost(int16_t gap) {
    int16_t boost = S16(gap - CATCHUP_REL_Z) >> CATCHUP_TAPER_SHIFT;
    return boost > CATCHUP_ZSPEED_CAP ? CATCHUP_ZSPEED_CAP : boost;
}

/* zspeed_max_for_lap — ceiling for a 1-based lap, capped at CAM_ZSPEED_MAX so
 * every constant tuned against it keeps its margin.  That clamp also makes
 * it safe on a wire-decoded lap (1..8): apply_speed_modifiers' catch-up passes
 * the leader's lap, which may be the peer's.
 * Lap times at 30720 units: 120 / 96 / 80 frames (2.4 / 1.92 / 1.6 s).
 * Saturates at CAM_ZSPEED_MAX from lap 3 on, so with LAPS_PER_RACE=5 the
 * per-lap ceilings are 256/320/384/384/384 — a deliberate flat tail, not a
 * bug: raising CAM_ZSPEED_MAX would need re-sizing every despawn/z-phase
 * assert and the wire's 14-bit position fields. */
static inline int16_t zspeed_max_for_lap(uint8_t lap) {
    int16_t z = S16(CAM_ZSPEED_MAX_L1 + ((lap - 1) << 6));
    return z > CAM_ZSPEED_MAX ? CAM_ZSPEED_MAX : z;
}

/* zspeed_after_stun — the speed penalty on leaving a stun: a bounded
 * subtract, floored at the brake floor.  The single definition for the
 * player (state_crash) and the bot (its RS_DEAD -> RS_CRUISE edge): if the
 * two ever differed, a hit would favour one of them. */
static inline int16_t zspeed_after_stun(int16_t zspeed) {
    zspeed = S16(zspeed - CRASH_ZSPEED_PENALTY);
    return zspeed < CAM_ZSPEED_MIN ? CAM_ZSPEED_MIN : zspeed;
}

/* Largest cam_zspeed excess over the lap ceiling: drafting and catch-up are
 * mutually exclusive (their gap windows don't overlap), so the peak is the
 * greater of the two caps, not their sum. */
#define OVER_CEILING_MAX \
    (CATCHUP_ZSPEED_CAP > DRAFT_ZSPEED_CAP ? CATCHUP_ZSPEED_CAP : DRAFT_ZSPEED_CAP)

/* ALIEN_DESPAWN_Z must stay below -(top speed) or alien_hit_player's
 * (0, -cam_zspeed] crossing window is cut short; z_phase's single-subtract wrap
 * in main needs top speed < GRID_ZSTEP.  Top speed is the lap ceiling plus the
 * larger over-ceiling boost. */
_Static_assert(ALIEN_DESPAWN_Z < -(CAM_ZSPEED_MAX + OVER_CEILING_MAX), "despawn too near");
_Static_assert(CAM_ZSPEED_MAX + OVER_CEILING_MAX < GRID_ZSTEP, "z_phase wrap breaks");
/* Both boosts must out-gain state_cruise's over-ceiling bleed, or they cancel. */
_Static_assert(DRAFT_ZSPEED_STEP > THROTTLE_STEP, "drafting cancelled by bleed");
_Static_assert(CATCHUP_ZSPEED_STEP > THROTTLE_STEP, "catch-up cancelled by bleed");

/* apply_lateral — update vel_x and cam_x from Left/Right keys.
 * steer/vel_x_max are compile-time constants at each call site;
 * GCC constant-folds all branches when inlined. */
static inline void apply_lateral(int16_t steer, int16_t vel_x_max,
                                  uint8_t keys, int16_t *vel_x, int16_t *cam_x)
{
    if (keys & KEY_LEFT)  *vel_x = S16(*vel_x - steer);
    if (keys & KEY_RIGHT) *vel_x = S16(*vel_x + steer);
    /* Drag rounds the magnitude up on both sides so vel_x decays to 0.  A
     * bare >> floors: left drift would still die out, but right drift would
     * stall at +15 forever and slide the ship off the grid. */
    *vel_x = S16(*vel_x - (*vel_x > 0
                           ? (*vel_x + (1 << DRAG_SHIFT) - 1) >> DRAG_SHIFT
                           : *vel_x >> DRAG_SHIFT));
    if (*vel_x >  vel_x_max) *vel_x =  vel_x_max;
    if (*vel_x < -vel_x_max) *vel_x = -vel_x_max;
    *cam_x = S16(*cam_x + *vel_x);
}

/* ── Alien / missile helpers ─────────────────────────────────────────────── */

/* alien_gap — spawn spacing for this round's density: shrinks
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
    return U16W((uint16_t)LAP_LENGTH - (uint16_t)w->finish_dist);
}

/* free_slot — index of the first dead entry in an entity set's alive[], or
 * -1 when every slot is busy.  The one allocator for aliens, missiles and
 * mines (ours and the peer's). */
static int free_slot(const bool *alive, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!alive[i]) return i;
    return -1;
}

/* Opponent — last frame's view of the other racer, as the local simulation
 * needs it: built by main() from RaceState (defined further down, with the
 * wire/bot machinery) and read by state_cruise and the alien logic. */
typedef struct {
    int16_t x;          /* lateral position (their cam_x)              */
    int16_t rel_z;      /* rel_depth from us: > 0 means they lead      */
    bool    live;       /* fresh peer or active bot                    */
    bool    finished;   /* crossed their final line this race (latched) */
} Opponent;

/* update_alien_spawns — materialize scheduled aliens entering the window.
 * World SCHEDULE (z-position, timing, slot bookkeeping) is f(round, k) only,
 * so race peers build identical schedules regardless of their own progress —
 * see below for why lateral x is deliberately NOT part of that guarantee.
 *
 * The schedule advances unconditionally, even if no slot is free: two peers
 * reach a given course position with different slot occupancy (they are at
 * different progress), so stalling on a full field would diverge their fields
 * for the rest of the race — observably, since peer missiles are simulated
 * against the local field.
 *
 * Which slot an alien lands in is not observable — only the set of live
 * aliens is — so the free-slot search stays.  What matters is that the search
 * never fails: the spawn window is GRID_ZFAR + ALIEN_SPAWN_LEAD = 10240 deep
 * and live aliens persist back to ALIEN_DESPAWN_Z = -512, so at the
 * ALIEN_GAP_MIN floor at most (10240 + 512) / 1280 = 9 are alive at once
 * against ALIEN_COUNT = 10 slots.  The assert holds that margin.  Do not key
 * the slot off alien_seq % ALIEN_COUNT: the lap crossing rebases alien_seq by
 * aliens_per_lap, which is not a multiple of ALIEN_COUNT for most gaps and
 * shifts the mapping onto live slots.
 *
 * Lateral x is offset around a target racer (weighted toward whoever is
 * leading) rather than track-center, so no lane stays statistically safe —
 * see ALIEN_LEADER_BIAS_NUM in tuning.h.  This makes x depend on our cam_x
 * and opp->x, which are each peer's own locally-known (opp->x: network-
 * received, slightly stale) positions, so — unlike the schedule above — the two peers' x for
 * "the same" spawn WILL diverge from spawn time onward.  That's fine: it
 * only extends the divergence drift already causes post-spawn, and neither
 * peer's collision detection ever depends on the other side's copy. */
static void update_alien_spawns(World *w, uint16_t my_progress,
                                const Opponent *opp) {
    AlienField *a = &w->aliens;
    int16_t gap = w->alien_gap;
    int16_t my_x = w->ps.cam_x;
    for (;;) {
        int16_t rel = S16W(w->next_alien_pos - my_progress);   /* modular course sub */
        if (rel > (int16_t)(GRID_ZFAR + ALIEN_SPAWN_LEAD)) break;
        if (rel > ALIEN_ZMIN) {          /* skip ones already behind us */
            int slot = free_slot(a->alive, ALIEN_COUNT);
            assert(slot >= 0);
            if (slot >= 0) {
                /* Unsigned: round*37 leaves int16_t range around round 886,
                 * and signed overflow is UB even where the modular result
                 * would have been the one we want. */
                uint16_t r   = LCG_STEP(U16W((uint16_t)w->round * 37u
                                             + w->alien_seq * 13u));
                /* offset magnitude in [FP_ONE, 3*FP_ONE) around the target —
                 * span 2*FP_ONE is a power of two, so a mask (one AND)
                 * replaces an expensive %.  Bit layout of r: 1-11 magnitude,
                 * 12-13 target pick, 15 sign (14 unused) — all independent,
                 * one cheap LCG_STEP call. */
                int16_t mag      = S16(FP_ONE + ((r >> 1) & (2 * FP_ONE - 1)));
                int16_t offset   = (r & 0x8000) ? mag : S16(-mag);
                int16_t target_x = my_x;
                if (opp->live) {
                    bool opp_leads   = opp->rel_z > 0;
                    int16_t leader_x = opp_leads ? opp->x : my_x;
                    int16_t trail_x  = opp_leads ? my_x   : opp->x;
                    /* 3-in-4 (ALIEN_LEADER_BIAS_NUM) toward the leader, the
                     * rest toward the trailer, so trailing never goes safe. */
                    target_x = (((r >> 12) & 3) < ALIEN_LEADER_BIAS_NUM)
                                   ? leader_x : trail_x;
                }
                int16_t x = S16(target_x + offset);
                if (x > ALIEN_DRIFT_X_LIMIT)  x =  ALIEN_DRIFT_X_LIMIT;
                if (x < -ALIEN_DRIFT_X_LIMIT) x = S16(-ALIEN_DRIFT_X_LIMIT);
                a->x[slot]     = x;
                a->z[slot]     = rel;
                a->vx[slot]    = 0;
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
 * STATE_CRASH. */
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
        if (unlikely(rel < tol)) { alive[i] = false; return true; }
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
        if (z[i] < despawn_z) alive[i] = false;
    }
}

static bool alien_hit_player(World *w)
{
    return field_hit_player(w->aliens.x, w->aliens.z, w->aliens.alive, ALIEN_COUNT,
                            w->ps.cam_x, w->cam_zspeed, PLAYER_HIT_TOL);
}

/* alien_crash_index — index of the live alien an OPPONENT occupying
 * camera-relative depth racer_z and lateral racer_x is colliding with (its
 * centre within z_half of the racer's plane and x_tol laterally), or -1.
 * Shared by the two ways an opponent runs into an alien: the locally simulated
 * bot (bot_update, with a wider "sloppy pilot" x_tol) and a serial peer whose
 * crash we detect from its RS_DEAD edge (race_update, with the exact player
 * hitbox).  It does NOT clear the alien — the caller owns the policy (the bot
 * dodges most line-ups; a peer's reported crash is authoritative) — but when it
 * does clear, the alien leaves the shared world field for both racers, exactly
 * as an opponent's missile already does via update_missiles.  dz is 32-bit:
 * racer_z reaches ±LAP_LENGTH, so a->z[i] - racer_z can exceed int16
 * before the range test rejects it. */
static int alien_crash_index(const AlienField *a, int16_t racer_z,
                             int16_t racer_x, int16_t z_half, int16_t x_tol)
{
    int i;
    for (i = 0; i < ALIEN_COUNT; i++) {
        int32_t dz; int16_t ax;
        if (!a->alive[i]) continue;
        dz = (int32_t)a->z[i] - racer_z;
        if (dz < -z_half || dz >= z_half) continue;
        ax = S16(a->x[i] - racer_x);
        if (ax < 0) ax = S16(-ax);
        if (ax < x_tol) return i;
    }
    return -1;
}

/* update_alien_drift — steer each live alien's lateral velocity toward
 * whichever racer (local player or opponent) is nearer to it in z, plus a
 * per-frame random jitter so the motion doesn't read as mechanical homing.
 * No per-alien RNG state is kept: the jitter is redrawn each frame from
 * w->frame and the slot index via the shared LCG, which is enough variety
 * for visual "aliveness" without needing to survive/replay across frames.
 * opp->live gates the opponent branch off entirely (no bot, no paired peer)
 * so a lone racer's aliens just home on them. */
static void update_alien_drift(AlienField *a, uint16_t frame, int16_t my_x,
                               const Opponent *opp)
{
    int i;
    for (i = 0; i < ALIEN_COUNT; i++) {
        int16_t target_x, pull, jitter, vx;
        uint16_t r;
        if (!a->alive[i]) continue;

        target_x = my_x;
        if (opp->live) {
            /* opp->rel_z reaches ±LAP_LENGTH (rel_depth's clamp), so
             * a->z[i] - opp->rel_z can exceed int16 range before the
             * comparison narrows it back down — same overflow alien_crash_index
             * guards against with its own int32 dz. */
            int16_t d_me   = a->z[i] < 0 ? S16(-a->z[i]) : a->z[i];
            int32_t d_opp32 = (int32_t)a->z[i] - opp->rel_z;
            if (d_opp32 < 0) d_opp32 = -d_opp32;
            if (d_opp32 < d_me) target_x = opp->x;
        }

        pull = S16(target_x - a->x[i]);
        if (pull >  ALIEN_DRIFT_ACCEL) pull =  ALIEN_DRIFT_ACCEL;
        if (pull < -ALIEN_DRIFT_ACCEL) pull = -ALIEN_DRIFT_ACCEL;

        r = LCG_STEP(U16W(frame * 6151u + (uint16_t)i * 9973u));
        /* Scale the LCG's high byte into [0, 2*JITTER]: one mulu.w, not a
         * ~140-cycle divu.w for `r % 25` per live alien per frame.  The
         * high byte is also the better-mixed half of an LCG. */
        jitter = S16((int16_t)(((r >> 8) * (2u * ALIEN_DRIFT_JITTER + 1)) >> 8)
                     - ALIEN_DRIFT_JITTER);

        vx = S16(a->vx[i] + pull + jitter);
        if (vx >  ALIEN_DRIFT_SPEED_MAX) vx =  ALIEN_DRIFT_SPEED_MAX;
        if (vx < -ALIEN_DRIFT_SPEED_MAX) vx = -ALIEN_DRIFT_SPEED_MAX;
        a->vx[i] = vx;

        a->x[i] = S16(a->x[i] + vx);
        if (a->x[i] >  ALIEN_DRIFT_X_LIMIT) a->x[i] =  ALIEN_DRIFT_X_LIMIT;
        if (a->x[i] < -ALIEN_DRIFT_X_LIMIT) a->x[i] = -ALIEN_DRIFT_X_LIMIT;
    }
}

/* Scroll all live aliens toward the camera each frame, freeing any that have
 * passed us so their slots recycle for the rest of the lap. */
static void update_aliens(World *w, const Opponent *opp)
{
    AlienField *a = &w->aliens;
    update_alien_drift(a, w->frame, w->ps.cam_x, opp);
    field_scroll(a->z, a->alive, ALIEN_COUNT, w->cam_zspeed, ALIEN_DESPAWN_Z);
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
    if ((i = free_slot(m->alive, MISSILE_COUNT)) < 0) return false;
    m->x[i]     = w->ps.cam_x;
    m->z[i]     = HLINE_ZMIN;
    m->vis_z[i] = HLINE_ZMIN;
    m->alive[i] = true;
    backend_snd_sfx(SND_FIRE);
    w->ps.fire_cooldown = FIRE_COOLDOWN_FRAMES;
    return true;
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
    if ((i = free_slot(m->alive, MINE_COUNT)) < 0) return false;
    m->x[i]     = w->ps.cam_x;
    m->z[i]     = 0;
    m->alive[i] = true;
    w->mines_left--;
    w->mine_cooldown = MINE_DROP_COOLDOWN;
    return true;
}

/* Smallest lateral hit window, world units: an alien's drawn half-width up
 * close (ALIEN_SCALE_W/FOCAL = 48).  Aliens and the ghost both use it, so a
 * target on screen is as hittable as it looks. */
#define ALIEN_AIM_TOL  ((int16_t)(ALIEN_SCALE_W / FOCAL))

/* missile_hits — the one missile-vs-target test.  The missile at (mx, mz) has
 * just advanced, and the target at depth tz > 0 closed in, by `step` in all
 * (missile_speed + cam_zspeed), so it hit if tz lies in [mz - step, mz] and
 * the lateral miss is inside the target's visual half-width:
 * max(min_tol, ALIEN_MIN_PIX * tz / FOCAL), the far-away pixel floor. */
static inline bool missile_hits(int16_t mx, int16_t mz, int16_t tx, int16_t tz,
                                int16_t step, int16_t min_tol)
{
    int16_t dx, tol;
    if (mz < tz || mz > tz + step) return false;
    dx = S16(mx - tx);
    if (dx < 0) dx = S16(-dx);
    tol = S16((tz * ALIEN_MIN_PIX) >> 7);    /* tz > 0, FOCAL = 2^7: exact */
    if (tol < min_tol) tol = min_tol;
    return dx < tol;
}

/* Advance all live missiles and test collision against all live aliens.
 * Takes the sets separately (not World) — race_update runs the peer's
 * missiles through the same sim against the same alien field. */
static void update_missiles(int16_t cam_zspeed, MissileSet *m, AlienField *a,
                            uint16_t *kills)
{
    int mi;
    int16_t missile_speed = S16(cam_zspeed * MISSILE_SPEED_FACTOR);
    int16_t step          = S16(missile_speed + cam_zspeed);
    for (mi = 0; mi < MISSILE_COUNT; mi++) {
        int ai;
        if (likely(!m->alive[mi])) continue;
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
            if (unlikely(!a->alive[ai])) continue;
            if (a->z[ai] <= 0) continue;             /* already passed player */
            if (unlikely(missile_hits(m->x[mi], m->z[mi], a->x[ai], a->z[ai],
                                      step, ALIEN_AIM_TOL))) {
                a->alive[ai] = false;
                m->alive[mi] = false;
                backend_snd_sfx(SND_ENMYHIT);
                if (kills) (*kills)++;
                break;   /* spent: one missile, one kill — its z window
                          * (4*cam_zspeed) can span more than one alien_gap */
            }
        }
    }
}

/* missiles_hit_ghost — test all live missiles in a set against one ghost
 * target (the remote player, or ourselves for bot missiles) at camera-
 * relative depth rel_z, with missile_hits().  Kills the hitting missile and
 * returns true.
 *
 * min_tol — lateral floor of the hit window in world units:
 *   ALIEN_AIM_TOL (= alien parity) for our missiles vs the ghost,
 *   PLAYER_HIT_TOL (= alien_hit_player parity) for peer missiles crossing
 *   us at rel_z = 1 (their missiles fly forward, so they reach us only from
 *   behind).
 *
 * Plain static, like mines_hit_ghost and bot_update: forcing noinline on the
 * three measured +40 B and +1.2k cycles/frame at -Os. */
static bool missiles_hit_ghost(int16_t cam_zspeed,
    MissileSet *m, int16_t ghost_x, int16_t rel_z, int16_t min_tol)
{
    int mi;
    int16_t step = S16(cam_zspeed * (MISSILE_SPEED_FACTOR + 1));
    if (rel_z <= 0 || rel_z > GRID_ZFAR) return false;
    for (mi = 0; mi < MISSILE_COUNT; mi++) {
        if (!m->alive[mi]) continue;
        if (unlikely(missile_hits(m->x[mi], m->z[mi], ghost_x, rel_z,
                                  step, min_tol))) {
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
 * Sampling dx one frame late costs at most CRUISE_VEL_X_MAX (80) units of
 * lateral error against PLAYER_HIT_TOL (128). */
static bool mines_hit_ghost(MineField *m,
    int16_t ghost_x, int16_t rel_z)
{
    int i; bool hit = false;
    if (rel_z > 0) return false;   /* our mines only threaten someone behind us;
                                    * also bounds d to int16 — both terms are then
                                    * in [-LAP_LENGTH, 0] */
    for (i = 0; i < MINE_COUNT; i++) {
        int16_t d, dx;
        if (!m->alive[i]) continue;
        d = S16(m->z[i] - rel_z);
        if (d > 0) continue;                   /* still ahead of them */
        m->alive[i] = false;                   /* reached: consume regardless */
        dx = S16(m->x[i] - ghost_x);
        if (dx < 0) dx = S16(-dx);
        if (unlikely(dx < PLAYER_HIT_TOL)) hit = true;
    }
    return hit;
}

/* ── State update functions ───────────────────────────────────────────────── */

static GameState state_cruise(World *w, bool *fired, bool *dropped, uint8_t keys,
                              const Opponent *opp)
{
    bool peer_finished = opp->finished;
    /* Throttle: Up/Down are free in cruise (no vertical control here).
     * Racing trade-off — faster reaches the finish line sooner but leaves
     * less time to dodge aliens and line up shots.
     * Excess over the ceiling is bled back every frame, not just while
     * holding Up: drafting/catch-up push cam_zspeed past it with nothing
     * else pulling it down, and unbounded growth would outrun
     * ALIEN_DESPAWN_Z and the z_phase wrap.
     * Below the ceiling, cam_zspeed also bleeds toward CAM_ZSPEED_BASE
     * (not CAM_ZSPEED_MIN) when Up isn't held, so throttle is an active
     * control rather than cruise control: releasing Up settles back to
     * base speed, never all the way down to the brake floor.  Below base
     * (post-brake or post-crash) there is no automatic recovery — climbing
     * back is the player's Up key, or auto-acceleration would just
     * reappear through the back door. */
    {
        int16_t zmax = zspeed_max_for_lap(w->lap);
        /* Three mutually exclusive cases.  The over-ceiling case must come
         * first and NOT be undone by the KEY_UP branch: re-clamping to zmax
         * whenever Up is held would deny a throttling player (the normal way
         * to race) any catch-up.  The excess only bleeds by THROTTLE_STEP,
         * which the boosts out-gain, so the ship can ride above the lap
         * ceiling while chasing. */
        if (w->cam_zspeed > zmax)
            w->cam_zspeed = S16(w->cam_zspeed - THROTTLE_STEP);
        else if (keys & KEY_UP) {
            w->cam_zspeed = S16(w->cam_zspeed + THROTTLE_STEP);
            if (w->cam_zspeed > zmax) w->cam_zspeed = zmax;
        } else if (w->cam_zspeed > CAM_ZSPEED_BASE) {
            w->cam_zspeed = S16(w->cam_zspeed - THROTTLE_DECAY);
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
    if (unlikely(w->finish_dist <= 0 || peer_finished)) {
        bool crossed = w->finish_dist <= 0;
        if (crossed) {                       /* per-lap timing, our own crossings only */
            uint16_t now = backend_ticks();
            uint16_t lap_ticks = U16W(now - w->lap_start_tick);
            if (w->best_lap_ticks == 0 || lap_ticks < w->best_lap_ticks)
                w->best_lap_ticks = lap_ticks;
            w->lap_start_tick = now;
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
            assert(w->next_alien_pos >= (uint16_t)LAP_LENGTH + ALIEN_Z_MARGIN);
            w->lap++;
            w->finish_dist    = S16(w->finish_dist + LAP_LENGTH);
            w->next_alien_pos = U16W(w->next_alien_pos - LAP_LENGTH);
            w->alien_seq      = U16W(w->alien_seq - w->aliens_per_lap);
        } else {
            w->race_ticks    = U16W(backend_ticks() - w->race_start_tick);
            w->race_finished = true;
            w->race_result    = peer_finished ? RACE_LOST : RACE_WON;
            w->gate_timer    = GATE_MIN_FRAMES;
            w->round++;
            return STATE_GATE;
        }
    }
    update_alien_spawns(w, world_progress(w), opp);
    update_aliens(w, opp);
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
        w->cam_zspeed = zspeed_after_stun(w->cam_zspeed);
        return STATE_CRUISE;              /* same lap, progress kept */
    }
    return STATE_CRASH;
}

/* race_start — reset per-race state; called on every GATE→CRUISE launch.
 * Does NOT touch race_result (the gate shows the last verdict until the
 * next crossing overwrites it).  Entities are cleared HERE ONLY: a mid-race
 * lap crossing (see the crossing branch in state_cruise) must not clear
 * them — the spawn window reaches GRID_ZFAR + ALIEN_SPAWN_LEAD = 10240 units
 * ahead, so the next lap's aliens are already on screen when you cross. */
static void race_start(World *w) {
    int i;
    w->lap              = 1;
    w->finish_dist       = LAP_LENGTH;
    w->race_finished     = false;
    w->gate_ready        = false;
    w->race_parity      ^= 1;
    w->alien_kills       = 0;                 /* per RACE, not per lap */
    w->race_start_tick   = backend_ticks();   /* feeds race_ticks at the gate */
    w->lap_start_tick    = w->race_start_tick;
    w->cam_zspeed        = CAM_ZSPEED_BASE;
    w->alien_gap         = alien_gap(w->round);
    w->aliens_per_lap    = U16W(LAP_LENGTH / w->alien_gap);  /* 1 divide/race */
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

/* state_gate — between-laps victory/defeat screen: spins the logo, arms FIRE
 * once the dwell has elapsed (the PRESS FIRE prompt only appears then), and
 * hands off to the 3-2-1-GO countdown as soon as both the local player and
 * the bot/peer are ready.  gate_ready is what race_update
 * broadcasts as READY, so it must never latch during the dwell. */
static GameState state_gate(World *w, uint8_t keys, bool peer_gate_ok)
{
    w->angleY = S16W(w->angleY + LOGO_SPIN_Y);   /* spin the logo; wraps, masked by fastSin */
    w->angleX = S16W(w->angleX + LOGO_SPIN_X);
    if (w->gate_timer > 0)     w->gate_timer--;
    else if (keys & KEY_FIRE)  w->gate_ready = true;
    if (w->gate_ready && peer_gate_ok) {
        race_start(w);
        w->cam_zspeed      = 0;               /* frozen at the line until GO */
        w->countdown_timer = COUNTDOWN_FRAMES;
        return STATE_COUNTDOWN;
    }
    return STATE_GATE;
}

/* state_countdown — track visible, camera frozen (cam_zspeed=0, set by
 * state_gate), 3/2/1/GO overlaid until launch.  apply_speed_modifiers is a
 * no-op outside STATE_CRUISE, so the freeze holds untouched for the whole
 * phase; the "Grid always scrolls" line in main() adds 0 every frame. */
static GameState state_countdown(World *w) {
    if (--w->countdown_timer <= 0) {
        w->cam_zspeed = CAM_ZSPEED_BASE;      /* unfreeze: race starts now */
        return STATE_CRUISE;
    }
    return STATE_COUNTDOWN;
}
