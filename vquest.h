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
/* Per-leg race course length; also the cap for the wire `progress` field so a
 * decoded value never exceeds int16 range (see serial.h). */
#define LANDING_APPROACH_DIST  (30 * FP_ONE)

/* Cap a race progress value at the course length, so (int16_t)progress stays
 * non-negative and relative-depth subtractions can't flip sign.  The single
 * source of truth for the cap — used by the wire decode (serial.h), the bot,
 * and race_update; all three must agree or peers disagree on ghost depth. */
static inline uint16_t progress_clamp(uint16_t p) {
    return p > LANDING_APPROACH_DIST ? (uint16_t)LANDING_APPROACH_DIST : p;
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
    int16_t cam_y, cam_x, vel_y, vel_x;
    int8_t  fire_cooldown;
} PhysicsState;

typedef enum { STATE_TAKEOFF, STATE_CRUISE, STATE_LANDING, STATE_CRASH, STATE_SUCCESS } GameState;

/* Remote-player wire states (2-bit field in the serial status byte).
 * Values match GameState for the flyable states; STATE_SUCCESS maps to
 * RS_TAKEOFF (the peer is on the ground waiting to start its next leg). */
#define RS_TAKEOFF 0
#define RS_CRUISE  1
#define RS_LANDING 2
#define RS_DEAD    3

/* One update's worth of remote-player data.  Filled by serial_recv() (wire
 * peer) or bot_update() (computer opponent); consumers never see the source.
 * progress is the per-leg race coordinate LANDING_APPROACH_DIST - strip_dist
 * (0 during takeoff/dead), so rel_z = remote.progress - my_progress. */
typedef struct {
    uint8_t  state;     /* RS_*                                          */
    bool     fire;      /* fired a missile this update                   */
    bool     kill;      /* their missile hit us (shooter-authoritative)  */
    int16_t  cam_x;
    uint16_t progress;
    int16_t  alt;       /* cam_y; quantized to 32 units on the wire      */
} RemoteState;

/* Per-state render flags — indexed by GameState value.
 * Adding a new visual element: add a field here + one column in the table.
 * `flash` is transient (set per-frame in STATE_CRASH) so it stays in the switch. */
typedef struct {
    bool grid, logo, arrows, takeoff_strip, landing_strip, aliens, credits, remote_player;
} RenderFlags;

#endif /* VQUEST_H */
