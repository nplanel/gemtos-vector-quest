/* physics.c — game state machine, entity logic, and physics for vquest.
 * Unity-included from vquest.c (not a separate TU).
 * Requires: game constants, PhysicsState, GameState, RenderFlags from vquest.h,
 *           rendering functions from render.c (included before this file).   */

/* Per-frame game logic: raised back to O3 under the global -Os Atari build
 * (see OPT_ATARI in the Makefile).  Together with backend_gemtos.c's O3
 * regions this recovers most of -Ofast's frame time at a fraction of its
 * size; render.c deliberately stays -Os (its C code is divide-bound — O3
 * there measured +4.4 kB for <1k cycles/frame). */
#pragma GCC push_options
#pragma GCC optimize("O3")

static const RenderFlags kStateFlags[] = {
/*                        grid   logo   arrows takeof  land   aliens credits remote */
/* STATE_TAKEOFF */     { true,  false, true,  true,   false, false, false,  true  },
/* STATE_CRUISE  */     { true,  false, true,  false,  true,  true,  false,  true  },
/* STATE_LANDING */     { true,  false, true,  false,  true,  true,  false,  true  },
/* STATE_CRASH   */     { true,  false, false, false,  false, false, false,  false },
/* STATE_SUCCESS */     { false, true,  false, false,  false, false, true,   false },
};

/* lateral_crash_landing(cam_x, 0) is the no-strip case; GCC folds the constant
 * subtraction so the takeoff caller pays nothing for the unified form. */
static inline bool lateral_crash_landing(int16_t cam_x, int16_t strip_x) {
    int16_t rel = (int16_t)(cam_x - strip_x);
    return rel > CRASH_CAM_X || rel < -CRASH_CAM_X;
}

/* LCG_STEP — one step of the shared LCG (multiplier 2053, addend 13849).
 * Used by next_strip_x() and spawn_aliens() for deterministic pseudo-random positions. */
#define LCG_STEP(seed) ((uint16_t)((uint16_t)(seed) * 2053u + 13849u))

/* aliens_for_round — number of aliens to spawn this round (capped at ALIEN_COUNT). */
static inline int aliens_for_round(int16_t round) {
    int n = ALIEN_COUNT_BASE + (round - 1);
    return n < ALIEN_COUNT ? n : ALIEN_COUNT;
}

/* zspeed_for_round — nominal cruise speed for a round.  The cruise throttle
 * moves cam_zspeed away from this, so round transitions recompute from the
 * round number instead of accumulating on the throttled value. */
static inline int16_t zspeed_for_round(int16_t round) {
    int16_t z = (int16_t)(CAM_ZSPEED_BASE + (round - 1) * CAM_ZSPEED_STEP);
    return z > CAM_ZSPEED_MAX ? CAM_ZSPEED_MAX : z;
}

/* next_strip_x — strip position varying by round and frame (not deterministic
 * per round alone, so each playthrough differs).
 * Picks magnitude from [STRIP_X_MIN, STRIP_X_MAX] and sign from high bit. */
static int16_t next_strip_x(int16_t round, uint16_t frame) {
    uint16_t r   = LCG_STEP(LCG_STEP(round) + frame);
    int16_t  mag = (r & 0x2000) ? STRIP_X_MAX : STRIP_X_MIN;
    return   (r & 0x8000) ? mag : (int16_t)(-mag);
}

/* apply_lateral — update vel_x and cam_x from Left/Right keys.
 * steer/vel_x_max are compile-time constants at each call site;
 * GCC constant-folds all branches when inlined. */
static inline void apply_lateral(int16_t steer, int16_t vel_x_max,
                                  uint8_t keys, int16_t *vel_x, int16_t *cam_x)
{
    if (keys & KEY_LEFT)  *vel_x = (int16_t)(*vel_x - steer);
    if (keys & KEY_RIGHT) *vel_x = (int16_t)(*vel_x + steer);
    *vel_x = (int16_t)(*vel_x - (*vel_x >> DRAG_SHIFT));
    if (*vel_x >  vel_x_max) *vel_x =  vel_x_max;
    if (*vel_x < -vel_x_max) *vel_x = -vel_x_max;
    *cam_x = (int16_t)(*cam_x + *vel_x);
}

/* apply_vertical — update vel_y and cam_y from Up/Down keys.
 * thrust/descent_thrust/vel_y_max/clamp_ground are compile-time constants;
 * GCC folds all dependent branches away when inlined.
 * descent_thrust=0 disables KEY_DOWN (used for takeoff). */
static inline void apply_vertical(int16_t thrust, int16_t descent_thrust,
                                   int16_t vel_y_max, bool clamp_ground, uint8_t keys,
                                   int16_t *vel_y, int16_t *cam_y)
{
    if (keys & KEY_UP) {
        *vel_y = (int16_t)(*vel_y + thrust - GRAVITY);
    } else if (descent_thrust && (keys & KEY_DOWN)) {
        *vel_y = (int16_t)(*vel_y - GRAVITY - descent_thrust);
    } else {
        *vel_y = (int16_t)(*vel_y - GRAVITY);
    }
    *vel_y = (int16_t)(*vel_y - (*vel_y >> DRAG_SHIFT));
    if (*vel_y > vel_y_max) *vel_y = vel_y_max;
    if (*vel_y < VEL_Y_MIN) *vel_y = VEL_Y_MIN;
    *cam_y = (int16_t)(*cam_y + *vel_y);
    if (clamp_ground && *cam_y < CAM_Y_INIT) { *cam_y = CAM_Y_INIT; *vel_y = 0; }
}


/* ── Alien / missile helpers ─────────────────────────────────────────────── */

/* Spawn this round's aliens spread evenly along the whole approach corridor,
 * then clear all missile slots.  Called once when transitioning
 * TAKEOFF → CRUISE.  One divide per round (not per frame). */
static void spawn_aliens(World *w)
{
    AlienField *a = &w->aliens;
    int i;
    int n_aliens = aliens_for_round(w->round);
    int16_t gap = (int16_t)((LANDING_APPROACH_DIST - ALIEN_Z_MARGIN) / (n_aliens + 1));
    for (i = 0; i < n_aliens; i++) {
        uint16_t r;
        int16_t  mag;
        a->z[i]    = (int16_t)(LANDING_APPROACH_DIST - (i + 1) * gap);
        r          = LCG_STEP(w->round * 37 + i * 13);
        /* magnitude in [FP_ONE, 3*FP_ONE) — span 2*FP_ONE is a power of two, so
         * a mask (one AND) replaces an expensive %; bits 1-11 feed it while
         * bit 15 stays reserved for the sign so the two are independent. */
        mag        = (int16_t)(FP_ONE + ((r >> 1) & (2 * FP_ONE - 1)));
        a->x[i]    = (r & 0x8000) ? mag : (int16_t)(-mag);
        a->alive[i] = true;
    }
    for (i = n_aliens; i < ALIEN_COUNT; i++) a->alive[i] = false;
    for (i = 0; i < MISSILE_COUNT; i++) w->missiles.alive[i] = false;
}

/* True if a live alien crossed z=0 this frame and is within FP_ONE laterally.
 * The z window (0, -cam_zspeed] covers exactly the crossing frame so this
 * does not re-trigger on subsequent frames after the alien has passed. */
static bool alien_hit_player(const World *w)
{
    const AlienField *a = &w->aliens;
    int i;
    for (i = 0; i < ALIEN_COUNT; i++) {
        int16_t rel;
        if (!a->alive[i]) continue;
        if (a->z[i] > 0 || a->z[i] <= -w->cam_zspeed) continue;
        rel = (int16_t)(w->ps.cam_x - a->x[i]);
        if (rel < 0) rel = (int16_t)(-rel);
        if (rel < FP_ONE / 4) return true;
    }
    return false;
}

/* Scroll all live aliens toward the camera each frame. */
static void update_aliens(int16_t cam_zspeed, AlienField *a)
{
    int i;
    for (i = 0; i < ALIEN_COUNT; i++) {
        if (a->alive[i])
            a->z[i] = (int16_t)(a->z[i] - cam_zspeed);
    }
}

/* Fire: spawn one missile in the first free slot, rate-limited to one per
 * FIRE_COOLDOWN_FRAMES.  Returns true when a missile was actually spawned
 * (the FIRE event broadcast to the peer). */
#define FIRE_COOLDOWN_FRAMES 5
static bool try_fire_missile(World *w, uint8_t keys)
{
    MissileSet *m = &w->missiles;
    int i;
    if (w->ps.fire_cooldown > 0) { w->ps.fire_cooldown--; return false; }
    if (!(keys & KEY_FIRE)) return false;
    for (i = 0; i < MISSILE_COUNT; i++) {
        if (!m->alive[i]) {
            m->x[i]     = w->ps.cam_x;
            m->z[i]     = HLINE_ZMIN;
            m->alive[i] = true;
            backend_snd_sfx(SND_FIRE);
            w->ps.fire_cooldown = FIRE_COOLDOWN_FRAMES;
            return true;
        }
    }
    return false;
}

/* Advance all live missiles and test collision against all live aliens.
 * Takes the sets separately (not World) — race_update runs the peer's
 * missiles through the same sim against the same alien field. */
static void update_missiles(int16_t cam_zspeed, MissileSet *m, AlienField *a)
{
    int mi;
    int16_t missile_speed = (int16_t)(cam_zspeed * MISSILE_SPEED_FACTOR);
    for (mi = 0; mi < MISSILE_COUNT; mi++) {
        int ai;
        if (!m->alive[mi]) continue;
        m->z[mi] = (int16_t)(m->z[mi] + missile_speed);
        if (m->z[mi] > GRID_ZFAR) { m->alive[mi] = false; continue; }
        for (ai = 0; ai < ALIEN_COUNT; ai++) {
            int16_t rel, aim_tol;
            if (!a->alive[ai]) continue;
            if (a->z[ai] <= 0) continue;             /* already passed player */
            if (m->z[mi] < a->z[ai]) continue;
            if (m->z[mi] > a->z[ai] + missile_speed + cam_zspeed) continue;
            rel = (int16_t)(m->x[mi] - a->x[ai]);
            /* Tolerance matches alien visual half-width: max(ALIEN_SCALE_W, ALIEN_MIN_PIX*z)/FOCAL.
             * gcc folds the constants into asr+add sequences, no muls/divs emitted. */
            aim_tol = ALIEN_SCALE_W / FOCAL;                         /* 48, constant      */
            { int16_t t = a->z[ai] * ALIEN_MIN_PIX / FOCAL;         /* 3*z/128           */
              if (t > aim_tol) aim_tol = t; }
            if (rel > -aim_tol && rel < aim_tol) {
                a->alive[ai] = false;
                m->alive[mi] = false;
                backend_snd_sfx(SND_ENMYHIT);
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
    int16_t missile_speed = (int16_t)(cam_zspeed * MISSILE_SPEED_FACTOR);
    if (rel_z <= 0 || rel_z > GRID_ZFAR) return false;
    for (mi = 0; mi < MISSILE_COUNT; mi++) {
        int16_t rel, aim_tol;
        if (!m->alive[mi]) continue;
        if (m->z[mi] < rel_z) continue;
        if (m->z[mi] > rel_z + missile_speed + cam_zspeed) continue;
        rel = (int16_t)(m->x[mi] - ghost_x);
        if (rel < 0) rel = (int16_t)(-rel);
        aim_tol = min_tol;
        { int16_t t = (int16_t)(rel_z * ALIEN_MIN_PIX / FOCAL);
          if (t > aim_tol) aim_tol = t; }
        if (rel < aim_tol) {
            m->alive[mi] = false;
            return true;
        }
    }
    return false;
}

/* ── State update functions ───────────────────────────────────────────────── */

static GameState state_takeoff(World *w, uint8_t keys)
{
    PhysicsState *ps = &w->ps;
    if (--w->takeoff_timer <= 0 && ps->cam_y < CRUISE_ALT) {
        return STATE_CRASH;
    }
    apply_vertical(TAKEOFF_THRUST, 0, VEL_Y_MAX, true, keys, &ps->vel_y, &ps->cam_y);
    apply_lateral(STEER, VEL_X_MAX, keys, &ps->vel_x, &ps->cam_x);
    if (lateral_crash_landing(ps->cam_x, 0)) {
        return STATE_CRASH;
    }
    if (ps->cam_y >= CRUISE_ALT) {
        ps->cam_y = CRUISE_ALT; ps->vel_y = 0;
        /* cam_x/vel_x intentionally NOT reset — carry into landing */
        w->strip_dist = LANDING_APPROACH_DIST;
        spawn_aliens(w);
        return STATE_CRUISE;
    }
    return STATE_TAKEOFF;
}

static GameState state_cruise(World *w, bool *fired, uint8_t keys)
{
    /* Throttle: Up/Down are free in cruise (no vertical control here).
     * Racing trade-off — faster reaches the strip sooner but leaves less
     * time to dodge aliens and line up; speed carries into the landing. */
    if (keys & KEY_UP) {
        w->cam_zspeed = (int16_t)(w->cam_zspeed + THROTTLE_STEP);
        if (w->cam_zspeed > CAM_ZSPEED_MAX) w->cam_zspeed = CAM_ZSPEED_MAX;
    }
    if (keys & KEY_DOWN) {
        w->cam_zspeed = (int16_t)(w->cam_zspeed - THROTTLE_STEP);
        if (w->cam_zspeed < CAM_ZSPEED_MIN) w->cam_zspeed = CAM_ZSPEED_MIN;
    }
    apply_lateral(CRUISE_STEER, CRUISE_VEL_X_MAX, keys, &w->ps.vel_x, &w->ps.cam_x);
    w->strip_dist = (int16_t)(w->strip_dist - w->cam_zspeed);
    if (w->strip_dist <= LANDING_STRIP_MIN) {
        w->strip_dist = LANDING_STRIP_MIN;
        return STATE_LANDING;
    }
    update_aliens(w->cam_zspeed, &w->aliens);
    *fired = try_fire_missile(w, keys);
    return STATE_CRUISE;
}

static GameState state_landing(World *w, uint8_t keys)
{
    PhysicsState *ps = &w->ps;
    update_aliens(w->cam_zspeed, &w->aliens);   /* keep aliens scrolling with the world */
    apply_vertical(TAKEOFF_THRUST, DESCENT_THRUST, VEL_Y_MAX, false, keys, &ps->vel_y, &ps->cam_y);
    apply_lateral(STEER, VEL_X_MAX, keys, &ps->vel_x, &ps->cam_x);
    if (lateral_crash_landing(ps->cam_x, w->strip_x)) {
        return STATE_CRASH;
    }
    if (ps->cam_y <= 0) {
        int16_t abs_vel_y = (int16_t)(ps->vel_y < 0 ? -ps->vel_y : ps->vel_y);
        int16_t rel_x     = (int16_t)(ps->cam_x - w->strip_x);
        int16_t abs_rel_x = (int16_t)(rel_x < 0 ? -rel_x : rel_x);
        if (abs_vel_y < CRASH_VEL_Y && abs_rel_x < LAND_CAM_X) {
            w->round++;
            w->cam_zspeed = zspeed_for_round(w->round);
            ps->cam_y    = CAM_Y_INIT;
            ps->vel_y    = 0; ps->vel_x = 0;
            w->strip_x  = next_strip_x(w->round, w->frame);
            hud_draw(w->round);
            return STATE_SUCCESS;
        }
        return STATE_CRASH;
    }
    /* Park strip_dist once the strip is well behind: hovering here for long
     * enough (~9 s at base speed, ~4 s at max) would otherwise wrap it
     * through int16 and make the strip reappear ahead as a phantom. */
    if (w->strip_dist > -(int16_t)(4 * FP_ONE))
        w->strip_dist = (int16_t)(w->strip_dist - w->cam_zspeed);
    return STATE_LANDING;
}

static GameState state_crash(World *w, bool *flash)
{
    *flash = true;
    if (--w->crash_timer <= 0) {
        w->round         = 1;
        w->takeoff_timer = TAKEOFF_FRAMES_BASE;
        w->cam_zspeed    = zspeed_for_round(1);
        w->strip_x = next_strip_x(1, w->frame);
        w->ps.cam_y = CAM_Y_INIT; w->ps.vel_y = 0; w->ps.cam_x = CAM_X_INIT; w->ps.vel_x = 0;
        hud_draw(1);
        return STATE_TAKEOFF;
    }
    return STATE_CRASH;
}

static GameState state_success(World *w, uint8_t keys)
{
    w->angleY = (int16_t)(w->angleY + w->angleYinc);
    w->angleX = (int16_t)(w->angleX + w->angleXinc);
    if (keys) {
        w->ps.cam_y = CAM_Y_INIT; w->ps.cam_x = CAM_X_INIT; w->ps.vel_y = 0; w->ps.vel_x = 0;
        w->takeoff_timer = TAKEOFF_FRAMES_BASE;
        return STATE_TAKEOFF;
    }
    return STATE_SUCCESS;
}

/* ── Computer opponent ────────────────────────────────────────────────────── *
 * Drives the remote-player slot when no serial peer is heard, producing the
 * same RemoteState that serial_recv() fills — everything downstream (ghost,
 * peer missiles, kills) is source-agnostic.  Cost per frame: a handful of
 * int16 adds/compares; one 16-bit mul (LCG) every 32nd frame; the 8-alien
 * scan only on frames its fire cooldown has expired.                        */

#define BOT_TAKEOFF_CLIMB   36         /* alt/frame: ~57 frames to CRUISE_ALT  */
#define BOT_DESCENT         40         /* alt/frame during landing             */
#define BOT_STEER           24         /* max lateral step/frame toward lane   */
#define BOT_DEAD_FRAMES    150         /* ~3 s out of the race after being shot */
#define BOT_FIRE_COOLDOWN   20         /* slower trigger than the player's 5   */
#define BOT_AIM_TOL  ((int16_t)(FP_ONE / 4))  /* lateral window to take a shot */

typedef struct {
    uint8_t  state;      /* RS_*                              */
    int16_t  timer;      /* RS_DEAD countdown                 */
    int16_t  cam_x, target_x;
    uint16_t progress;
    int16_t  alt;
    int16_t  zspeed;
    int16_t  round;
    int8_t   cooldown;
    uint16_t lcg;
} Bot;

static void bot_start_leg(Bot *b) {
    b->state    = RS_TAKEOFF;
    b->progress = 0;
    b->alt      = CAM_Y_INIT;
    b->zspeed   = zspeed_for_round(b->round);
}

static void bot_init(Bot *b) {
    b->round    = 1;
    b->cam_x    = CAM_X_INIT;
    b->target_x = CAM_X_INIT;
    b->cooldown = BOT_FIRE_COOLDOWN;
    b->lcg      = 0x5EED;
    bot_start_leg(b);
}

/* bot_kill — our missile hit the bot: same penalty as the player (back to
 * round 1), benched for the game-over jingle's worth of frames. */
static void bot_kill(Bot *b) {
    b->state = RS_DEAD;
    b->timer = BOT_DEAD_FRAMES;
    b->round = 1;
    b->progress = 0;
}

/* bot_update — advance the bot one frame and emit its RemoteState.
 * aliens->z[] is in the *local* camera frame; my_progress - bot progress
 * converts it to the bot's frame for aiming (aliens are world-fixed).
 * noinline: once per frame; keeping it out of main saves ~1KB of text. */
static __attribute__((noinline)) void bot_update(Bot *b, RemoteState *out,
    const AlienField *aliens,
    uint16_t my_progress, int16_t my_cam_x, uint16_t frame)
{
    bool fire = false;
    switch (b->state) {
    case RS_DEAD:
        if (--b->timer <= 0) bot_start_leg(b);
        break;
    case RS_TAKEOFF:
        b->alt = (int16_t)(b->alt + BOT_TAKEOFF_CLIMB);
        if (b->alt >= CRUISE_ALT) { b->alt = CRUISE_ALT; b->state = RS_CRUISE; }
        break;
    case RS_CRUISE: {
        int16_t d;
        /* Re-roll throttle and lane every 32 frames (one LCG mul). */
        if ((frame & 31) == 0) {
            b->lcg = LCG_STEP(b->lcg);
            b->zspeed = (int16_t)(zspeed_for_round(b->round) - 16 + (b->lcg & 63));
            if (b->zspeed < CAM_ZSPEED_MIN) b->zspeed = CAM_ZSPEED_MIN;
            if (b->zspeed > CAM_ZSPEED_MAX) b->zspeed = CAM_ZSPEED_MAX;
            b->target_x = (int16_t)(((b->lcg >> 4) & 0x0FFF) - 2 * FP_ONE);
        }
        b->progress = (uint16_t)(b->progress + b->zspeed);
        if (b->progress >= LANDING_APPROACH_DIST - LANDING_STRIP_MIN) {
            b->state = RS_LANDING;
            break;
        }
        d = (int16_t)(b->target_x - b->cam_x);
        if (d >  BOT_STEER) d =  BOT_STEER;
        if (d < -BOT_STEER) d = -BOT_STEER;
        b->cam_x = (int16_t)(b->cam_x + d);
        /* Fire at the player if they are ahead and roughly in our lane,
         * else at an alien near our lane. */
        if (b->cooldown > 0) { b->cooldown--; break; }
        {
            int16_t me_rel = (int16_t)(my_progress - b->progress);
            int16_t dx     = (int16_t)(my_cam_x - b->cam_x);
            if (dx < 0) dx = (int16_t)(-dx);
            if (me_rel > REMOTE_Z_NEAR && me_rel < GRID_ZFAR && dx < BOT_AIM_TOL) {
                fire = true;
            } else {
                int i;
                for (i = 0; i < ALIEN_COUNT; i++) {
                    int32_t az;                            /* bot-relative z */
                    int16_t ax;
                    if (!aliens->alive[i]) continue;
                    az = (int32_t)aliens->z[i] + me_rel;   /* 32-bit: no wrap */
                    if (az < ALIEN_ZMIN || az > GRID_ZFAR) continue;
                    ax = (int16_t)(aliens->x[i] - b->cam_x);
                    if (ax < 0) ax = (int16_t)(-ax);
                    if (ax < BOT_AIM_TOL) { fire = true; break; }
                }
            }
        }
        if (fire) b->cooldown = BOT_FIRE_COOLDOWN;
        break; }
    case RS_LANDING:
        b->progress = (uint16_t)(b->progress + b->zspeed);
        /* same cap as the player's my_progress: keeps rel_z in int16_t */
        b->progress = progress_clamp(b->progress);
        b->alt = (int16_t)(b->alt - BOT_DESCENT);
        if (b->alt <= 0) {           /* landed: straight into the next leg */
            b->round++;
            bot_start_leg(b);
        }
        break;
    }
    out->state    = b->state;
    out->fire     = fire;
    out->kill     = false;   /* bot hits on us are detected locally */
    out->cam_x    = b->cam_x;
    out->progress = b->progress;
    out->alt      = b->alt;
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
} RaceState;

static void race_init(RaceState *rs, bool bot_enabled) {
    memset(rs, 0, sizeof(*rs));
    rs->remote_idle = REMOTE_TIMEOUT_FRAMES;
    rs->bot_enabled = bot_enabled;
    bot_init(&rs->bot);
}

/* noinline is load-bearing for size: gcc's jump threading duplicates the
 * region of main()'s loop that this call sits in (specialising paths through
 * the state-machine compares — the call sequence appears twice in the
 * disassembly), so inlined code here is paid for twice.  One out-of-line
 * body + a call per frame is a net −1.6 kB of text; a plain static would be
 * re-inlined at -Ofast, so the attribute is required. */
static __attribute__((noinline))
void race_update(RaceState *rs, GameState *state, bool remote_player_flag,
    World *w, bool fired)
{
    const PhysicsState *ps = &w->ps;
    int16_t cam_zspeed     = w->cam_zspeed;

    /* Per-leg race coordinate shared with the peer: how far down the approach
     * we are.  0 during takeoff so both players start each leg level; capped
     * at the course length.  strip_dist runs negative during descent and is
     * unbounded below while hovering, so 30720 - strip_dist can exceed
     * INT16_MAX: the subtraction is done in uint16 (defined wraparound, same
     * sub.w) instead of signed 16-bit int, which would be UB under -mshort. */
    uint16_t my_progress = (*state == STATE_CRUISE || *state == STATE_LANDING)
        ? progress_clamp((uint16_t)((uint16_t)LANDING_APPROACH_DIST - (uint16_t)w->strip_dist)) : 0;

    /* Peer missiles run through the same sim against the same deterministic
     * alien field, so both machines agree on alien kills. */
    update_missiles(cam_zspeed, &rs->rmissiles, &w->aliens);

    /* Remote slot: a serial peer when one is talking, the bot otherwise. */
    RemoteState rs_in;
    bool got = serial_recv(&rs_in);
    if (got)                                          rs->remote_idle = 0;
    else if (rs->remote_idle < REMOTE_TIMEOUT_FRAMES) rs->remote_idle++;
    bool bot_active = rs->bot_enabled && rs->remote_idle >= REMOTE_TIMEOUT_FRAMES;
    if (!got && bot_active) {
        bot_update(&rs->bot, &rs_in, &w->aliens,
                   my_progress, ps->cam_x, w->frame);
        got = true;
    }
    if (got) {
        rs->remote = rs_in;
        if (rs->remote.fire && rs->remote.state != RS_DEAD) {
            int16_t muzzle_z = (int16_t)((int16_t)rs->remote.progress
                                         - (int16_t)my_progress + HLINE_ZMIN);
            int i;
            for (i = 0; i < MISSILE_COUNT; i++)
                if (!rs->rmissiles.alive[i]) {
                    rs->rmissiles.x[i]     = rs->remote.cam_x;
                    rs->rmissiles.z[i]     = muzzle_z;
                    rs->rmissiles.alive[i] = true;
                    break;
                }
        }
        /* Their missile hit us (shooter-authoritative, edge-triggered). */
        if (rs->remote.kill && !rs->kill_latched &&
            (*state == STATE_CRUISE || *state == STATE_LANDING))
            *state = STATE_CRASH;
        rs->kill_latched = rs->remote.kill;
    }

    /* Bot shots at us are resolved locally (no wire to carry a KILL):
     * its missiles fly forward in our frame and cross us at z≈0. */
    if (bot_active && (*state == STATE_CRUISE || *state == STATE_LANDING) &&
        missiles_hit_ghost(cam_zspeed, &rs->rmissiles,
                           ps->cam_x, 1, (int16_t)(FP_ONE / 4)))
        *state = STATE_CRASH;

    /* Our missiles vs the ghost: shooter detects the hit and tells the
     * peer via the KILL bit (the bot is killed directly).  The local
     * RS_DEAD override hides the ghost until its own state catches up. */
    if ((rs->remote_idle < REMOTE_TIMEOUT_FRAMES || bot_active) &&
        rs->remote.state != RS_DEAD &&
        missiles_hit_ghost(cam_zspeed, &w->missiles,
                           rs->remote.cam_x,
                           (int16_t)((int16_t)rs->remote.progress - (int16_t)my_progress),
                           (int16_t)(ALIEN_SCALE_W / FOCAL))) {
        backend_snd_sfx(SND_ENMYHIT);
        if (bot_active) bot_kill(&rs->bot);
        else            rs->kill_pending = KILL_REPEAT;
        rs->remote.state = RS_DEAD;
    }

    /* Beacon until a peer is heard: sending costs 7 BIOS traps on Atari,
     * so single-player only pays them every 16th frame.  Both sides
     * beacon, so pairing completes within ~0.3s; once paired, send every
     * frame for smooth ghosting (and fall back to beaconing if the peer
     * goes quiet past the ghost timeout). */
    if (rs->remote_idle < REMOTE_TIMEOUT_FRAMES || (w->frame & 15) == 0) {
        RemoteState out;
        out.state    = *state == STATE_CRUISE  ? RS_CRUISE
                     : *state == STATE_LANDING ? RS_LANDING
                     : *state == STATE_CRASH   ? RS_DEAD
                     : RS_TAKEOFF;
        out.fire     = fired;
        out.kill     = rs->kill_pending > 0;
        out.cam_x    = ps->cam_x;
        out.progress = my_progress;
        out.alt      = ps->cam_y;
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
    int16_t rel_z = (int16_t)((int16_t)rs->remote.progress - (int16_t)my_progress);
    bool peer_on  = (rs->remote_idle < REMOTE_TIMEOUT_FRAMES || rs->bot_enabled) &&
                    rs->remote.state != RS_DEAD;
    rs->ghost_show = peer_on && rel_z > 0 && rel_z <= GRID_ZFAR &&
                     remote_player_flag;
    rs->ghost_z    = CLAMP(rel_z, REMOTE_Z_NEAR, GRID_ZFAR);
}

#pragma GCC pop_options
