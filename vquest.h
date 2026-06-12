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

/* Per-state render flags — indexed by GameState value.
 * Adding a new visual element: add a field here + one column in the table.
 * `flash` is transient (set per-frame in STATE_CRASH) so it stays in the switch. */
typedef struct {
    bool grid, logo, arrows, takeoff_strip, landing_strip, aliens, credits, remote_player;
} RenderFlags;

#endif /* VQUEST_H */
