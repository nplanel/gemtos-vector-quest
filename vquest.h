#ifndef VQUEST_H
#define VQUEST_H

#include <stdint.h>
#include <stdbool.h>

/* Fixed-point format, trig table size and model scale — shared with the
 * host-side generator (gen_tables.c) that bakes the sine quarter-table and
 * the pre-scaled model into gen_tables.h in exactly this format. */
#define FP_SHIFT 10
#define FP_ONE (1 << FP_SHIFT)
#define LUT_SIZE 2048
/* Per-lap race course length; also the cap for the wire `progress` field so a
 * decoded value never exceeds int16 range (see serial.h). */
#define LANDING_APPROACH_DIST  (30 * FP_ONE)

/* Cap a race progress value at the course length, so (int16_t)progress stays
 * non-negative and relative-depth subtractions can't flip sign.  The single
 * source of truth for the cap — used by the wire decode (serial.h), the bot,
 * and race_update; all three must agree or peers disagree on ghost depth. */
static inline uint16_t progress_clamp(uint16_t p) {
    return p > LANDING_APPROACH_DIST ? (uint16_t)LANDING_APPROACH_DIST : p;
}

#define LAPS_PER_RACE 1                    /* -> 3 in Commit 4 */
_Static_assert(LAPS_PER_RACE <= 4, "lap field is 2 bits on the wire");

/* rel_depth — camera-relative depth of a racer at (their_lap, their_progress)
 * seen from (my_lap, my_progress).  progress is per-lap, so a plain subtraction
 * is garbage exactly at a lap boundary; the lap delta restores it.
 *
 * Clamped to +/-LANDING_APPROACH_DIST, NOT +/-GRID_ZFAR: the opponent-distance
 * HUD reads the full range, and an incoming mine materializes at this depth
 * from an arbitrarily far leader.  +/-30720 fits int16 with 2047 to spare.
 *
 * Compiles to one muls.w #30720 (~70 cycles, verified): the lap delta is
 * provably int16 and the multiplier is a compile-time constant.  Do NOT
 * "optimize" this into a narrow multiply plus a shift — it is not faster, and
 * see the mul_fp rule in "Atari performance constraints" before touching it. */
static inline int16_t rel_depth(uint8_t their_lap, uint16_t their_progress,
                                 uint8_t my_lap,    uint16_t my_progress)
{
    int32_t r = (int32_t)((int16_t)their_lap - (int16_t)my_lap) * LANDING_APPROACH_DIST
              + (int32_t)(int16_t)(their_progress - my_progress);   /* uint16 sub */
    if (r >  (int32_t)LANDING_APPROACH_DIST) return  (int16_t)LANDING_APPROACH_DIST;
    if (r < -(int32_t)LANDING_APPROACH_DIST) return -(int16_t)LANDING_APPROACH_DIST;
    return (int16_t)r;
}

#define LOGO_SCALE (3.0f/230.0f)   /* model units → world units (gen_tables.c only) */

/* 3-D coordinate types used throughout vquest.c, render.c, and physics.c.
 * Point3DFloat  — raw model data (float, only read by host-side gen_tables.c)
 * Point3DInt    — post-rotation working type (int16_t, fits in one d-register) */
typedef struct {
    float x, y, z;
} Point3DFloat;

typedef struct {
    int16_t x, y, z;
} Point3DInt;

/* Camera and velocity state grouped together; passed as a single pointer to
 * state-machine functions so adding a new physics variable needs only one
 * struct change rather than a signature change in every function. */
typedef struct {
    int16_t cam_y, cam_x, vel_x;
    int8_t  fire_cooldown;
} PhysicsState;

/* Entity array sizes (shared by World below and the tuning constants in
 * render.c). */
#define ALIEN_COUNT      10  /* maximum aliens / array size            */
#define MISSILE_COUNT    3   /* max simultaneous missiles in flight    */

/* Parallel-array entity sets.  MissileSet is its own type (not AlienField
 * with a smaller count) so a missile set can never be passed where an alien
 * field is expected. */
typedef struct {
    int16_t x[ALIEN_COUNT];
    int16_t z[ALIEN_COUNT];
    bool    alive[ALIEN_COUNT];
} AlienField;

typedef struct {
    int16_t x[MISSILE_COUNT];
    int16_t z[MISSILE_COUNT];
    bool    alive[MISSILE_COUNT];
} MissileSet;

/* Everything the game simulates per frame, passed as one pointer (same
 * rationale as PhysicsState above — signatures stay put when state grows).
 * The remote-player slot (RaceState in physics.c) stays outside: it is the
 * other player, not this world, and carries its own wire/bot machinery. */
typedef struct {
    PhysicsState ps;
    AlienField   aliens;
    MissileSet   missiles;
    int16_t      angleY, angleX;        /* logo rotation (spins at the gate) */
    int16_t      angleYinc, angleXinc;  /* set once at init                */
    int16_t      z_phase;               /* grid scroll phase               */
    int16_t      cam_zspeed;
    int16_t      finish_dist;           /* distance to the start/finish line, world units */
    int16_t      crash_timer;
    int16_t      round;          /* RACE number, 1-based (was: lap number)     */
    uint8_t      lap;            /* lap within the race, 1..LAPS_PER_RACE      */
    uint16_t     frame;
    uint16_t     next_alien_pos;  /* course pos of the next alien to materialize */
    uint16_t     alien_seq;       /* per-lap spawn counter (lateral LCG seed)    */
    int16_t      gate_timer;      /* victory-screen dwell before FIRE is armed   */
    int8_t       lap_result;      /* LAP_NONE / LAP_WON / LAP_LOST (gate text)   */
    uint8_t      race_parity;    /* was lap_parity: flips at every RACE launch */
    bool         race_finished;  /* was lap_finished: crossed the FINAL line   */
    bool         gate_ready;      /* fire pressed at the gate                    */
    uint16_t     alien_kills;     /* aliens destroyed this race (gate stats)     */
    uint16_t     lap_start_frame; /* w.frame when this lap launched              */
    uint16_t     lap_frames;      /* duration of the just-finished lap in frames */
    uint16_t     best_lap_frames; /* shortest lap so far, 0 = no best yet        */
} World;

#define LAP_NONE 0
#define LAP_WON  1
#define LAP_LOST 2

typedef enum { STATE_CRUISE, STATE_CRASH, STATE_GATE } GameState;

/* Remote-player wire states (2-bit field in the serial status byte).
 * Values no longer mirror GameState — this is the lap-gate handshake protocol
 * (see race_update/state_gate): RS_WAIT (at the gate, not ready yet),
 * RS_CRUISE (racing), RS_READY (at the gate, ready — FIRE pressed and dwell
 * elapsed), RS_DEAD (stunned after a hit). */
#define RS_WAIT   0
#define RS_CRUISE 1
#define RS_READY  2
#define RS_DEAD   3

/* One update's worth of remote-player data.  Filled by serial_recv() (wire
 * peer) or bot_update() (computer opponent); consumers never see the source.
 * progress is the per-lap race coordinate LANDING_APPROACH_DIST - finish_dist
 * (0 while not racing); combined with lap it feeds rel_depth() for the
 * camera-relative depth (ghost placement, drafting, missile/mine hit tests).
 * finished/race_parity carry the lap-crossing verdict (decisions 3/5):
 * finished is a held level (set once the peer crosses their finish line this
 * lap, cleared at their next lap start), race_parity is a 1-bit counter
 * flipped at every RACE launch so a stale finished packet from a previous
 * race can't be latched onto the new one. */
typedef struct {
    uint8_t  state;        /* RS_*                                          */
    bool     fire;         /* fired a missile this update                   */
    bool     kill;         /* their missile hit us (shooter-authoritative)  */
    bool     finished;     /* crossed their finish line this lap (held)     */
    bool     mine;         /* dropped a mine this update (event, like fire) */
    uint8_t  race_parity;  /* 1-bit, flips at every race launch (was `lap`) */
    uint8_t  lap;          /* lap in race, 1..LAPS_PER_RACE (2 bits on wire)*/
    int16_t  cam_x;
    uint16_t progress;     /* per-lap                                       */
} RemoteState;

/* Per-state render flags — indexed by GameState value.
 * Adding a new visual element: add a field here + one column in the table.
 * `flash` is transient (set per-frame in STATE_CRASH) so it stays in the switch. */
typedef struct {
    bool grid, gate, finish_line, aliens, credits, remote_player;
} RenderFlags;

#endif /* VQUEST_H */
