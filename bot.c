/* bot.c — the computer opponent.
 * Unity-included from vquest.c after physics.c (not a separate TU).
 * Requires: the speed and alien helpers (zspeed_*, catchup_boost,
 *           alien_crash_index), abs16/clamp16 and LCG_STEP from physics.c;
 *           RemoteState and rel_depth from vquest.h.  race.c, included
 *           next, drives it. */

/* ── Computer opponent ────────────────────────────────────────────────────── *
 * Drives the remote-player slot in 1-player mode (F1 at the gate), producing the
 * same RemoteState that serial_recv() fills — everything downstream (ghost,
 * peer missiles, kills) is source-agnostic.  Cost per frame: a handful of
 * int16 adds/compares; one 16-bit mul (LCG) every 32nd frame; the alien
 * scan only on frames its fire cooldown has expired.                        */

/* Single-personality opponent: aggressive, tuned to be hard to beat.  It
 * runs at (or fractionally under) the lap ceiling every frame, reacts at the
 * gate well before the player can launch, shoots the player on priority with
 * a short cooldown, and drops mines when leading.  The only reliable way past
 * it is to stun it with a missile — deliberately, so races stay fun. */
#define BOT_WAIT_FRAMES     35         /* frames at the gate before READY           */
#define BOT_STEER           24         /* max lateral step/frame toward lane   */
#define BOT_FIRE_COOLDOWN   10         /* short trigger — stuns the player often     */
#define BOT_AIM_TOL  ((int16_t)(FP_ONE / 4))  /* lateral window to take a shot */
/* Alien crashes: the bot flies a wider "sloppy pilot" hitbox than the player's
 * exact PLAYER_HIT_TOL (FP_ONE/8) so it lines up with aliens often enough to
 * matter, and clips BOT_CRASH_ODDS_MASK-gated ~1 in 4 of those line-ups — its
 * one unforced error, giving a trailing player openings without keeping the
 * bot easy.  Measured ~0.4 slips per race with a full-throttle autopilot
 * (1/2 odds gave ~0.8, 1/8 gave ~0.14); raise the mask to make it rarer. */
#define BOT_CRASH_TOL       ((int16_t)(FP_ONE / 2))
#define BOT_CRASH_ODDS_MASK 3          /* (lcg & MASK)==0 clips: 3 => ~1/4       */

typedef struct {
    uint8_t  state;      /* RS_*                              */
    int16_t  timer;      /* RS_DEAD / RS_WAIT / RS_READY countdown */
    int16_t  cam_x, target_x;
    uint16_t progress;
    int16_t  zspeed;
    int8_t   cooldown;
    uint16_t lcg;
    bool     finished;
    uint8_t  race_parity; /* 1-bit, flips at every race launch               */
    uint8_t  lap;         /* lap in race, 1..LAPS_PER_RACE                  */
    uint8_t  mines_left;  /* drops remaining this race                      */
    int8_t   mine_cooldown;
} Bot;

/* bot_race_start — the per-race reset, shared by boot and every launch out
 * of RS_READY (race_start's counterpart for the player). */
static void bot_race_start(Bot *b) {
    b->progress      = 0;
    b->finished      = false;
    b->lap           = 1;
    b->zspeed        = CAM_ZSPEED_BASE;
    b->mines_left    = MINES_PER_RACE;
    b->mine_cooldown = 0;
}

static void bot_init(Bot *b) {
    memset(b, 0, sizeof(*b));        /* race_parity 0, timer 0 */
    bot_race_start(b);
    b->cam_x       = CAM_X_INIT;
    b->target_x    = CAM_X_INIT;
    b->cooldown    = BOT_FIRE_COOLDOWN;
    b->lcg         = 0x5EED;
    /* Ready from the start (timer 0 = RS_READY's "not yet armed"), so a
     * fresh 1-player gate launches the instant FIRE is pressed; the
     * BOT_WAIT_FRAMES pause only paces the bot's post-race gate, which the
     * player's own GATE_MIN_FRAMES dwell always outlasts. */
    b->state       = RS_READY;
}

/* bot_kill — the bot is stunned (our missile or mine, or its own alien
 * crash): same stun as the player, progress kept; it resumes RS_CRUISE
 * mid-lap on expiry. */
static void bot_kill(Bot *b) {
    b->state = RS_DEAD;
    b->timer = STUN_FRAMES;
}

/* bot_finish — the bot's race is over (it crossed the final line, or the
 * player beat it there): FINISHED goes out and it waits out its gate. */
static void bot_finish(Bot *b) {
    b->finished = true;
    b->state    = RS_WAIT;
    b->timer    = BOT_WAIT_FRAMES;
}

/* bot_update — advance the bot one frame and emit its RemoteState.
 * aliens->z[] is in the *local* camera frame; my_progress - bot progress
 * converts it to the bot's frame for aiming (aliens are world-fixed).
 * player_going: local player is RS_READY or RS_CRUISE — gates the bot's own
 * launch out of RS_READY, mirroring the human gate handshake.
 * force_finish: the player just won the race — a real peer would apply the
 * mirror-image peer_finished check on their own machine and end their round
 * immediately too, so the bot (which runs no such independent simulation)
 * needs this external push to lose immediately instead of racing on to its
 * own finish line. */
static void bot_update(Bot *b, RemoteState *out,
    AlienField *aliens, uint16_t my_progress, uint8_t my_lap, int16_t my_cam_x,
    uint16_t frame, bool player_going, bool force_finish)
{
    bool fire = false;
    bool mine = false;
    if (force_finish && b->state != RS_WAIT && b->state != RS_READY)
        bot_finish(b);
    switch (b->state) {
    case RS_DEAD:
        if (--b->timer <= 0) {
            b->state  = RS_CRUISE;   /* resume the lap where it was */
            b->zspeed = zspeed_after_stun(b->zspeed);
        }
        break;
    case RS_WAIT:
        if (--b->timer <= 0) b->state = RS_READY;
        break;
    case RS_READY:
        if (player_going) {
            /* Mirror state_gate/state_countdown's COUNTDOWN_FRAMES so the
             * bot's progress starts advancing the same frame our own
             * cam_zspeed unfreezes, not the instant we enter STATE_COUNTDOWN
             * — otherwise the bot gets a free ~COUNTDOWN_FRAMES head start.
             * b->timer is unused during RS_READY otherwise, so 0 is a safe
             * "not yet armed" sentinel. */
            if (b->timer <= 0) b->timer = COUNTDOWN_FRAMES;
            if (--b->timer > 0) break;
            /* Fold the frame count reached (which varies with the player's own
             * reaction time at the gate, unlike the fixed boot-time seed) into
             * the LCG so lane picks and speed jitter differ race to race. */
            b->lcg = LCG_STEP(U16W(b->lcg ^ frame));
            b->state       = RS_CRUISE;
            b->race_parity ^= 1;
            bot_race_start(b);
        }
        break;
    case RS_CRUISE: {
        int16_t d;
        if ((frame & 31u) == 0) {
            int16_t zmax = zspeed_max_for_lap(b->lap);
            int16_t gap  = rel_depth(my_lap, my_progress, b->lap, b->progress);
            /* Tapered like the player's catch-up boost
             * (apply_speed_modifiers), so the bot settles into shooting range
             * instead of blowing past at the full cap. */
            if (gap >= CATCHUP_REL_Z)
                zmax = S16(zmax + catchup_boost(gap));
            b->lcg = LCG_STEP(b->lcg);
            /* Sit at or a hair under the ceiling every re-roll (zmax-16..zmax+14,
             * clamped) so the player can never simply out-throttle it. */
            b->zspeed = S16(zmax - 16 + (b->lcg & 31));
            if (b->zspeed < CAM_ZSPEED_MIN) b->zspeed = CAM_ZSPEED_MIN;
            if (b->zspeed > zmax) b->zspeed = zmax;
            b->target_x = S16(((b->lcg >> 4) & 0x0FFF) - 2 * FP_ONE);
        }
        b->progress = U16W(b->progress + b->zspeed);
        if (b->progress >= LAP_LENGTH) {
            if (b->lap < LAPS_PER_RACE) {
                /* Mid-race lap: preserve the overshoot (-=, not =0), same
                 * reason as state_cruise's crossing branch. */
                b->lap++;
                b->progress = U16W(b->progress - LAP_LENGTH);
            } else {
                bot_finish(b);
                break;
            }
        }
        d = clamp16(S16(b->target_x - b->cam_x), BOT_STEER);
        b->cam_x = S16(b->cam_x + d);
        {
            int16_t me_rel = rel_depth(my_lap, my_progress, b->lap, b->progress);
            int16_t dx     = abs16(S16(my_cam_x - b->cam_x));
            /* Occasional alien crash: the bot flies the same hazard field as
             * the player and usually threads through it, but clips ~1 in 4 of the
             * aliens it lines up with (BOT_CRASH_TOL/ODDS), costing it the
             * player's own stun and clearing the (shared) alien.  This is its
             * one unforced error, so a trailing player gets openings without
             * landing a shot.  The bot's plane sits at depth -me_rel in our
             * frame; z_half = zspeed/2 catches the single crossing frame, since
             * that depth moves by exactly b->zspeed each frame. */
            {
                int hi = alien_crash_index(aliens, S16(-me_rel), b->cam_x,
                                           S16(b->zspeed >> 1), BOT_CRASH_TOL);
                if (hi >= 0) {
                    b->lcg = LCG_STEP(b->lcg);
                    if ((b->lcg & BOT_CRASH_ODDS_MASK) == 0) {
                        aliens->alive[hi] = false;
                        bot_kill(b);
                    }
                }
            }
            if (b->state != RS_CRUISE) break;   /* crashed this frame: no fire/mine */
            /* Player first, fall back to aliens, on a short cooldown.
             * me_rel feeds mine-dropping below too, so the missile cooldown
             * gate only skips the targeting/fire logic, not this whole
             * block. */
            if (b->cooldown > 0) {
                b->cooldown--;
            } else {
                fire = (me_rel > REMOTE_Z_NEAR && me_rel < GRID_ZFAR
                        && dx < BOT_AIM_TOL);
                if (!fire) {
                    int i;
                    for (i = 0; i < ALIEN_COUNT; i++) {
                        int32_t az; int16_t ax;
                        if (unlikely(!aliens->alive[i])) continue;
                        az = (int32_t)aliens->z[i] + me_rel;
                        if (az < ALIEN_ZMIN || az > GRID_ZFAR) continue;
                        ax = abs16(S16(aliens->x[i] - b->cam_x));
                        if (ax < BOT_AIM_TOL) { fire = true; break; }
                    }
                }
                if (fire) b->cooldown = BOT_FIRE_COOLDOWN;
            }
            /* Mine: dropped blind when we're leading (me_rel < 0 — we can
             * neither see nor shoot a chaser behind us) and they're within
             * guessing range, on its own cooldown while ammo remains.  Same
             * blind proximity guess a human relies on the opponent-distance
             * HUD for.  Also what gives the headless tests a mine to
             * collide with, since the ascii autopilot never presses
             * KEY_DOWN. */
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
