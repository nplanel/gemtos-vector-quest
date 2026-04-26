/* physics.c — game state machine, entity logic, and physics for vquest.
 * Unity-included from vquest.c (not a separate TU).
 * Requires: game constants, PhysicsState, GameState, RenderFlags from vquest.h,
 *           rendering functions from render.c (included before this file).   */

static const RenderFlags kStateFlags[] = {
/*                        grid   logo   arrows takeof  land   aliens credits */
/* STATE_TAKEOFF */     { true,  false, true,  true,   false, false, false },
/* STATE_CRUISE  */     { true,  false, true,  false,  true,  true,  false },
/* STATE_LANDING */     { true,  false, true,  false,  true,  true,  false },
/* STATE_CRASH   */     { true,  false, false, false,  false, false, false },
/* STATE_SUCCESS */     { false, true,  false, false,  false, false, true  },
};

static inline bool lateral_crash(int16_t cam_x) {
    return cam_x > CRASH_CAM_X || cam_x < -CRASH_CAM_X;
}

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

#define SUCCESS_FLASH_FRAMES 60  /* ~1 s of blinking runway on good landing */

/* ── Alien / missile helpers ─────────────────────────────────────────────── */

/* Spawn n_aliens aliens spread along the approach corridor, then clear
 * all missile slots.  Called once when transitioning TAKEOFF → CRUISE. */
static void spawn_aliens(int16_t round, int n_aliens,
    int16_t alien_x[], int16_t alien_z[], bool alien_alive[],
    bool missile_alive[])
{
    int i;
    for (i = 0; i < n_aliens; i++) {
        uint16_t r;
        int16_t  mag;
        alien_z[i]    = (int16_t)(LANDING_APPROACH_DIST - (i + 1) * ALIEN_Z_GAP);
        r             = LCG_STEP(round * 37 + i * 13);
        mag           = (int16_t)(FP_ONE + (r >> 13) % (2 * FP_ONE + 1));
        alien_x[i]    = (r & 0x8000) ? mag : (int16_t)(-mag);
        alien_alive[i] = true;
    }
    for (i = n_aliens; i < ALIEN_COUNT; i++) alien_alive[i] = false;
    for (i = 0; i < MISSILE_COUNT; i++) missile_alive[i] = false;
}

/* True if a live alien crossed z=0 this frame and is within FP_ONE laterally.
 * The z window (0, -cam_zspeed] covers exactly the crossing frame so this
 * does not re-trigger on subsequent frames after the alien has passed. */
static bool alien_hit_player(int16_t cam_x, int16_t cam_zspeed,
    int16_t alien_x[], int16_t alien_z[], bool alien_alive[])
{
    int i;
    for (i = 0; i < ALIEN_COUNT; i++) {
        int16_t rel;
        if (!alien_alive[i]) continue;
        if (alien_z[i] > 0 || alien_z[i] <= -cam_zspeed) continue;
        rel = (int16_t)(cam_x - alien_x[i]);
        if (rel < 0) rel = (int16_t)(-rel);
        if (rel < FP_ONE / 4) return true;
    }
    return false;
}

/* Scroll all live aliens toward the camera each frame. */
static void update_aliens(int16_t cam_zspeed, int16_t alien_z[], bool alien_alive[])
{
    int i;
    for (i = 0; i < ALIEN_COUNT; i++) {
        if (alien_alive[i])
            alien_z[i] = (int16_t)(alien_z[i] - cam_zspeed);
    }
}

/* Fire: spawn one missile in the first free slot (one shot per key press). */
static void try_fire_missile(uint8_t keys, int16_t cam_x,
    int16_t missile_x[], int16_t missile_z[], bool missile_alive[])
{
    int i;
    if (!(keys & KEY_FIRE)) return;
    for (i = 0; i < MISSILE_COUNT; i++) {
        if (!missile_alive[i]) {
            missile_x[i]    = cam_x;
            missile_z[i]    = HLINE_ZMIN;
            missile_alive[i] = true;
            #ifdef __m68k__
            zik_sfx(ZIK_FIRE);
            #endif
            return;
        }
    }
}

/* Advance all live missiles and test collision against all live aliens. */
static void update_missiles(int16_t cam_zspeed,
    int16_t missile_x[], int16_t missile_z[], bool missile_alive[],
    int16_t alien_x[],  int16_t alien_z[],  bool alien_alive[])
{
    int mi;
    int16_t missile_speed = (int16_t)(cam_zspeed * MISSILE_SPEED_FACTOR);
    for (mi = 0; mi < MISSILE_COUNT; mi++) {
        int ai;
        if (!missile_alive[mi]) continue;
        missile_z[mi] = (int16_t)(missile_z[mi] + missile_speed);
        if (missile_z[mi] > GRID_ZFAR) { missile_alive[mi] = false; continue; }
        for (ai = 0; ai < ALIEN_COUNT; ai++) {
            int16_t rel, aim_tol;
            if (!alien_alive[ai]) continue;
            if (alien_z[ai] <= 0) continue;          /* already passed player */
            if (missile_z[mi] < alien_z[ai]) continue;
            if (missile_z[mi] > alien_z[ai] + missile_speed + cam_zspeed) continue;
            rel = (int16_t)(missile_x[mi] - alien_x[ai]);
            /* Tolerance matches alien visual half-width: max(ALIEN_SCALE_W, ALIEN_MIN_PIX*z)/FOCAL.
             * gcc folds the constants into asr+add sequences, no muls/divs emitted. */
            aim_tol = ALIEN_SCALE_W / FOCAL;                         /* 48, constant      */
            { int16_t t = alien_z[ai] * ALIEN_MIN_PIX / FOCAL;      /* 3*z/128           */
              if (t > aim_tol) aim_tol = t; }
            if (rel > -aim_tol && rel < aim_tol) {
                alien_alive[ai]   = false;
                missile_alive[mi] = false;
            }
        }
    }
}

/* ── State update functions ───────────────────────────────────────────────── */

static GameState state_takeoff(
    int16_t *takeoff_timer, PhysicsState *ps,
    int16_t *strip_dist, int16_t *crash_timer, int16_t round,
    int16_t alien_x[], int16_t alien_z[], bool alien_alive[],
    bool missile_alive[], uint8_t keys)
{
    if (--(*takeoff_timer) <= 0 && ps->cam_y < CRUISE_ALT) {
        *crash_timer = CRASH_FLASH_FRAMES; return STATE_CRASH;
    }
    apply_vertical(TAKEOFF_THRUST, 0, VEL_Y_MAX, true, keys, &ps->vel_y, &ps->cam_y);
    apply_lateral(STEER, VEL_X_MAX, keys, &ps->vel_x, &ps->cam_x);
    if (lateral_crash(ps->cam_x)) {
        *crash_timer = CRASH_FLASH_FRAMES; return STATE_CRASH;
    }
    if (ps->cam_y >= CRUISE_ALT) {
        ps->cam_y = CRUISE_ALT; ps->vel_y = 0;
        /* cam_x/vel_x intentionally NOT reset — carry into landing */
        *strip_dist = LANDING_APPROACH_DIST;
        spawn_aliens(round, aliens_for_round(round), alien_x, alien_z, alien_alive, missile_alive);
        return STATE_CRUISE;
    }
    return STATE_TAKEOFF;
}

static GameState state_cruise(
    int16_t *strip_dist, PhysicsState *ps, int16_t cam_zspeed,
    int16_t alien_z[], bool alien_alive[],
    int16_t missile_x[], int16_t missile_z[], bool missile_alive[],
    uint8_t keys)
{
    apply_lateral(CRUISE_STEER, CRUISE_VEL_X_MAX, keys, &ps->vel_x, &ps->cam_x);
    *strip_dist = (int16_t)(*strip_dist - cam_zspeed);
    if (*strip_dist <= LANDING_STRIP_MIN) {
        *strip_dist = LANDING_STRIP_MIN;
        return STATE_LANDING;
    }
    update_aliens(cam_zspeed, alien_z, alien_alive);
    try_fire_missile(keys, ps->cam_x, missile_x, missile_z, missile_alive);
    return STATE_CRUISE;
}

static GameState state_landing(
    PhysicsState *ps,
    int16_t *strip_dist, int16_t *strip_x,
    int16_t *round, int16_t *cam_zspeed, int16_t *crash_timer,
    int16_t alien_z[], bool alien_alive[],
    uint8_t keys, uint16_t frame)
{
    update_aliens(*cam_zspeed, alien_z, alien_alive);   /* keep aliens scrolling with the world */
    apply_vertical(TAKEOFF_THRUST, DESCENT_THRUST, VEL_Y_MAX, false, keys, &ps->vel_y, &ps->cam_y);
    apply_lateral(STEER, VEL_X_MAX, keys, &ps->vel_x, &ps->cam_x);
    if (lateral_crash_landing(ps->cam_x, *strip_x)) {
        *crash_timer = CRASH_FLASH_FRAMES; return STATE_CRASH;
    }
    if (ps->cam_y <= 0) {
        int16_t abs_vel_y = (int16_t)(ps->vel_y < 0 ? -ps->vel_y : ps->vel_y);
        int16_t rel_x     = (int16_t)(ps->cam_x - *strip_x);
        int16_t abs_rel_x = (int16_t)(rel_x < 0 ? -rel_x : rel_x);
        if (abs_vel_y < CRASH_VEL_Y && abs_rel_x < LAND_CAM_X) {
            (*round)++;
            *cam_zspeed = (int16_t)(*cam_zspeed + CAM_ZSPEED_STEP);
            if (*cam_zspeed > CAM_ZSPEED_MAX) *cam_zspeed = CAM_ZSPEED_MAX;
            ps->cam_y    = CAM_Y_INIT;
            ps->vel_y    = 0; ps->vel_x = 0;
            *strip_x  = next_strip_x(*round, frame);
            *crash_timer = SUCCESS_FLASH_FRAMES;
            hud_draw(*round);
            return STATE_SUCCESS;
        }
        *crash_timer = CRASH_FLASH_FRAMES; return STATE_CRASH;
    }
    *strip_dist = (int16_t)(*strip_dist - *cam_zspeed);
    return STATE_LANDING;
}

static GameState state_crash(
    bool *flash, int16_t *crash_timer, int16_t *round,
    int16_t *takeoff_timer, int16_t *cam_zspeed,
    int16_t *strip_x, PhysicsState *ps, uint16_t frame)
{
    *flash = true;
    if (--(*crash_timer) <= 0) {
        *round         = 1;
        *takeoff_timer = TAKEOFF_FRAMES_BASE;
        *cam_zspeed    = CAM_ZSPEED_BASE;
        *strip_x = next_strip_x(1, frame);
        ps->cam_y = CAM_Y_INIT; ps->vel_y = 0; ps->cam_x = CAM_X_INIT; ps->vel_x = 0;
        hud_draw(1);
        return STATE_TAKEOFF;
    }
    return STATE_CRASH;
}

static GameState state_success(
    int16_t *angleY, int16_t *angleX, int16_t *crash_timer,
    PhysicsState *ps,
    int16_t *takeoff_timer, int16_t angleYinc, int16_t angleXinc, uint8_t keys)
{
    *angleY = (int16_t)(*angleY + angleYinc);
    *angleX = (int16_t)(*angleX + angleXinc);
    if (--(*crash_timer) <= 0 || keys) {
        ps->cam_y = CAM_Y_INIT; ps->cam_x = CAM_X_INIT; ps->vel_y = 0; ps->vel_x = 0;
        *takeoff_timer = TAKEOFF_FRAMES_BASE;
        return STATE_TAKEOFF;
    }
    return STATE_SUCCESS;
}

