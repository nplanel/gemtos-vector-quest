/* render.c — 3-D rendering pipeline for vquest.
 * Unity-included from vquest.c (not a separate TU).
 * Requires: FP_SHIFT/FP_ONE/mul_fp/fastSin/fastCos from vquest.c preamble,
 *           game constants (#defines) defined before this include.        */

/* Model data is baked pre-scaled and bias-packed into kModelVertsPacked /
 * kModelEdges by the host generator (gen_tables.c) — no float, no malloc;
 * model_init() unpacks the vertices into gModelVerts at startup. */
#define NUM_VERTICES MODEL_NUM_VERTICES
#define NUM_EDGES    MODEL_NUM_EDGES
static Point2D gProjVerts[NUM_VERTICES];

/* Model vertices, decoded from the bias-packed kModelVertsPacked[] once at
 * startup (model_init): x/y per vertex; z is the constant MODEL_Z.  BSS is
 * free — the packed form only exists to keep the binary small. */
static int16_t gModelVerts[NUM_VERTICES][2];

static void model_init(void) {
    unsigned i;
    for (i = 0; i < NUM_VERTICES; i++) {
        const uint8_t *p = kModelVertsPacked[i];
        uint16_t xb = (uint16_t)(((uint16_t)p[0] << 5) | (p[1] >> 3));
        uint16_t yb = (uint16_t)(((uint16_t)(p[1] & 7) << 8) | p[2]);
        gModelVerts[i][0] = (int16_t)(xb + MODEL_X_BIAS);
        gModelVerts[i][1] = (int16_t)(yb + MODEL_Y_BIAS);
    }
}

/* Rotate vertex i around Y then X axes using precomputed sin/cos values.
 * Caller hoists the 4 trig lookups outside the per-vertex loop (PERF-2).
 * Vertex coords post-scale: max ~3072 (3*FP_ONE); a rotation preserves the
 * vector norm, so any single axis is bounded by sqrt(2)*3072 ≈ 4344 after
 * mixing two axes — well within int16_t range. */
static inline Point3DInt rotate(unsigned i,
    int16_t cosY, int16_t sinY, int16_t cosX, int16_t sinX) {
    Point3DInt p_out;
    int16_t x, y, z, temp_x, temp_z;

    x = gModelVerts[i][0];
    y = gModelVerts[i][1];
    z = MODEL_Z;

    temp_x = (int16_t)(mul_fp(x, cosY) + mul_fp(z, sinY));
    temp_z = (int16_t)(mul_fp(z, cosY) - mul_fp(x, sinY));
    x = temp_x;
    z = temp_z;

    p_out.x = x;
    p_out.y = (int16_t)(mul_fp(y, cosX) + mul_fp(z, sinX));
    p_out.z = (int16_t)(mul_fp(z, cosX) - mul_fp(y, sinX));

    return p_out;
}

/* ── Camera / projection ──────────────────────────────────────────────────── */
#define FOCAL        128       /* focal length (2^7); gives ~102° HFOV; power-of-2
                                * so val*FOCAL == val<<7, avoiding muls on m68k   */
#define CAM_X_INIT   ((int16_t)(FP_ONE / 2))   /* centered on the course, ±0.5 units */
#define CAM_ZSPEED_BASE  64   /* Z advance per frame on round 1 (= FP_ONE/16)   */
#define CAM_ZSPEED_STEP   8   /* increase per round (~12.5%)                     */
#define CAM_ZSPEED_MAX  192   /* ceiling (~3× base)                              */
#define CAM_ZSPEED_MIN   32   /* throttle floor (cruise speed control)           */
#define THROTTLE_STEP     2   /* cam_zspeed change/frame holding Up/Down in
                               * cruise — addq/subq range like the physics consts */

/* ── Lateral physics ─────────────────────────────────────────────────────── *
 * Constants chosen so net per-frame deltas fit in addq/subq range (1-8)      *
 * on m68k.                                                                   */
#define DRAG_SHIFT      4   /* drag: vel -= vel>>4  (GCC arithmetic shift ok)   */
#define STEER           4   /* unused directly; base unit for CRUISE_STEER      */
#define VEL_X_MAX       8   /* unused directly; base unit for CRUISE_VEL_X_MAX  */
#define CRUISE_STEER      (4 * STEER)      /* lateral accel while racing         */
#define CRUISE_VEL_X_MAX  (6 * VEL_X_MAX)  /* lateral speed cap while racing     */

#define CRUISE_ALT    ((int16_t)(2 * FP_ONE))  /* fixed camera altitude          */

/* ── Lap timing ──────────────────────────────────────────────────────────── *
 * STUN_FRAMES: crash penalty (time + speed, decision 1).                     *
 * GATE_MIN_FRAMES: minimum dwell at the victory screen before FIRE arms.     *
 * LAP_JOIN_MAX: see peer_gate_ok in race_update — bounds the RS_CRUISE       *
 * clause to a freshly launched peer, never a mid-lap one.                   */
#define STUN_FRAMES      75
#define GATE_MIN_FRAMES  75
#define LAP_JOIN_MAX    ((int16_t)(2 * FP_ONE))

/* ── Alien enemies ───────────────────────────────────────────────────────── */
/* Hit tolerance is computed per-collision to match the alien's visual half-width:
 * aim_tol = max(ALIEN_SCALE_W, ALIEN_MIN_PIX * z) / FOCAL  (world units)
 * This equals ALIEN_SCALE_W/FOCAL = 48 for close aliens (z ≤ 2048) and grows
 * proportionally for distant ones where ALIEN_MIN_PIX clamps the pixel size. */
#define ALIEN_SCALE_W  (6 * FP_ONE)              /* screen half-width  at z=FP_ONE   */
#define ALIEN_MIN_PIX  3                          /* min half-size (far away)         */
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
#define ALIEN_GAP_BASE   ((int16_t)(7 * FP_ONE / 2))
#define ALIEN_GAP_STEP   ((int16_t)(FP_ONE / 2))
#define ALIEN_GAP_MIN    ((int16_t)(5 * FP_ONE / 4))
/* How far ahead (beyond GRID_ZFAR) an alien materializes before it would be
 * visible — must clear the missile-hit window's overshoot so a materializing
 * alien is never skipped by update_missiles() the frame it appears. */
#define ALIEN_SPAWN_LEAD ((int16_t)(2 * FP_ONE))
/* Aliens are freed once this far behind the camera; must be < -CAM_ZSPEED_MAX
 * so alien_hit_player()'s (0, -cam_zspeed] crossing window is never cut short. */
#define ALIEN_DESPAWN_Z  ((int16_t)(-(FP_ONE / 2)))
/* Minimum z for draw_alien reciprocal safety: focal_rcp = FOCAL*FP_ONE/z (int16_t).
 * mul_fp(focal_rcp, max_wx_diff) must fit int16_t: max|(wx-cam_x)|=9*FP_ONE=9216,
 * focal_rcp ≤ 32767*1024/9216 = 3640 → z ≥ FOCAL*FP_ONE/3640 = 36.1; use 40 for margin.
 * Also hw = mul_fp(focal_rcp, ALIEN_SCALE_W)>>7: 3277*6144>>10>>7 = 153, well within range. */
#define ALIEN_ZMIN     ((int16_t)40)

/* ── Missiles (MISSILE_COUNT lives in vquest.h with MissileSet) ──────────── */
#define MISSILE_SPEED_FACTOR  3   /* missile speed = cam_zspeed * this factor   */
#define MISSILE_GUN_SEP  20   /* screen-space half-separation of the two cannons */
/* Segment half-length in z-units.  GRID_ZFAR=8192=2^13; the segment slides
 * along the cannon→horizon line as z increases.  400 ≈ 2 frames of travel. */
#define MISSILE_SEG_HZ   400

/* ── Perspective ground grid (world units; FP_ONE = 1.0 unit) ────────────── */
/*
 * Projection:  screen_x = 160 + wx*FOCAL/z_rel
 *              screen_y = 100 + cam_y*FOCAL/z_rel   (horizon at y=100)
 *
 * Horizontal lines tile by wrapping their z_rel with z_phase each frame.
 * Vertical lines span GRID_ZNEAR..GRID_ZFAR unchanged (converge to VP).
 */
#define GRID_ZNEAR   ((int16_t)(FP_ONE / 2))   /* 0.5 units — near clip         */
#define GRID_ZFAR    ((int16_t)(8 * FP_ONE))   /* 8.0 units — far edge          */
#define GRID_ZSTEP   ((int16_t)(FP_ONE))        /* 1.0 unit between Z rows       */
#define GRID_ZDIVS   7                           /* 8 horizontal lines            */
#define GRID_XHALF   ((int16_t)(5 * FP_ONE))   /* ±5 units wide                 */
#define GRID_XDIVS   10                          /* 11 vertical lines             */
#define GRID_XSTEP   ((int16_t)(FP_ONE))        /* 1.0 unit column spacing       */
#define GRID_NUM_LINES ((GRID_XDIVS + 1) + (GRID_ZDIVS + 1))   /* 11+8 = 19    */


#define SCREEN_WIDTH_HALF  (SCREEN_WIDTH  / 2)
#define SCREEN_HEIGHT_HALF (SCREEN_HEIGHT / 2)

/* Orthographic logo projection (not perspective — grid uses divs16() for that).
 * FP_SHIFT-5 = 5: screen_x = 160 + p.x/32 → 32 px per FP_ONE unit laterally.
 * Z offset uses FP_SHIFT-4 = 6 → 16 px per FP_ONE unit, giving a shallower
 * isometric feel on the depth axis than the lateral axes. */
static inline Point2D project(Point3DInt p) {
    Point2D out;
    out.x = SCREEN_WIDTH_HALF  + (p.x >> (FP_SHIFT - 5));
    out.y = SCREEN_HEIGHT_HALF + (p.y >> (FP_SHIFT - 5)) - (p.z >> (FP_SHIFT - 4));
    return out;
}

/* 3-D world-space line pair (grid stored as world coords, projected per frame) */
typedef struct { Point3DInt p0, p1; } Line3D;


static Line3D gGridWorld[GRID_NUM_LINES];

/* Store the ground grid as 3-D world coordinates (projected per frame) */
static void build_grid(void) {
    int i = 0, xi, zi;
    int16_t x, z;

    /* Horizontal lines: constant Z rows (z_phase applied at render time) */
    for (zi = 0; zi <= GRID_ZDIVS; zi++) {
        z = (int16_t)(GRID_ZNEAR + zi * GRID_ZSTEP);
        gGridWorld[i].p0.x = (int16_t)(-GRID_XHALF); gGridWorld[i].p0.y = 0; gGridWorld[i].p0.z = z;
        gGridWorld[i].p1.x =           GRID_XHALF;   gGridWorld[i].p1.y = 0; gGridWorld[i].p1.z = z;
        i++;
    }

    /* Vertical lines: span full Z range, converge to vanishing point */
    for (xi = 0; xi <= GRID_XDIVS; xi++) {
        x = (int16_t)(-GRID_XHALF + xi * GRID_XSTEP);
        gGridWorld[i].p0.x = x; gGridWorld[i].p0.y = 0; gGridWorld[i].p0.z = GRID_ZNEAR;
        gGridWorld[i].p1.x = x; gGridWorld[i].p1.y = 0; gGridWorld[i].p1.z = GRID_ZFAR;
        i++;
    }
}

/* gLines / gNLines / append_line live in draw.c (included via draw.h). */
#define STRIP_LINES  4  /* near/far crossbars + 2 guides */
#define FUEL_LINES   1
#define ARROW_LINES  2  /* 2 segments per arrow; one direction shown at a time */


/*
 * ALL endpoints must be clamped to [1,319] x [1,199] before calling
 * backend_draw_lines: the Atari SegmentedLine assembly takes unsigned
 * short coordinates — passing negative values causes an address error.
 */
#define SC_X0   1
#define SC_X1 319
#define SC_Y0   1
#define SC_Y1 199

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/*
 * divs16 — force the m68k hardware divs.w instruction (32÷16→16).
 * CALLER MUST ENSURE the quotient fits in int16_t.  NOTE: divs.w does NOT
 * trap on quotient overflow — it sets the V flag and leaves a garbage low
 * word (only division by zero traps).  Safety therefore rests entirely on the
 * caller's bound plus the CLAMP at each call site; the assert catches
 * violations in debug builds on all targets.
 *
 * Reciprocal idiom — when multiple quantities share the same z denominator:
 *   int16_t rcp = divs16((int32_t)SCALE << FP_SHIFT, z);  // one div
 *   int16_t a   = mul_fp(rcp, A);   // one muls.w replaces a divs.w
 *   int16_t b   = mul_fp(rcp, B);   // …and another
 * Constraint: SCALE*FP_ONE/z_min must fit in int16_t for the chosen SCALE.
 */
static inline int16_t divs16(int32_t num, int16_t den) {
    assert(num / den >= -32768 && num / den <= 32767);
#ifdef __m68k__
    __asm__("divs.w %1,%0" : "+d"(num) : "dmi"(den));
    return (int16_t)num;   /* quotient in low word after divs */
#else
    return (int16_t)(num / den);
#endif
}

/* Minimum z_rel that keeps the horizontal-line reciprocal computation within
 * int16_t bounds.  focal_rcp = FOCAL*FP_ONE/z_rel (one divs16); then
 * x_off = mul_fp(focal_rcp, GRID_XHALF) = FOCAL*GRID_XHALF/z_rel.
 * At z_rel=20: FOCAL*FP_ONE/20 = 6554 → mul_fp(6554,5120)>>10 = 32770 overflows.
 * At z_rel=21: FOCAL*FP_ONE/21 = 6241 → mul_fp(6241,5120)>>10 = 31202 fits.
 * Visually identical — line already clamps to screen edges at this near. */
#define HLINE_ZMIN ((int16_t)21)

static void render_grid(bool enabled, int16_t cam_y, int16_t z_phase, int16_t cam_x) {
    if (!enabled) return;
    unsigned int i;
    int16_t z_wrap = (int16_t)((GRID_ZDIVS + 1) * GRID_ZSTEP);

    for (i = 0; i < (unsigned int)(GRID_ZDIVS + 1); i++) {
        int16_t z_rel = (int16_t)(gGridWorld[i].p0.z - z_phase);
        if (z_rel <= 0) z_rel += z_wrap;
        if (z_rel < HLINE_ZMIN) z_rel = HLINE_ZMIN;
        /* focal_rcp = FOCAL*FP_ONE/z_rel (max 6241 at z=21, fits int16_t).
         * Two muls replace two divs; see reciprocal idiom note at divs16(). */
        int16_t focal_rcp = divs16((int32_t)FOCAL << FP_SHIFT, z_rel);
        int16_t x_off     = mul_fp(focal_rcp, GRID_XHALF);
        int16_t vp_shift  = mul_fp(focal_rcp, cam_x);
        int16_t y         = (int16_t)(SCREEN_HEIGHT_HALF + mul_fp(focal_rcp, cam_y));
        append_line(CLAMP((int16_t)(SCREEN_WIDTH_HALF - vp_shift - x_off), SC_X0, SC_X1),
                    CLAMP(y, SC_Y0, SC_Y1),
                    CLAMP((int16_t)(SCREEN_WIDTH_HALF - vp_shift + x_off), SC_X0, SC_X1),
                    CLAMP(y, SC_Y0, SC_Y1));
    }

    {
        int16_t cam_x_near = (int16_t)((int32_t)cam_x * FOCAL / GRID_ZNEAR);
        int16_t cam_x_far  = (int16_t)((int32_t)cam_x * FOCAL / GRID_ZFAR);
        int16_t cam_y_near = (int16_t)(SCREEN_HEIGHT_HALF + (int32_t)cam_y * FOCAL / GRID_ZNEAR);
        int16_t cam_y_far  = (int16_t)(SCREEN_HEIGHT_HALF + (int32_t)cam_y * FOCAL / GRID_ZFAR);
        int32_t x_base_near = SCREEN_WIDTH_HALF - cam_x_near;
        int32_t x_base_far  = SCREEN_WIDTH_HALF - cam_x_far;
        for (i = GRID_ZDIVS + 1; i < (unsigned int)GRID_NUM_LINES; i++) {
            int16_t wx = gGridWorld[i].p0.x;
            int32_t x0 = x_base_near + (int32_t)wx * FOCAL / GRID_ZNEAR;
            int32_t y0 = cam_y_near;
            int32_t x1 = x_base_far  + (int32_t)wx * FOCAL / GRID_ZFAR;
            int32_t y1 = cam_y_far;
            /* x1's offset from screen centre is x0's scaled by GRID_ZNEAR/GRID_ZFAR
             * (= 1/16), so x1 off-screen implies x0 off the same side, 16× further:
             * the whole line is invisible.  Skipping keeps x1 in [SC_X0,SC_X1] for
             * append_line — SegmentedLine has no clipping (segline.s). */
            if (x1 < SC_X0 || x1 > SC_X1) continue;
            if (x0 < SC_X0 || x0 > SC_X1) {
                int16_t edge = (x0 < SC_X0) ? SC_X0 : SC_X1;
                int16_t dx   = (int16_t)(x1 - x0);
                int16_t dy   = (int16_t)(y1 - y0);
                y0 += divs16((int32_t)dy * (edge - (int16_t)x0), dx);
                x0  = edge;
            }
            if (y0 > SC_Y1) {
                int16_t dy = (int16_t)(y1 - y0);
                int16_t dx = (int16_t)(x1 - x0);
                if (dy != 0)
                    x0 += divs16((int32_t)dx * (SC_Y1 - (int16_t)y0), dy);
                y0 = SC_Y1;
            }
            append_line((int16_t)CLAMP(x0, SC_X0, SC_X1), (int16_t)y0,
                        (int16_t)x1, (int16_t)CLAMP(y1, SC_Y0, SC_Y1));
        }
    }
}

static void render_logo(bool enabled, int16_t angleY, int16_t angleX) {
    if (!enabled) return;
    unsigned int i;
    /* Hoist trig lookups outside vertex loop — identical for all 309 vertices */
    int16_t cosY = fastCos(angleY), sinY = fastSin(angleY);
    int16_t cosX = fastCos(angleX), sinX = fastSin(angleX);
    for (i = 0; i < NUM_VERTICES; ++i) {
        Point3DInt t = rotate(i, cosY, sinY, cosX, sinX);
        gProjVerts[i] = project(t);
    }
    for (i = 0; i < NUM_EDGES; ++i) {
        Point2D p1 = gProjVerts[kModelEdges[i][0]];
        Point2D p2 = gProjVerts[kModelEdges[i][1]];
        p1.x = CLAMP(p1.x, SC_X0, SC_X1); p1.y = CLAMP(p1.y, SC_Y0, SC_Y1);
        p2.x = CLAMP(p2.x, SC_X0, SC_X1); p2.y = CLAMP(p2.y, SC_Y0, SC_Y1);
        append_line(p1.x, p1.y, p2.x, p2.y);
    }
}

/*
 * Project and draw a ground-level rectangle: near crossbar at z_near,
 * far crossbar at z_far, lateral half-width x_half.
 * Near endpoints are clipped to y=SC_Y1 if they project below the screen.
 */
static void draw_ground_strip(int16_t x_half, int16_t z_near, int16_t z_far,
                               int16_t cam_x, int16_t cam_y, bool near_crossbar)
{
    assert(z_far >= HLINE_ZMIN && z_near < z_far);
    /* Exact truncating division per endpoint — deliberately NOT the reciprocal
     * idiom: the near endpoints must land on the same pixels as render_grid's
     * exactly-divided verticals (strip edges sit on grid lines), and a
     * truncated reciprocal drifts up to ~4px at finish-line z.  Quotient fit
     * relies on the caller's z bounds (render_finish_line: z_near ≥
     * GRID_ZNEAR, z_far = z_near + GRID_ZSTEP), not the assert above;
     * divs16's debug assert catches a caller that violates them.  6 divs,
     * per-frame, one strip at most — cheap. */
    int16_t sxl_f = (int16_t)(SCREEN_WIDTH_HALF + divs16((-x_half - (int32_t)cam_x) * FOCAL, z_far));
    int16_t sxr_f = (int16_t)(SCREEN_WIDTH_HALF + divs16(( x_half - (int32_t)cam_x) * FOCAL, z_far));
    int16_t sy_f  = (int16_t)(SCREEN_HEIGHT_HALF + divs16((int32_t)cam_y * FOCAL, z_far));
    /* Near endpoint projections (int32 to allow off-screen values before clip) */
    int32_t sxl_n = SCREEN_WIDTH_HALF + divs16((-x_half - (int32_t)cam_x) * FOCAL, z_near);
    int32_t sxr_n = SCREEN_WIDTH_HALF + divs16(( x_half - (int32_t)cam_x) * FOCAL, z_near);
    int32_t sy_n  = SCREEN_HEIGHT_HALF + divs16((int32_t)cam_y * FOCAL, z_near);
    /* Slide near endpoints up to y=SC_Y1 if below screen */
    if (sy_n > SC_Y1) {
        int16_t dy = (int16_t)((int32_t)sy_f - sy_n);
        if (dy != 0) {
            int16_t dt = (int16_t)(SC_Y1 - sy_n);
            sxl_n += divs16((int32_t)(sxl_f - sxl_n) * dt, dy);
            sxr_n += divs16((int32_t)(sxr_f - sxr_n) * dt, dy);
        }
        sy_n = SC_Y1;
    }
    int16_t sy_nc  = (int16_t)sy_n;
    int16_t sy_fc  = CLAMP(sy_f,           SC_Y0, SC_Y1);
    int16_t sxl_nc = CLAMP((int16_t)sxl_n, SC_X0, SC_X1);
    int16_t sxr_nc = CLAMP((int16_t)sxr_n, SC_X0, SC_X1);
    int16_t sxl_fc = CLAMP(sxl_f,          SC_X0, SC_X1);
    int16_t sxr_fc = CLAMP(sxr_f,          SC_X0, SC_X1);
    if (near_crossbar)
        append_line(sxl_nc, sy_nc, sxr_nc, sy_nc);  /* Near crossbar */
    append_line(sxl_fc, sy_fc, sxr_fc, sy_fc);      /* Far crossbar  */
    append_line(sxl_nc, sy_nc, sxl_fc, sy_fc);      /* Left guide    */
    append_line(sxr_nc, sy_nc, sxr_fc, sy_fc);      /* Right guide   */
}

/* draw_triangle — shared projector for the alien and remote-player triangles.
 * sy is the screen-space vertical centre (eye level for aliens; altitude-
 * projected for the remote player); apex_up flips the orientation so the two
 * read as visually distinct.  Caller guarantees z is in the valid range.
 * focal_rcp = FOCAL*FP_ONE/z (one div); sx uses it directly, hw needs
 * ALIEN_SCALE_W/z = FOCAL*ALIEN_SCALE_W/z / FOCAL, and FOCAL=2^7 so >>7 is exact.
 * All coords are clamped before append_line (SegLine does no clipping). */
static void draw_triangle(int16_t wx, int16_t z, int16_t cam_x,
                          int16_t sy, bool apex_up)
{
    int16_t focal_rcp = divs16((int32_t)FOCAL << FP_SHIFT, z);
    int16_t sx = (int16_t)(SCREEN_WIDTH_HALF + mul_fp(focal_rcp, (int16_t)(wx - cam_x)));
    int16_t hw = (int16_t)(mul_fp(focal_rcp, ALIEN_SCALE_W) >> 7); /* >>7 divides out FOCAL=2^7 */
    if (hw < ALIEN_MIN_PIX) hw = ALIEN_MIN_PIX;
    /* equilateral: hh = hw*sqrt(3)/2; approx 111/128 = 1-1/8-1/128 = 0.8672 (err<0.14%) */
    int16_t hh = (int16_t)(hw - (hw >> 3) - (hw >> 7));
    if (sx - hw > SC_X1 || sx + hw < SC_X0) return;  /* fully off-screen */
    if (sy - hh > SC_Y1 || sy + hh < SC_Y0) return;
    int16_t d      = apex_up ? (int16_t)(-hh) : hh;  /* one select for both */
    int16_t apex_y = (int16_t)(sy + d);
    int16_t base_y = (int16_t)(sy - d);
    int16_t tx = CLAMP(sx,                 SC_X0, SC_X1);   /* apex   */
    int16_t ty = CLAMP(apex_y,             SC_Y0, SC_Y1);
    int16_t lx = CLAMP((int16_t)(sx - hw), SC_X0, SC_X1);   /* base   */
    int16_t ly = CLAMP(base_y,             SC_Y0, SC_Y1);
    int16_t rx = CLAMP((int16_t)(sx + hw), SC_X0, SC_X1);
    append_line(tx, ty, lx, ly);
    append_line(lx, ly, rx, ly);
    append_line(rx, ly, tx, ty);
}

/* draw_alien — eye-level triangle, apex down.
 * ALIEN_ZMIN=40 bounds focal_rcp to 3277; mul_fp(3277, 9*FP_ONE)=29476 fits int16. */
static void draw_alien(int16_t wx, int16_t z, int16_t cam_x)
{
    if (z < ALIEN_ZMIN || z > GRID_ZFAR) return;
    draw_triangle(wx, z, cam_x, SCREEN_HEIGHT_HALF, false);
}

/* draw_remote_player — apex-up triangle, vertically offset by the altitude
 * difference (cam_y - alt): zero in cruise (both at CRUISE_ALT), showing the
 * peer climbing/descending during takeoff/landing.  z is pre-clamped by the
 * caller to [REMOTE_Z_NEAR, GRID_ZFAR] (REMOTE_Z_NEAR=512 bounds focal_rcp to
 * 256; mul_fp(256, ±6144) = ±3072, fits int16). */
#define REMOTE_Z_NEAR  ((int16_t)(FP_ONE / 2))
static void draw_remote_player(int16_t wx, int16_t z, int16_t alt,
                               int16_t cam_x, int16_t cam_y)
{
    int16_t focal_rcp = divs16((int32_t)FOCAL << FP_SHIFT, z);
    int16_t sy = (int16_t)(SCREEN_HEIGHT_HALF + mul_fp(focal_rcp, (int16_t)(cam_y - alt)));
    draw_triangle(wx, z, cam_x, sy, true);
}

/* draw_remote_missile — short vertical tick at the peer missile's projected
 * position, eye level (missiles fly at cruise altitude like aliens).
 * RMISSILE_ZMIN=48 keeps the lateral mul in range: focal_rcp ≤ 2730 and
 * |wx-cam_x| ≤ 12288 (both players clamped to ±6144) → ≤ 32760. */
#define RMISSILE_ZMIN  ((int16_t)48)
#define RMISSILE_H     ((int16_t)(3 * FP_ONE))   /* world half-height of the tick */
static void draw_remote_missile(int16_t wx, int16_t z, int16_t cam_x)
{
    int16_t focal_rcp, sx, hh;
    if (z < RMISSILE_ZMIN || z > GRID_ZFAR) return;
    focal_rcp = divs16((int32_t)FOCAL << FP_SHIFT, z);
    sx = (int16_t)(SCREEN_WIDTH_HALF + mul_fp(focal_rcp, (int16_t)(wx - cam_x)));
    if (sx < SC_X0 || sx > SC_X1) return;
    hh = (int16_t)(mul_fp(focal_rcp, RMISSILE_H) >> 7);
    if (hh < 2) hh = 2;
    append_line(sx, CLAMP((int16_t)(SCREEN_HEIGHT_HALF - hh), SC_Y0, SC_Y1),
                sx, CLAMP((int16_t)(SCREEN_HEIGHT_HALF + hh), SC_Y0, SC_Y1));
}

/* draw_missile — two perspective-correct segments sliding along their cannon→horizon
 * trajectories.  Both endpoints use 1/t perspective so that when missile_z ≈ alien_z
 * the segment appears at (sx, SCREEN_HEIGHT_HALF) — the alien's screen position.
 *
 * Formula: pos = horizon_pos + (gun_pos - horizon_pos) * HLINE_ZMIN / t
 *   • t = HLINE_ZMIN → pos = gun_pos   (missile just fired, segment at cannon)
 *   • t → ∞          → pos = horizon_pos (missile at vanishing point)
 *   • t ≈ alien_z    → pos ≈ alien screen pos (hit looks correct)
 *
 * Left  cannon: screen x = 160-SEP, y = SC_Y1
 * Right cannon: screen x = 160+SEP, y = SC_Y1
 * Horizon target: screen x = sx (perspective projection of missile_x), y = SCREEN_HEIGHT_HALF */
static void draw_missile(int16_t wx, int16_t z, int16_t cam_x)
{
    int32_t t0, t1;
    int16_t sx, lx, rx, y0, y1, lx0, lx1, rx0, rx1;
    if (z < HLINE_ZMIN || z > GRID_ZFAR) return;
    /* wx = cam_x at fire time; missile_z starts at HLINE_ZMIN and grows each frame,
     * so |wx-cam_x| is small at low z and grows as cam drifts.  In the worst case
     * (z=HLINE_ZMIN, large lateral drift) this divs16 could overflow int16_t and
     * trap on m68k.  The CLAMP absorbs the wrong sx value visually — but consider
     * converting to the focal_rcp pattern if missile range or cam_x limits grow. */
    sx = CLAMP((int16_t)(SCREEN_WIDTH_HALF + divs16((int32_t)(wx - cam_x) * FOCAL, z)),
               SC_X0, SC_X1);
    lx = (int16_t)(SCREEN_WIDTH_HALF - MISSILE_GUN_SEP);   /* left  cannon x */
    rx = (int16_t)(SCREEN_WIDTH_HALF + MISSILE_GUN_SEP);   /* right cannon x */

    t0 = (int32_t)z - MISSILE_SEG_HZ; if (t0 < 0) t0 = 0;
    t1 = (int32_t)z + MISSILE_SEG_HZ; if (t1 > GRID_ZFAR) t1 = GRID_ZFAR;

    /* Perspective position at parameter t: horizon + (gun - horizon) * HLINE_ZMIN / t.
     * t0 == 0 means segment tail is still at the cannon — use gun coordinates directly.
     * hzmin_rcp = HLINE_ZMIN*FP_ONE/t (one div each); two muls replace two divs per t.
     * Max at t=HLINE_ZMIN=21: hzmin_rcp=FP_ONE=1024; mul_fp(1024,179)=179 (fits int16_t). */
    if (t0 > 0) {
        int16_t t0s     = (int16_t)t0;
        int16_t hzmin_rcp_t0 = divs16((int32_t)HLINE_ZMIN << FP_SHIFT, t0s);
        y0  = (int16_t)(SCREEN_HEIGHT_HALF + mul_fp(hzmin_rcp_t0, (int16_t)(SC_Y1 - SCREEN_HEIGHT_HALF)));
        lx0 = (int16_t)(sx               - mul_fp(hzmin_rcp_t0, (int16_t)(sx - lx)));
        rx0 = (int16_t)(sx               + mul_fp(hzmin_rcp_t0, (int16_t)(rx - sx)));
    } else {
        y0 = SC_Y1; lx0 = lx; rx0 = rx;
    }
    {
        int16_t t1s     = (int16_t)t1;
        int16_t hzmin_rcp_t1 = divs16((int32_t)HLINE_ZMIN << FP_SHIFT, t1s);
        y1  = (int16_t)(SCREEN_HEIGHT_HALF + mul_fp(hzmin_rcp_t1, (int16_t)(SC_Y1 - SCREEN_HEIGHT_HALF)));
        lx1 = (int16_t)(sx               - mul_fp(hzmin_rcp_t1, (int16_t)(sx - lx)));
        rx1 = (int16_t)(sx               + mul_fp(hzmin_rcp_t1, (int16_t)(rx - sx)));
    }

    append_line(CLAMP(lx0, SC_X0, SC_X1), CLAMP(y0, SC_Y0, SC_Y1),
                      CLAMP(lx1, SC_X0, SC_X1), CLAMP(y1, SC_Y0, SC_Y1));
    append_line(CLAMP(rx0, SC_X0, SC_X1), CLAMP(y0, SC_Y0, SC_Y1),
                      CLAMP(rx1, SC_X0, SC_X1), CLAMP(y1, SC_Y0, SC_Y1));
}

/* ── Per-element render helpers (each owns its enabled guard) ────────────── */

/* render_finish_line — full-track-width ground band at the lap line.
 * Same grid-snap trick as the old landing strip.  z floor is GRID_ZNEAR
 * (not HLINE_ZMIN): with x_half = GRID_XHALF and |cam_x| ≤ 6*FP_ONE the
 * divs16 quotient (5120+6144)*128/512 = 2816 stays int16; at HLINE_ZMIN
 * it would overflow.  The band vanishes ~0.5 units before the camera
 * crosses it, which reads as passing under the ship. */
static inline void render_finish_line(bool enabled, int16_t finish_dist,
                                      int16_t cam_x, int16_t cam_y,
                                      int16_t z_phase) {
    int16_t sz, rem;
    if (!enabled || finish_dist <= 0 || finish_dist > GRID_ZFAR) return;
    sz  = finish_dist;
    rem = (int16_t)((sz + z_phase - GRID_ZNEAR + GRID_ZSTEP) % GRID_ZSTEP);
    sz  = (int16_t)(sz - rem);
    if (sz < GRID_ZNEAR) return;
    draw_ground_strip(GRID_XHALF, sz, (int16_t)(sz + GRID_ZSTEP),
                      cam_x, cam_y, true);
}

/* draw_world_plane / draw_alien_plane (the per-frame composition layer) live
 * in vquest.c: they consume World and RaceState, which physics.c defines
 * after this file is included. */
