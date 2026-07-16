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
/*                     grid   gate   finish aliens credits remote */
/* STATE_CRUISE */   { true,  false, true,  true,  false,  true  },
/* STATE_CRASH  */   { true,  false, false, false, false,  false },
/* STATE_GATE   */   { false, true,  false, false, true,   false },
};

/* LCG_STEP — one step of the shared LCG (multiplier 2053, addend 13849).
 * Used by update_alien_spawns() for deterministic pseudo-random positions. */
#define LCG_STEP(seed) ((uint16_t)((uint16_t)(seed) * 2053u + 13849u))

/* zspeed_for_round — nominal cruise speed for a round.  The cruise throttle
 * moves cam_zspeed away from this, so round transitions recompute from the
 * round number instead of accumulating on the throttled value. */
static inline int16_t zspeed_for_round(int16_t round) {
    int16_t z = (int16_t)(CAM_ZSPEED_BASE + (round - 1) * CAM_ZSPEED_STEP);
    return z > CAM_ZSPEED_MAX ? CAM_ZSPEED_MAX : z;
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

/* ── Alien / missile helpers ─────────────────────────────────────────────── */

/* alien_gap — spawn spacing for this round's density (decision 8): shrinks
 * every round to a floor, so later rounds pack more aliens into the same
 * GRID_ZFAR+ALIEN_SPAWN_LEAD window. */
static inline int16_t alien_gap(int16_t round) {
    int16_t gap = (int16_t)(ALIEN_GAP_BASE - (round - 1) * ALIEN_GAP_STEP);
    return gap < ALIEN_GAP_MIN ? ALIEN_GAP_MIN : gap;
}

/* lap_start — reset per-lap state; called on every GATE→CRUISE launch.
 * Does NOT touch lap_result (the gate shows the last verdict until the
 * next crossing overwrites it). */
static void lap_start(World *w) {
    int i;
    w->finish_dist    = LANDING_APPROACH_DIST;
    w->lap            = 1;
    w->race_finished  = false;
    w->gate_ready     = false;
    w->race_parity   ^= 1;
    w->alien_kills    = 0;
    w->lap_start_frame = w->frame;
    w->alien_gap      = alien_gap(w->round);
    w->aliens_per_lap = (uint16_t)(LANDING_APPROACH_DIST / w->alien_gap);  /* 1 divide/race */
    w->next_alien_pos = (uint16_t)(ALIEN_Z_MARGIN + w->alien_gap);
    w->alien_seq      = 0;
    for (i = 0; i < ALIEN_COUNT; i++)   w->aliens.alive[i]   = false;
    for (i = 0; i < MISSILE_COUNT; i++) w->missiles.alive[i] = false;
}

/* update_alien_spawns — materialize scheduled aliens entering the window.
 * World position is f(round, k) only, so race peers build identical fields
 * regardless of their own progress. */
static void update_alien_spawns(World *w, uint16_t my_progress) {
    AlienField *a = &w->aliens;
    int16_t gap = w->alien_gap;
    for (;;) {
        int16_t rel = (int16_t)(w->next_alien_pos - my_progress); /* uint16 sub */
        if (rel > (int16_t)(GRID_ZFAR + ALIEN_SPAWN_LEAD)) break;
        if (rel > ALIEN_ZMIN) {          /* skip ones already behind us */
            int slot = -1, i;
            for (i = 0; i < ALIEN_COUNT; i++)
                if (!a->alive[i]) { slot = i; break; }
            if (slot < 0) break;   /* no free slot: don't advance the schedule */
            {
                uint16_t r   = LCG_STEP(w->round * 37 + w->alien_seq * 13);
                /* magnitude in [FP_ONE, 3*FP_ONE) — span 2*FP_ONE is a power of
                 * two, so a mask (one AND) replaces an expensive %; bits 1-11
                 * feed it while bit 15 stays reserved for the sign so the two
                 * are independent. */
                int16_t mag  = (int16_t)(FP_ONE + ((r >> 1) & (2 * FP_ONE - 1)));
                a->x[slot]     = (r & 0x8000) ? mag : (int16_t)(-mag);
                a->z[slot]     = rel;
                a->alive[slot] = true;
            }
        }
        w->alien_seq++;
        w->next_alien_pos = (uint16_t)(w->next_alien_pos + gap);
    }
}

/* True if a live alien crossed z=0 this frame and is within FP_ONE laterally.
 * The z window (0, -cam_zspeed] covers exactly the crossing frame so this
 * does not re-trigger on subsequent frames after the alien has passed.
 * Deactivates the hitting alien (mirrors update_missiles' alien kill):
 * aliens don't advance while frozen in STATE_CRASH, so a still-alive alien
 * would otherwise sit in this exact window and re-trigger the instant the
 * stun timer expires and the state briefly reads STATE_CRUISE again,
 * permanently locking the game in STATE_CRASH. */
static bool alien_hit_player(World *w)
{
    AlienField *a = &w->aliens;
    int i;
    for (i = 0; i < ALIEN_COUNT; i++) {
        int16_t rel;
        if (!a->alive[i]) continue;
        if (a->z[i] > 0 || a->z[i] <= -w->cam_zspeed) continue;
        rel = (int16_t)(w->ps.cam_x - a->x[i]);
        if (rel < 0) rel = (int16_t)(-rel);
        if (rel < FP_ONE / 4) { a->alive[i] = false; return true; }
    }
    return false;
}

/* Scroll all live aliens toward the camera each frame, freeing any that have
 * passed us so their slots recycle for the rest of the lap. */
static void update_aliens(int16_t cam_zspeed, AlienField *a)
{
    int i;
    for (i = 0; i < ALIEN_COUNT; i++) {
        if (!a->alive[i]) continue;
        a->z[i] = (int16_t)(a->z[i] - cam_zspeed);
        if (a->z[i] < ALIEN_DESPAWN_Z) { a->alive[i] = false; continue; }
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
static void update_missiles(int16_t cam_zspeed, MissileSet *m, AlienField *a,
                            uint16_t *kills)
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
                if (kills) (*kills)++;
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

static GameState state_cruise(World *w, bool *fired, uint8_t keys, bool peer_finished)
{
    /* Throttle: Up/Down are free in cruise (no vertical control here).
     * Racing trade-off — faster reaches the finish line sooner but leaves
     * less time to dodge aliens and line up shots. */
    if (keys & KEY_UP) {
        w->cam_zspeed = (int16_t)(w->cam_zspeed + THROTTLE_STEP);
        if (w->cam_zspeed > CAM_ZSPEED_MAX) w->cam_zspeed = CAM_ZSPEED_MAX;
    }
    if (keys & KEY_DOWN) {
        w->cam_zspeed = (int16_t)(w->cam_zspeed - THROTTLE_STEP);
        if (w->cam_zspeed < CAM_ZSPEED_MIN) w->cam_zspeed = CAM_ZSPEED_MIN;
    }
    apply_lateral(CRUISE_STEER, CRUISE_VEL_X_MAX, keys, &w->ps.vel_x, &w->ps.cam_x);
    w->finish_dist = (int16_t)(w->finish_dist - w->cam_zspeed);
    /* Round ends the instant either side crosses: racing out a lap you've
     * already lost (or making the winner wait for the loser) just feels
     * like standing around, so peer_finished ends it here too instead of
     * only once our own finish_dist reaches zero. */
    if (w->finish_dist <= 0 || peer_finished) {
        w->lap_frames = (uint16_t)(w->frame - w->lap_start_frame);
        if (w->finish_dist <= 0 &&
            (w->best_lap_frames == 0 || w->lap_frames < w->best_lap_frames))
            w->best_lap_frames = w->lap_frames;   /* only a completed lap counts */
        w->race_finished = true;
        w->lap_result   = peer_finished ? LAP_LOST : LAP_WON;
        w->gate_timer   = GATE_MIN_FRAMES;
        w->round++;
        w->cam_zspeed   = zspeed_for_round(w->round);
        hud_draw(w->round);
        return STATE_GATE;
    }
    update_alien_spawns(w, (uint16_t)((uint16_t)LANDING_APPROACH_DIST
                                      - (uint16_t)w->finish_dist));
    update_aliens(w->cam_zspeed, &w->aliens);
    *fired = try_fire_missile(w, keys);
    return STATE_CRUISE;
}

/* state_crash — the stun: crashing (alien contact or an enemy missile) costs
 * time and speed, not the lap.  finish_dist/aliens are frozen because only
 * state_cruise advances them — that IS the time penalty; on recovery
 * cam_zspeed drops to the floor as the speed penalty.  No round/lap touches. */
static GameState state_crash(World *w, bool *flash)
{
    *flash = true;
    if (--w->crash_timer <= 0) {
        w->cam_zspeed = CAM_ZSPEED_MIN;   /* speed penalty on resume */
        return STATE_CRUISE;              /* same lap, progress kept */
    }
    return STATE_CRASH;
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
    w->angleY = (int16_t)(w->angleY + w->angleYinc);   /* spin the logo */
    w->angleX = (int16_t)(w->angleX + w->angleXinc);
    if (w->gate_timer > 0) w->gate_timer--;
    if (keys & KEY_FIRE)  w->gate_ready = true;
    if (w->gate_timer <= 0 && w->gate_ready && peer_gate_ok) {
        lap_start(w);
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

/* Bot personality — revealed on the first race launch (see bot_update's
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
    /* Personality is revealed on the first RS_READY->RS_CRUISE launch (see
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
    if (force_finish && b->state != RS_WAIT && b->state != RS_READY) {
        b->round++;
        b->finished = true;
        b->state    = RS_WAIT;
        b->timer    = b->gate_wait;
    }
    switch (b->state) {
    case RS_DEAD:
        if (--b->timer <= 0) b->state = RS_CRUISE;   /* resume the lap where it was */
        break;
    case RS_WAIT:
        if (--b->timer <= 0) b->state = RS_READY;
        break;
    case RS_READY:
        if (player_going) {
            if (b->round == 1) {
                /* First launch: fold in the frame count reached (which
                 * varies with the player's own reaction time at the gate,
                 * unlike the fixed boot-time seed) so personality actually
                 * differs race to race instead of always landing on the
                 * same b->lcg % 3 result. */
                b->lcg = LCG_STEP((uint16_t)(b->lcg ^ frame));
                b->personality = (uint8_t)(b->lcg % 3);
                switch (b->personality) {
                case BOT_AGGRESSIVE:   b->gate_wait = 35; break;
                case BOT_DEFENSIVE:    b->gate_wait = 65; break;
                default:               b->gate_wait = BOT_WAIT_FRAMES; break;
                }
            }
            b->state       = RS_CRUISE;
            b->progress    = 0;
            b->finished    = false;
            b->race_parity ^= 1;
            b->lap         = 1;
            b->zspeed      = zspeed_for_round(b->round);
        }
        break;
    case RS_CRUISE: {
        int16_t d;
        {
            /* Re-roll period: aggressive/unpredictable change lane faster. */
            uint16_t reroll_mask = (b->personality == BOT_DEFENSIVE) ? 63u : 31u;
            if ((frame & reroll_mask) == 0) {
                int16_t nom = zspeed_for_round(b->round);
                b->lcg = LCG_STEP(b->lcg);
                switch (b->personality) {
                case BOT_AGGRESSIVE:
                    /* Races for the win: a player holding Up reaches
                     * CAM_ZSPEED_MAX in ~64 frames via THROTTLE_STEP, so a
                     * nom-relative bump (still close to the passive
                     * baseline) could never keep up — push for near-max
                     * speed instead, same ballpark as a throttling human. */
                    b->zspeed = (int16_t)(CAM_ZSPEED_MAX - 24 + (b->lcg & 31));
                    break;
                case BOT_DEFENSIVE:
                    b->zspeed = (int16_t)(nom - 32 + (b->lcg & 31));
                    break;
                default: /* BOT_UNPREDICTABLE: true min..max spread, not a
                          * narrow band around nominal. */
                    b->zspeed = (int16_t)(CAM_ZSPEED_MIN +
                        (b->lcg % (CAM_ZSPEED_MAX - CAM_ZSPEED_MIN + 1)));
                    break;
                }
                if (b->zspeed < CAM_ZSPEED_MIN) b->zspeed = CAM_ZSPEED_MIN;
                if (b->zspeed > CAM_ZSPEED_MAX) b->zspeed = CAM_ZSPEED_MAX;
                b->target_x = (int16_t)(((b->lcg >> 4) & 0x0FFF) - 2 * FP_ONE);
            }
        }
        b->progress = (uint16_t)(b->progress + b->zspeed);
        if (b->progress >= LANDING_APPROACH_DIST) {
            b->round++;
            b->finished = true;
            b->state    = RS_WAIT;
            b->timer    = b->gate_wait;
            break;
        }
        d = (int16_t)(b->target_x - b->cam_x);
        if (d >  BOT_STEER) d =  BOT_STEER;
        if (d < -BOT_STEER) d = -BOT_STEER;
        b->cam_x = (int16_t)(b->cam_x + d);
        /* Fire priority and cooldown vary by personality:
         *   Aggressive:    player first, short cooldown
         *   Defensive:     aliens first, long cooldown
         *   Unpredictable: random priority, moderate cooldown */
        if (b->cooldown > 0) { b->cooldown--; break; }
        {
            int16_t me_rel = rel_depth(my_lap, my_progress, b->lap, b->progress);
            int16_t dx     = (int16_t)(my_cam_x - b->cam_x);
            if (dx < 0) dx = (int16_t)(-dx);
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
                    ax = (int16_t)(aliens->x[i] - b->cam_x);
                    if (ax < 0) ax = (int16_t)(-ax);
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
                        ax = (int16_t)(aliens->x[i] - b->cam_x);
                        if (ax < 0) ax = (int16_t)(-ax);
                        if (ax < BOT_AIM_TOL) { fire = true; break; }
                    }
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
        break; }
    }
    out->state       = b->state;
    out->fire        = fire;
    out->kill        = false;   /* bot hits on us are detected locally */
    out->mine        = false;   /* Commit 6 wires this up */
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
} RaceState;

static void race_init(RaceState *rs, bool bot_enabled) {
    memset(rs, 0, sizeof(*rs));
    rs->remote_idle = REMOTE_TIMEOUT_FRAMES;
    rs->bot_enabled = bot_enabled;
    bot_init(&rs->bot);
}

/* race_lap_reset — clear the peer FINISHED latch; called by main on every
 * GATE→CRUISE transition (the same frame lap_start() runs). */
static void race_lap_reset(RaceState *rs) { rs->peer_finished = false; }

/* noinline is load-bearing for size: gcc's jump threading duplicates the
 * region of main()'s loop that this call sits in (specialising paths through
 * the state-machine compares — the call sequence appears twice in the
 * disassembly), so inlined code here is paid for twice.  One out-of-line
 * body + a call per frame is a net −1.6 kB of text; a plain static would be
 * re-inlined at -Ofast, so the attribute is required. */
static __attribute__((noinline))
void race_update(RaceState *rs, GameState *state, bool remote_player_flag,
    World *w, bool fired, bool player_won)
{
    const PhysicsState *ps = &w->ps;
    int16_t cam_zspeed     = w->cam_zspeed;

    /* Per-lap race coordinate shared with the peer: how far down the course
     * we are.  0 while not racing so both players start each lap level;
     * capped at the course length.  finish_dist can run negative for a
     * frame right after crossing (before lap_start resets it), so
     * 30720 - finish_dist can exceed INT16_MAX: the subtraction is done in
     * uint16 (defined wraparound, same sub.w) instead of signed 16-bit int,
     * which would be UB under -mshort.
     * Emergent behaviour: combined with a `lap` field that keeps its
     * race-end value while not racing, a peer sitting at the gate reports
     * (lap=LAPS_PER_RACE, progress=0) — rel_depth() then saturates against
     * whichever lap we're on, so the ghost hides and drafting is off, which
     * is what we want, but it is accidental rather than checked here. */
    uint16_t my_progress = (*state == STATE_CRUISE)
        ? progress_clamp((uint16_t)((uint16_t)LANDING_APPROACH_DIST - (uint16_t)w->finish_dist)) : 0;

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
    if (got)                                          rs->remote_idle = 0;
    else if (rs->remote_idle < REMOTE_TIMEOUT_FRAMES) rs->remote_idle++;
    bool bot_active = rs->bot_enabled && rs->remote_idle >= REMOTE_TIMEOUT_FRAMES;
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
            int16_t muzzle_z = (int16_t)(rs->peer_rel_z + HLINE_ZMIN);
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

    /* Our missiles vs the ghost: shooter detects the hit and tells the
     * peer via the KILL bit (the bot is killed directly).  The local
     * RS_DEAD override hides the ghost until its own state catches up. */
    if ((rs->remote_idle < REMOTE_TIMEOUT_FRAMES || bot_active) &&
        rs->remote.state != RS_DEAD &&
        missiles_hit_ghost(cam_zspeed, &w->missiles,
                           rs->remote.cam_x, rs->peer_rel_z,
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
        out.state       = my_rs;
        out.fire        = fired;
        out.kill        = rs->kill_pending > 0;
        out.finished    = w->race_finished;
        out.mine        = false;         /* Commit 6 wires this up */
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

#pragma GCC pop_options
