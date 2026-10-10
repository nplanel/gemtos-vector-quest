/* race.c — the remote-player slot: serial peer or bot, ghost, peer missiles
 * and mines, kills, link health, and the speed modifiers that depend on the
 * opponent.
 * Unity-included from vquest.c after bot.c (not a separate TU).
 * Requires: World and the state helpers from physics.c, the bot from bot.c,
 *           the wire format from serial.h. */

/* ── Race / multiplayer slot ─────────────────────────────────────────────── */

/* Frames without a peer packet before a 2-player peer counts as gone (~1s):
 * the ghost freezes, sending falls back to beaconing, and the gate stops
 * releasing.  Peers send every frame once paired, so any stall past this is
 * a real loss.  The opponent source itself never changes mid-race: 1 vs 2
 * players is picked with F1 at the gate (race_set_mode) and held until the
 * next one, so there is no bot takeover to arbitrate. */
#define REMOTE_TIMEOUT_FRAMES 50
/* Largest inter-packet gap (frames) that still yields a usable peer-speed
 * measurement for dead reckoning.  A beaconing peer sends every 16th frame,
 * so 16 admits the first packet after pairing; anything wider means a real
 * dropout, where the two samples straddle unknown gameplay and the implied
 * rate is meaningless. */
#define PEER_RATE_MAX_GAP 16
/* Consecutive packets carrying the KILL bit after our missile hits the peer,
 * so a lost byte cannot drop the kill (receiver edge-triggers on it). */
#define KILL_REPEAT 8

typedef struct {
    RemoteState remote;            /* last known peer/bot state            */
    int16_t  remote_idle;          /* saturating; starts timed-out         */
    uint16_t peer_prog_rx;         /* newest packet's progress, un-extrapolated */
    int16_t  peer_speed;           /* dead-reckoned peer progress per frame */
    MissileSet rmissiles;          /* peer missiles, local frame           */
    int16_t  kill_pending;         /* outgoing KILL packets left to send   */
    bool     kill_latched;         /* edge detector for incoming KILL      */
    Bot      bot;
    bool     bot_enabled;          /* 1 player: the bot is the opponent and
                                    * the serial port is never touched.
                                    * Fixed for a whole race (race_set_mode). */
    bool     test_solo;            /* TEST-ONLY ("nobot" argv): a 2-player
                                    * gate may release with no peer, for the
                                    * peerless test_race.sh runs.  Never set
                                    * from the UI. */
    bool     ghost_show;           /* per-frame outputs for the renderer   */
    int16_t  ghost_z;
    int16_t  peer_rel_z;           /* rel_depth(peer, us): ghost, drafting, missiles, mines */
    bool     peer_finished;        /* latched: peer crossed their final line this race */
    bool     peer_gate_ok;         /* per-frame output: gate may release next frame */
    bool     remote_live;          /* per-frame: fresh peer or active bot   */
    uint16_t rx_count;             /* packets decoded this session (debug)  */
    uint8_t  link_window;   /* frames elapsed in the current health window */
    uint8_t  link_rx;       /* packets decoded in the current window       */
    uint8_t  link_state;    /* LINK_*                                      */
    bool     link_changed;  /* set for one frame on a transition           */
} RaceState;

/* race_reset_remote — "no opponent yet": the remote slot, liveness and link
 * health all back to their boot values.  remote must be cleared (RS_WAIT)
 * on a mode switch, or a stale bot RS_READY would release a 2-player gate
 * with no peer. */
static void race_reset_remote(RaceState *rs) {
    memset(&rs->remote, 0, sizeof(rs->remote));
    rs->remote_idle  = REMOTE_TIMEOUT_FRAMES;
    rs->link_window  = 0;
    rs->link_rx      = 0;
    rs->link_state   = LINK_NONE;
    rs->link_changed = false;
    rs->peer_speed   = 0;
    rs->remote_live  = false;
    rs->peer_gate_ok = false;   /* read by state_gate before race_update runs */
    bot_init(&rs->bot);
}

static void race_init(RaceState *rs, bool bot_enabled) {
    memset(rs, 0, sizeof(*rs));
    race_reset_remote(rs);
    rs->bot_enabled = bot_enabled;
    rs->test_solo   = !bot_enabled;
}

/* race_set_mode — F1 at the gate: 1 player (bot) or 2 players (serial peer),
 * held for the whole next race.  Entering 2 players flushes the port: in
 * 1-player mode nothing reads it, so a beaconing peer has filled the input
 * buffer with stale packets that would otherwise fake a live, READY peer. */
static void race_set_mode(RaceState *rs, bool bot_enabled) {
    race_reset_remote(rs);
    rs->bot_enabled = bot_enabled;
    if (!bot_enabled) serial_flush();
}

/* race_finish_reset — clear the peer FINISHED latch; called by main on every
 * GATE→COUNTDOWN launch (the same frame race_start() runs). */
static void race_finish_reset(RaceState *rs) { rs->peer_finished = false; }

/* joinable_launch — true for a peer/bot state our gate handshake may launch
 * alongside: freshly into lap 1 and not yet past LAP_JOIN_MAX.  Shared by the
 * bot's player_going gate (local frame) and peer_gate_ok's RS_CRUISE clause
 * (remote frame) so the two mirror-image predicates cannot drift apart. */
static inline bool joinable_launch(uint8_t st, uint8_t lap, uint16_t prog) {
    return st == RS_CRUISE && lap == 1 && prog < (uint16_t)LAP_JOIN_MAX;
}

/* race_acquire_remote — fill rs->remote for this frame: from bot_update()
 * in 1-player mode, from the wire (plus the remote_idle liveness counter)
 * in 2-player mode.  Returns whether rs->remote was refreshed this frame
 * (real packet or bot-synthesised one). */
static bool race_acquire_remote(RaceState *rs, World *w, uint8_t my_rs,
                                 uint16_t my_progress, bool player_won,
                                 int16_t *rx_gap)
{
    RemoteState rs_in;
    if (rs->bot_enabled) {
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
                   my_progress, w->lap, w->ps.cam_x, w->frame,
                   my_rs == RS_READY ||
                   joinable_launch(my_rs, w->lap, my_progress),
                   player_won);
        rs->remote = rs_in;
        rs->remote_live = true;
        return true;
    }

    bool got = serial_recv(&rs_in);
    *rx_gap = rs->remote_idle;   /* frames since the previous packet */
    if (got)                                          { rs->remote_idle = 0; rs->rx_count++; }
    else if (rs->remote_idle < REMOTE_TIMEOUT_FRAMES) rs->remote_idle++;
    rs->remote_live = rs->remote_idle < REMOTE_TIMEOUT_FRAMES;
    if (got) rs->remote = rs_in;
    return got;
}

/* race_track_link_health — the HUD link indicator, evaluated once per
 * LINK_WINDOW_FRAMES with hysteresis (LINK_BAD_PKTS..LINK_OK_PKTS is a dead
 * band), so transitions are rare enough that the HUD plane's no-partial-erase
 * cost is fine. */
static void race_track_link_health(RaceState *rs, bool got)
{
    if (got) rs->link_rx++;
    rs->link_changed = false;
    if (++rs->link_window >= LINK_WINDOW_FRAMES) {
        uint8_t next = rs->link_state;
        if (rs->remote_idle >= REMOTE_TIMEOUT_FRAMES) next = LINK_NONE;
        else if (rs->link_rx >= LINK_OK_PKTS)  next = LINK_OK;
        else if (rs->link_rx <= LINK_BAD_PKTS) next = LINK_BAD;
        else if (rs->link_state == LINK_NONE)  next = LINK_BAD;
        rs->link_changed = (next != rs->link_state);
        rs->link_state   = next;
        rs->link_window  = 0;
        rs->link_rx      = 0;
    }
}

/* race_dead_reckon — keep the peer moving between packets.
 *
 * A packet is produced on the PEER's frames, not ours, and two real STs
 * never render at the same rate — whichever one is drawing the heavier
 * scene misses its VBL and drops to 25 or 16 Hz while the other still
 * hits 50.  The receiver then gets a packet on one frame in two or three,
 * and remote.progress sits still on the others while our own my_progress
 * keeps advancing: peer_rel_z walks backwards, then snaps forward when
 * the next packet lands.  That snap is the pvp stutter — the link is fine,
 * the sample rate just does not match the frame rate.  Hatari hides it
 * completely, because both emulated machines run the identical scene at
 * the identical speed and a packet arrives every single frame.
 *
 * So measure the peer's own rate from consecutive samples (spread over
 * the frames that actually elapsed between them, which is what makes the
 * estimate independent of their frame rate) and advance the ghost at that
 * rate on packetless frames.  Everything downstream — ghost depth,
 * drafting, missile and mine hit tests — reads remote.progress, so they
 * all move smoothly together and a packet only ever applies a small
 * correction.
 *
 * rs->remote already equals this frame's incoming packet when got is true
 * (race_acquire_remote assigns it before returning), so this reads
 * rs->remote directly rather than taking the packet as a separate arg. */
static void race_dead_reckon(RaceState *rs, bool got, int16_t rx_gap)
{
    if (got) {
        uint16_t prev = rs->peer_prog_rx;
        rs->peer_prog_rx = rs->remote.progress;
        /* Only a forward step inside one lap measures a rate: a lap
         * boundary resets progress to 0, and a gap wider than a beacon
         * interval straddles gameplay we never saw. */
        if (rs->remote.state == RS_CRUISE && rs->remote.progress > prev &&
            rx_gap <= PEER_RATE_MAX_GAP) {
            int16_t v = (int16_t)((uint16_t)(rs->remote.progress - prev)
                                  / (uint16_t)(rx_gap + 1));
            rs->peer_speed = v > CAM_ZSPEED_MAX ? CAM_ZSPEED_MAX : v;
        } else {
            rs->peer_speed = 0;   /* re-measured on the next pair */
        }
    } else if (rs->peer_speed > 0 && rs->remote.state == RS_CRUISE &&
               rs->remote_idle < REMOTE_TIMEOUT_FRAMES) {
        /* Extrapolate only while the peer is still considered live, so a
         * real dropout freezes the ghost where it was last seen instead
         * of flying it down a course it may have already left. */
        rs->remote.progress =
            progress_clamp((uint16_t)(rs->remote.progress + rs->peer_speed));
    }
}

/* race_apply_peer_events — the RS_DEAD-edge alien clearing, the FINISHED
 * latch, a peer's incoming missile/mine spawn, and the KILL edge. */
static void race_apply_peer_events(RaceState *rs, GameState *state, World *w,
                                   uint8_t prev_remote_state, bool got)
{
    /* A serial peer that just crashed (RS_DEAD edge) clears the alien it hit
     * from our shared course too, the same as their missiles already do via
     * update_missiles — the wire carries no alien-kill event, so we infer it
     * from an alien sitting at their reported position (peer_rel_z tracks that
     * alien's z, both scrolling at our speed).  If they died to our missile in
     * open space no alien matches and nothing is cleared.  Bot crashes are
     * resolved locally in bot_update, so this is the real-peer path only. */
    if (unlikely(got && !rs->bot_enabled && rs->remote.state == RS_DEAD
            && prev_remote_state != RS_DEAD)) {
        int hi = alien_crash_index(&w->aliens, rs->peer_rel_z,
                                   rs->remote.cam_x, CAM_ZSPEED_MAX, PLAYER_HIT_TOL);
        if (hi >= 0) w->aliens.alive[hi] = false;
    }

    if (!got) return;

    /* Latch FINISHED only for a packet tagged with our current race
     * parity: stale in-flight packets from the peer's previous
     * (already-won-or-lost) race must not poison this race's verdict. */
    if (unlikely(rs->remote.finished && rs->remote.race_parity == w->race_parity))
        rs->peer_finished = true;
    if (unlikely(rs->remote.fire && rs->remote.state != RS_DEAD)) {
        int16_t muzzle_z = S16(rs->peer_rel_z + HLINE_ZMIN);
        int i = free_slot(rs->rmissiles.alive, MISSILE_COUNT);
        if (i >= 0) {
            rs->rmissiles.x[i]     = rs->remote.cam_x;
            rs->rmissiles.z[i]     = muzzle_z;
            /* HLINE_ZMIN, not muzzle_z: draw_remote_missile never
             * reads vis_z, but update_missiles advances it ×1.5 every
             * frame for every set it is given.  A peer behind us has
             * peer_rel_z down to -LAP_LENGTH, so muzzle_z
             * reaches -30699 and the ×1.5 left int16_t range on the
             * next frame.  Seeding it the way try_fire_missile does
             * makes "vis_z stays in [HLINE_ZMIN, GRID_ZFAR]" hold for
             * every MissileSet by construction. */
            rs->rmissiles.vis_z[i] = HLINE_ZMIN;
            rs->rmissiles.alive[i] = true;
        }
    }
    /* Incoming mine is a render-only event: our hit detection against it is
     * bot-only below, a real peer's hit arrives as their KILL bit instead.
     * Spawns at their current depth; peer_rel_z > 0 excludes someone
     * behind us (can never matter) and < LAP_LENGTH excludes
     * the clamp, where the true depth is unknown. */
    if (unlikely(rs->remote.mine && rs->peer_rel_z > 0 &&
        rs->peer_rel_z < LAP_LENGTH)) {
        int i = free_slot(w->mines.alive, MINE_COUNT);
        if (i >= 0) {
            w->mines.x[i]     = rs->remote.cam_x;
            w->mines.z[i]     = rs->peer_rel_z;
            w->mines.alive[i] = true;
        }
    }
    /* Their missile hit us (shooter-authoritative, edge-triggered). */
    if (unlikely(rs->remote.kill && !rs->kill_latched && *state == STATE_CRUISE))
        *state = STATE_CRASH;
    rs->kill_latched = rs->remote.kill;
}

/* race_resolve_hits — bot missiles/mines vs us, then our missiles/mines vs
 * the ghost. */
static void race_resolve_hits(RaceState *rs, GameState *state, World *w,
                              int16_t cam_zspeed)
{
    const PhysicsState *ps = &w->ps;
    bool bot_active = rs->bot_enabled;

    /* Bot shots at us are resolved locally (no wire to carry a KILL):
     * its missiles fly forward in our frame and cross us at z≈0. */
    if (bot_active && *state == STATE_CRUISE &&
        missiles_hit_ghost(cam_zspeed, &rs->rmissiles,
                           ps->cam_x, 1, PLAYER_HIT_TOL))
        *state = STATE_CRASH;

    /* Bot mines are resolved locally too, same reason — field_hit_player's
     * (0, -cam_zspeed] crossing window is exactly the alien-collision shape,
     * reused here via the shared helper. */
    if (bot_active && *state == STATE_CRUISE &&
        field_hit_player(w->mines.x, w->mines.z, w->mines.alive, MINE_COUNT,
                         ps->cam_x, cam_zspeed, PLAYER_HIT_TOL))
        *state = STATE_CRASH;

    /* Our missiles/mines vs the ghost: shooter detects the hit and tells the
     * peer via the KILL bit (the bot is killed directly).  The local
     * RS_DEAD override hides the ghost until its own state catches up. */
    if (rs->remote_live && rs->remote.state != RS_DEAD) {
        bool hit = missiles_hit_ghost(cam_zspeed, &w->missiles,
                           rs->remote.cam_x, rs->peer_rel_z,
                           ALIEN_AIM_TOL);
        hit |= mines_hit_ghost(&w->mymines, rs->remote.cam_x, rs->peer_rel_z);
        if (unlikely(hit)) {
            backend_snd_sfx(SND_ENMYHIT);
            if (bot_active) bot_kill(&rs->bot);
            else            rs->kill_pending = KILL_REPEAT;
            rs->remote.state = RS_DEAD;
        }
    }
}

/* race_send — beacon-vs-every-frame pacing and the outgoing packet. */
static void race_send(RaceState *rs, const World *w, uint8_t my_rs,
                      uint16_t my_progress, bool fired, bool dropped)
{
    /* Beacon until a peer is heard: sending costs 6 BIOS traps on Atari,
     * so an unpaired 2-player session only pays them every 16th frame
     * (1 player never sends at all).  Both sides
     * beacon, so pairing completes within ~0.3s; once paired, send every
     * frame for smooth ghosting (and fall back to beaconing if the peer
     * goes quiet past the ghost timeout). */
    if (rs->remote_idle < REMOTE_TIMEOUT_FRAMES || (w->frame & 15) == 0) {
        RemoteState out;
        out.state       = my_rs;
        out.fire        = fired;
        out.kill        = rs->kill_pending > 0;
        out.finished    = w->race_finished;
        out.mine        = dropped;
        out.race_parity = w->race_parity;
        out.lap         = w->lap;
        out.cam_x       = w->ps.cam_x;
        out.progress    = my_progress;
        serial_send(&out);
        if (rs->kill_pending > 0) rs->kill_pending--;
    }
}

/* race_place_ghost — ghost_show/ghost_z for rendering, and peer_gate_ok for
 * next frame's gate handshake. */
static void race_place_ghost(RaceState *rs, bool remote_player_flag)
{
    /* Ghost placement: race-relative depth.  Drawn only when strictly ahead
     * of us — only the chaser sees the leader, never the reverse — and no
     * further than GRID_ZFAR, so visibility matches missiles_hit_ghost()'s
     * (0, GRID_ZFAR] window exactly: a ghost on screen is always a hittable
     * target (beyond it, a ghost pinned at the far clip would look reachable
     * but be unkillable).  A peer less than REMOTE_Z_NEAR ahead is
     * still drawn pinned at REMOTE_Z_NEAR, which bounds the focal_rcp
     * divide in draw_remote_player(). */
    bool peer_on  = rs->remote_live && rs->remote.state != RS_DEAD;
    rs->ghost_show = peer_on && rs->peer_rel_z > 0 && rs->peer_rel_z <= GRID_ZFAR &&
                     remote_player_flag;
    rs->ghost_z    = CLAMP(rs->peer_rel_z, REMOTE_Z_NEAR, GRID_ZFAR);

    /* Gate-release handshake: consumed next frame by
     * state_gate via main's rs.peer_gate_ok.  The RS_CRUISE clause exists
     * ONLY to absorb the 1-2-frame skew when the peer released the gate
     * first and never re-sends READY; the progress qualifier is what makes
     * it safe — without it, a winner holding FIRE at the gate would see the
     * still-racing loser's RS_CRUISE and relaunch alone, desynchronizing
     * all future laps.  A freshly launched peer is seen within a packet or
     * two (progress <= a few hundred); a mid-lap peer is always far past
     * LAP_JOIN_MAX. */
    /* 2 players never launches without a live peer: the player waits at
     * the gate (or presses F1 to fall back to the bot).  test_solo is the
     * test-only escape for peerless runs. */
    rs->peer_gate_ok =
        (rs->remote_idle >= REMOTE_TIMEOUT_FRAMES && rs->test_solo) ||
        (rs->remote_live &&
         (rs->remote.state == RS_READY ||
          joinable_launch(rs->remote.state, rs->remote.lap, rs->remote.progress)));
}

/* race_update — the remote slot's per-frame update.  Plain static: at -Os,
 * letting gcc inline it measured +48 B for ~1k cycles/frame back (2026-10).
 *
 * Orchestrator only: reads the wire, updates the peer/ghost/link state, and
 * resolves hits, in the order the wire protocol requires (see the per-helper
 * comments above for what each phase does). */
static void race_update(RaceState *rs, GameState *state, bool remote_player_flag,
    World *w, bool fired, bool dropped, bool player_won)
{
    int16_t cam_zspeed = w->cam_zspeed;

    /* Per-lap race coordinate shared with the peer: how far down the course
     * we are.  0 while not racing so both players start each lap level;
     * capped at the course length.  The crash stun is still racing: the
     * bot, the peer's alien removal (RS_DEAD) and its ghost estimate all
     * read it.  finish_dist can run negative for a
     * frame right after crossing (before race_start/the mid-race branch
     * resets it), so 30720 - finish_dist can exceed INT16_MAX: the subtraction is done in
     * uint16 (defined wraparound, same sub.w) instead of signed 16-bit int,
     * which would be UB under -mshort.
     * Emergent behaviour: combined with a `lap` field that keeps its
     * race-end value while not racing, a peer sitting at the gate reports
     * (lap=LAPS_PER_RACE, progress=0) — rel_depth() then saturates against
     * whichever lap we're on, so the ghost hides and drafting is off, which
     * is what we want, but it is accidental rather than checked here. */
    uint16_t my_progress = likely(*state == STATE_CRUISE || *state == STATE_CRASH)
        ? progress_clamp(world_progress(w)) : 0;

    /* Peer missiles run through the same sim against the same deterministic
     * alien field, so both machines agree on alien kills. */
    update_missiles(cam_zspeed, &rs->rmissiles, &w->aliens, NULL);

    /* Outgoing wire state, computed once (also feeds the bot's own gate
     * handshake below).  gate_ready only latches after the dwell (see
     * state_gate), so it alone means "able to launch". */
    uint8_t my_rs = likely(*state == STATE_CRUISE) ? RS_CRUISE
                  : *state == STATE_CRASH     ? RS_DEAD
                  : *state == STATE_COUNTDOWN ? RS_READY
                  : w->gate_ready ? RS_READY : RS_WAIT;

    uint8_t prev_remote_state = rs->remote.state;
    int16_t rx_gap = 0;           /* written only on the 2-player path */
    bool got = race_acquire_remote(rs, w, my_rs, my_progress, player_won,
                                    &rx_gap);
    if (!rs->bot_enabled) {
        race_track_link_health(rs, got);
        race_dead_reckon(rs, got, rx_gap);
    }

    /* Recomputed every frame, not just on a packet: my_progress moves even
     * when the peer is silent.  Single source for the ghost, peer muzzle_z,
     * our missile-vs-ghost test, incoming mine placement, drafting, and the
     * HUD. */
    rs->peer_rel_z = rel_depth(rs->remote.lap, rs->remote.progress, w->lap, my_progress);

    race_apply_peer_events(rs, state, w, prev_remote_state, got);
    race_resolve_hits(rs, state, w, cam_zspeed);
    if (!rs->bot_enabled) race_send(rs, w, my_rs, my_progress, fired, dropped);
    race_place_ghost(rs, remote_player_flag);
}

/* apply_speed_modifiers — the three adjustments main() makes to cam_zspeed
 * after race_update, in the order that leaves the anti-cheat clamp the final
 * word.  All three are cruise-only, so the state test is hoisted here.
 *
 * Left for gcc to inline: forcing it out of line measured +24 bytes. */
static void apply_speed_modifiers(World *w, const RaceState *rs, GameState state)
{
    if (unlikely(state != STATE_CRUISE)) return;

    /* Drafting: a small speed bonus when chasing close behind the opponent
     * (≤ 1 world unit, ~0.16 s at base speed).  Incentivises tight racing
     * and rewards the chaser for staying in the leader's slipstream.
     * Capped at DRAFT_ZSPEED_CAP over this lap's ceiling (nothing else
     * bounds cam_zspeed on a player drafting without throttling), and
     * state_cruise's bleed-off pulls any excess back down every frame, so
     * the bonus is a temporary slipstream.  DRAFT_ZSPEED_STEP must exceed
     * THROTTLE_STEP (the bleed-off) or the two cancel and the cap is never
     * reached. */
    if (rs->ghost_show && rs->peer_rel_z > 0 && rs->peer_rel_z <= FP_ONE) {
        int16_t cap = S16(zspeed_max_for_lap(w->lap) + DRAFT_ZSPEED_CAP);
        w->cam_zspeed = S16(w->cam_zspeed + DRAFT_ZSPEED_STEP);
        if (w->cam_zspeed > cap) w->cam_zspeed = cap;
    }

    /* Catch-up: the mirror of drafting — a racer dropped well behind
     * (>= CATCHUP_REL_Z) gains speed toward the *leader's* lap ceiling plus
     * a boost, and state_cruise's bleed pulls the excess back once the gap
     * closes.  Gain must exceed the bleed or they cancel (same rule as
     * drafting above).
     *
     * The ceiling is the leader's lap, not ours: per-lap ceilings rise
     * (256/320/384), so against our own lap's ceiling a lapped player could
     * never close.  This is the one caller passing a wire-decoded lap; the
     * CAM_ZSPEED_MAX saturation in zspeed_max_for_lap bounds it, so the peak
     * is still CAM_ZSPEED_MAX + CATCHUP_ZSPEED_CAP (the OVER_CEILING asserts
     * hold).
     *
     * The boost tapers with the gap (CATCHUP_TAPER_SHIFT) rather than
     * sitting at CATCHUP_ZSPEED_CAP: excess speed only bleeds off by
     * THROTTLE_STEP/frame, far slower than it closes the gap, so a flat cap
     * carries the chaser through the leader.  A cap that shrinks with the
     * gap makes the same clamp pull speed back, and the chaser arrives in
     * the shootable band at the leader's pace. */
    if (rs->remote_live && rs->peer_rel_z >= CATCHUP_REL_Z) {
        uint8_t lead_lap = rs->remote.lap > w->lap ? rs->remote.lap : w->lap;
        int16_t cap = S16(zspeed_max_for_lap(lead_lap) + catchup_boost(rs->peer_rel_z));
        w->cam_zspeed = S16(w->cam_zspeed + CATCHUP_ZSPEED_STEP);
        if (w->cam_zspeed > cap) w->cam_zspeed = cap;
    }

    /* Anti-cheat: aliens/mines never spawn past GRID_XHALF (the visible
     * ground grid's edge), so parking outside it would dodge every
     * hazard forever.  A hard clamp, not a drain: falling behind while
     * off-grid arms the catch-up boost above, which out-gains any
     * per-frame drain.  Placed last, after every other cam_zspeed adjustment
     * this frame, so it is unconditionally the final word: no combination
     * of drafting/catch-up/throttle can leave the ship faster than
     * CAM_ZSPEED_MIN while off-grid. */
    if (w->ps.cam_x > GRID_XHALF || w->ps.cam_x < -GRID_XHALF)
        w->cam_zspeed = CAM_ZSPEED_MIN;
}
