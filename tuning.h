#ifndef TUNING_H
#define TUNING_H

/* tuning.h — gameplay and simulation tuning constants.
 *
 * These all used to live in render.c, which is the 3-D rendering pipeline and
 * uses almost none of them: the split tracked the unity include order (render.c
 * is included before physics.c, so whatever physics needed had to be defined
 * there) rather than what the constants mean.  Anything genuinely tied to the
 * projection stays in render.c — FOCAL, the GRID_* geometry, the screen-space
 * sizes ALIEN_SCALE_W / ALIEN_MIN_PIX / MISSILE_GUN_SEP, and the divide-safety
 * z floors ALIEN_ZMIN / MINE_ZMIN / RMISSILE_ZMIN / HLINE_ZMIN, whose values
 * are derived from it.  Constants used only inside physics.c (the bot, the
 * draft/catch-up caps, the wire timeouts) stay there.
 *
 * Included by vquest.c ahead of render.c.  Depends only on vquest.h. */

#include "vquest.h"   /* FP_ONE, LANDING_APPROACH_DIST */

/* ── Camera speed and throttle ──────────────────────────────────────────── */
#define CAM_X_INIT   ((int16_t)(FP_ONE / 2))   /* centered on the course, ±0.5 units */
#define CAM_ZSPEED_BASE  128  /* Z advance per frame at race start (= FP_ONE/8)  */
#define CAM_ZSPEED_MAX  384   /* absolute ceiling (~3× base); see zspeed_max_for_lap
                               * in physics.c for the per-lap ceiling that actually
                               * gates a race                                    */
#define CAM_ZSPEED_MIN   64   /* throttle floor (cruise speed control)           */
#define THROTTLE_STEP     4   /* cam_zspeed change/frame holding Up/Down in
                               * cruise — addq/subq range like the physics consts */
#define THROTTLE_DECAY    2   /* cam_zspeed bleed/frame toward CAM_ZSPEED_BASE
                               * when Up is not held; must stay below
                               * THROTTLE_STEP or holding Up cannot gain */

/* ── Lateral physics ─────────────────────────────────────────────────────── *
 * Constants chosen so net per-frame deltas fit in addq/subq range (1-8)      *
 * on m68k.                                                                   */
#define DRAG_SHIFT      4   /* drag: vel -= vel>>4  (GCC arithmetic shift ok)   */
#define CRUISE_STEER     16   /* lateral accel while racing                      */
#define CRUISE_VEL_X_MAX 48   /* lateral speed cap while racing                  */

#define CRUISE_ALT    ((int16_t)(2 * FP_ONE))  /* fixed camera altitude          */

/* ── Lap timing ──────────────────────────────────────────────────────────── *
 * STUN_FRAMES: crash penalty (time + speed, decision 1).                     *
 * GATE_MIN_FRAMES: minimum dwell at the victory screen before FIRE arms.     *
 * LAP_JOIN_MAX: see peer_gate_ok in race_update — bounds the RS_CRUISE       *
 * clause to a freshly launched peer, never a mid-lap one.                   */
#define STUN_FRAMES      30
/* Speed penalty on resume: a bounded subtract, not a hard reset to
 * CAM_ZSPEED_MIN — the full reset used to be the larger of the two crash
 * penalties and was invisible to the player, which made hits read as
 * arbitrary.  Applied identically to the player (state_crash) and the bot
 * (its RS_DEAD -> RS_CRUISE edge), or the bot becomes strictly advantaged. */
#define CRASH_ZSPEED_PENALTY 96
#define GATE_MIN_FRAMES  75
#define LAP_JOIN_MAX    ((int16_t)(2 * FP_ONE))
/* Crash tolerance vs the player: wider than the drawn half-width
 * (ALIEN_SCALE_W/FOCAL = 48) to account for the ship's own width, but
 * narrow enough that an alien which has visibly slid off-screen no longer
 * connects.  FP_ONE/4 was ~5x the visual silhouette and produced invisible
 * edge crashes. */
#define ALIEN_CRASH_TOL  ((int16_t)(FP_ONE / 8))
/* Aliens spawn continuously along the whole lap (decision 8): alien k sits at
 * world course position ALIEN_Z_MARGIN + (k+1)*alien_gap(round), a pure
 * function of (round, k) so both race peers materialize the identical field
 * independently of their own progress.  The margin keeps the first alien
 * ahead of the player at lap start. */
#define ALIEN_Z_MARGIN ((int16_t)(2 * FP_ONE))
/* alien_gap(round) density tuning: gap shrinks every round to a floor.
 * BASE ⇒ ~3 concurrent aliens already on round 1; MIN ⇒ ~8 concurrent aliens
 * in the ~10-unit spawn window, fitting comfortably within the 10 alien
 * slots (ALIEN_COUNT). */
/* ALIEN_GAP_BASE = 3*FP_ONE (not the "natural" 7*FP_ONE/2): so that
 * LANDING_APPROACH_DIST divides evenly by every reachable gap in the ramp,
 * the alien layout replays identically lap to lap instead of drifting 2048
 * units every lap (decisions section, race redesign plan Commit 3). */
#define ALIEN_GAP_BASE   ((int16_t)(3 * FP_ONE))
#define ALIEN_GAP_STEP   ((int16_t)(FP_ONE / 2))
#define ALIEN_GAP_MIN    ((int16_t)(5 * FP_ONE / 4))
/* gap:            3072  2560  2048  1536  1280 (floor)
 * aliens per lap:   10    12    15    20    24
 * A future ALIEN_GAP_STEP/MIN change could silently break the exact
 * divisibility the per-lap alien schedule relies on (see race_start's
 * aliens_per_lap and the crossing branch in physics.c) — these assertions
 * are the real product, not the comment. */
_Static_assert(LANDING_APPROACH_DIST % ALIEN_GAP_BASE == 0, "gap must divide the lap");
_Static_assert(LANDING_APPROACH_DIST % (ALIEN_GAP_BASE - ALIEN_GAP_STEP) == 0, "gap must divide the lap");
_Static_assert(LANDING_APPROACH_DIST % (ALIEN_GAP_BASE - 2 * ALIEN_GAP_STEP) == 0, "gap must divide the lap");
_Static_assert(LANDING_APPROACH_DIST % (ALIEN_GAP_BASE - 3 * ALIEN_GAP_STEP) == 0, "gap must divide the lap");
_Static_assert(LANDING_APPROACH_DIST % ALIEN_GAP_MIN == 0, "gap must divide the lap");
/* How far ahead (beyond GRID_ZFAR) an alien materializes before it would be
 * visible — must clear the missile-hit window's overshoot so a materializing
 * alien is never skipped by update_missiles() the frame it appears. */
#define ALIEN_SPAWN_LEAD ((int16_t)(2 * FP_ONE))
/* Aliens are freed once this far behind the camera; must be < -CAM_ZSPEED_MAX
 * so alien_hit_player()'s (0, -cam_zspeed] crossing window is never cut short. */
#define ALIEN_DESPAWN_Z  ((int16_t)(-(FP_ONE / 2)))
#define MISSILE_SPEED_FACTOR  3   /* missile speed = cam_zspeed * this factor   */
/* ── Mines (MINE_COUNT lives in vquest.h with MineField) ─────────────────── *
 * FIRE + KEY_DOWN drops one: braking is the cost, mirroring try_fire_missile.
 * Our mine vs the peer is shooter-authoritative like missiles (we detect the
 * hit and send it via the KILL bit); the peer's/bot's mine is rendered
 * locally so it can be dodged, with the hit itself arriving as their KILL
 * bit (bot mines are resolved locally, same split as bot missiles). */
#define MINES_PER_RACE       3
#define MINE_DROP_COOLDOWN  50
#define MINE_HIT_TOL   ((int16_t)(FP_ONE / 4))    /* alien_hit_player parity */
/* Our mines live one full lap before expiring: long enough for a chaser 20
 * units back to reach one, and -30944 min still fits int16. */
#define MINE_DESPAWN_Z  (-(int16_t)LANDING_APPROACH_DIST)

#endif /* TUNING_H */
