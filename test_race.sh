#!/bin/sh
# Race-mode regression test on the text backend.
#
# Both serial transports accept regular files, so no FIFOs and no two-process
# synchronization are needed: posix_serial.c open()s its argv paths, and
# hatari feeds --rs232-in file bytes to the emulated MFP and captures TX in
# --rs232-out.  Each part does two autopiloted runs that differ only in
# whether a peer packet arrives, so their frame logs must differ only in the
# alien-plane lines (ALINES/ALINE) of the remote-player triangle.
#
# Part 1: vq-ascii      — game logic + POSIX serial transport.
# Part 2: vq-ascii.tos  — TOS serial path end-to-end under hatari.
set -u

MAX_FRAME=400

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

die() { echo "FAIL: $*" >&2; exit 1; }

# Segment count of the logo's ADN caption (drawn into the plane-0 yellow
# slice on the gate screen) — read from the generated tables so a TEXT change
# in gen_dna_helix.py doesn't silently invalidate check_logs' control assert.
CAPTION_RLINES=$(awk '/^#define MODEL_CAPTION_EDGES / { print $3 }' gen_tables.h)
case $CAPTION_RLINES in
    (*[!0-9]*|'') die "cannot read MODEL_CAPTION_EDGES from gen_tables.h";;
esac

# check_tx <file> — sent stream is non-empty, 6-byte framed, and every 6th
# byte (and only every 6th byte) has the marker bit (bit 7) set.
check_tx() {
    size=$(stat -c%s "$1")
    [ "$size" -gt 0 ]            || die "$1: nothing transmitted"
    [ $((size % 6)) -eq 0 ]      || die "$1: size $size not a multiple of 6"
    # first byte must be a marker (bit 7 set)
    od -An -tu1 -v "$1" | tr ' ' '\n' | grep -v '^$' \
        | awk 'NR % 6 == 1 { if ($1 < 128) exit 1; next }
               $1 >= 128   { exit 1 }' \
                                 || die "$1: framing invariant broken"
}

# has_triangle <log> — some frame drew an apex-up triangle (ghost or mine) in
# the opponent-coloured slice: its base is a horizontal RLINE below the
# horizon (y=100) and above the trailing opponent marker (OPP_MARK_BOT_Y=190).
# A line count alone no longer identifies the ghost: the marker is hidden
# while the ghost is on screen, and its own horizontal strokes sit at
# y 88-95 or 190-197.
has_triangle() {
    awk '/^RLINE / { split($2,a,","); split($3,b,",");
                     if (a[2]==b[2] && a[1]!=b[1] && a[2]>100 && a[2]<185) { found=1; exit } }
         END { exit !found }' "$1"
}

# check_logs <control.log> <test.log> — verify both runs have valid structure
# and that the test run renders a remote-player ghost that the control run does
# not.  Frame-by-frame equality is NOT required: gameplay changes (e.g. drafting)
# may shift timing and coordinates between the two runs.
check_logs() {
    cmp -s "$1" "$2" && die "$2: no difference vs control — remote player never rendered"
    # Verify both logs are well-formed: start with FRAME, end with DONE.
    for f in "$1" "$2"; do
        grep -q '^FRAME ' "$f"  || die "$f: no FRAME records"
        grep -q '^DONE '  "$f"  || die "$f: no DONE record (did not complete)"
    done
    # Control run must NEVER render remote-player lines.  RLINES equal to the
    # caption count is exempt: the logo's ADN caption rides the same plane-0
    # yellow slice on the gate screen, with or without a peer — anything else
    # is a real ghost/mine/missile leak.
    grep '^RLINES ' "$1" | grep -qEv "^RLINES (0|$CAPTION_RLINES)\$" \
        && die "$1: control run drew remote-player lines without a peer"
    # Test run must render the ghost triangle at least once.
    has_triangle "$2" || die "$2: remote-player ghost triangle never rendered"
    # Both runs must have drawn alien-plane lines (the game rendered gameplay).
    grep -q '^ALINES [1-9][0-9]*$' "$1" \
        || die "$1: control run drew no alien-plane lines (never reached gameplay?)"
    grep -q '^ALINES [1-9][0-9]*$' "$2" \
        || die "$2: test run drew no alien-plane lines (never reached gameplay?)"
}

# ── Part 1: Linux ascii ────────────────────────────────────────────────────────
# Control/test runs disable the computer opponent ("nobot") so the only remote
# ghost can come from the injected peer packet.
: > "$tmp/tx_ctl"
./vq-ascii 0 $MAX_FRAME "$tmp/tx_ctl" /dev/null nobot > "$tmp/ctl.log" || die "vq-ascii control run failed"
check_tx "$tmp/tx_ctl"

# Crafted peer packet — progress=1500 (between drafting threshold FP_ONE=1024
# and gate-handshake LAP_JOIN_MAX=2*FP_ONE=2048) so the ghost renders but
# drafting does not alter the simulation speed vs the control run.
# state=RS_CRUISE cam_x=1024 progress=1500 lap=1, no mine.
printf '\201\110\000\000\135\146' > "$tmp/peer"

: > "$tmp/tx_test"
./vq-ascii 0 $MAX_FRAME "$tmp/tx_test" "$tmp/peer" nobot > "$tmp/test.log" || die "vq-ascii test run failed"
check_tx "$tmp/tx_test"
check_logs "$tmp/ctl.log" "$tmp/test.log"
echo "PASS: linux ascii (posix serial + remote player rendered)"

# ── Part 1b: computer opponent ────────────────────────────────────────────────
# Without "nobot" and without a peer, the bot must fill the remote slot: the
# ghost triangle and at least one bot missile tick. Since the opponent
# marker recolor (render: colour the opponent HUD gauge like the opponent),
# the marker rides the same coloured tail slice as the ghost/missile, so
# exact RLINES totals (the old "RLINES 3" / "RLINES 1" checks) no longer
# isolate either one, so both are identified by shape (has_triangle above for
# the ghost).  draw_remote_missile draws a single vertical line (x0==x1)
# straddling eye level (SCREEN_HEIGHT_HALF=100 +/- hh, hh >= 2) — distinct
# from the ghost triangle (no edge is ever vertical) and from the marker's
# chevron/digit glyphs (y 88-95 at OPP_MARK_TOP_Y, 190-197 at OPP_MARK_BOT_Y;
# neither crosses y=100).
# Longer window than MAX_FRAME: the bot is active from frame 0 (remote_idle
# starts timed out) but waits BOT_WAIT_FRAMES=50 before the ready handshake
# launches both players together; the ghost then needs time to open a gap
# (avg somewhat above our constant 128) before it's within GRID_ZFAR — first
# visible around FRAME 66 (measured; unchanged from the pre-speed-bump value
# — CAM_ZSPEED_BASE/MAX/etc doubled together, so the gap-opening rate and
# the autopilot's own pace scaled by the same factor.  The player's own
# catch-up boost, once the bot's lead passes CATCHUP_REL_Z, narrows the gap
# again but not before the ghost has come into view once).  Races are
# LAPS_PER_RACE=5 laps long (race redesign plan), so the bot no longer needs
# several gate cycles to get a shot off — it fires well within the very
# first race, around FRAME 898 (measured; deterministic — the ascii
# autopilot only holds FIRE and never presses Up, and its own cam_zspeed
# only ever moves via drafting/catch-up, never plain throttling).
# Linux-only segment, so the extra frames are cheap.
BOT_MAX_FRAME=4500
./vq-ascii 0 $BOT_MAX_FRAME /dev/null /dev/null > "$tmp/bot.log" || die "vq-ascii bot run failed"
has_triangle "$tmp/bot.log" || die "bot run: ghost triangle never rendered"
awk '/^RLINE / { split($2,a,","); split($3,b,",");
                 if (a[1]==b[1] && (a[2]-100)*(b[2]-100) < 0) { found=1; exit } }
     END { exit !found }' "$tmp/bot.log" \
    || die "bot run: bot never fired a visible missile"
echo "PASS: linux ascii (computer opponent renders and fires)"

# ── Part 1c: PvP kill path ────────────────────────────────────────────────────
# A crafted cruise-state peer sits 800 units ahead in the autopilot's lane
# (cam_x=512=CAM_X_INIT, progress=800 → wire 800>>2=200), lap=1, no mine.
# The autopilot fires continuously, so a missile must hit the ghost and the
# next KILL_REPEAT=8 transmitted packets must carry the KILL bit (byte 0,
# bit 3).
#
# A single-shot regular-file peer (as in Part 1/1b above) no longer works
# here: posix_serial's serial_recv() drains the whole file on frame 0 (see
# Part 1d's comment), so remote_idle climbs unanswered afterward and exceeds
# REMOTE_TIMEOUT_FRAMES=50 well before the pre-race 3/2/1/GO countdown
# (COUNTDOWN_FRAMES=160, state_gate -> STATE_COUNTDOWN -> state_countdown)
# even finishes — the ghost is gone (peer "timed out") by the time
# STATE_CRUISE (and try_fire_missile) ever starts, so no hit is possible.
# A real peer sends every frame once paired, so this is a test-harness
# artifact of the static-file technique, not a product bug (same conclusion
# as Part 1d). Fixed the same way Part 1f feeds a live link: a FIFO paced by
# a background writer (VQ_FRAME_MS-throttled on both sides so vq-ascii can't
# race ahead of it and let remote_idle climb between writes).
KILL_MAX_FRAME=400
KILL_FRAME_MS=5
printf '\201\104\000\000\062\027' > "$tmp/peer_ahead"
: > "$tmp/tx_kill"
KILL_PIPE="$tmp/kill_pipe"
mkfifo "$KILL_PIPE"
( while true; do cat "$tmp/peer_ahead"; sleep 0.005; done > "$KILL_PIPE" ) &
kill_feeder=$!
VQ_FRAME_MS=$KILL_FRAME_MS ./vq-ascii 0 $KILL_MAX_FRAME "$tmp/tx_kill" "$KILL_PIPE" nobot \
    > "$tmp/kill.log"
kill_status=$?
kill "$kill_feeder" 2>/dev/null
wait "$kill_feeder" 2>/dev/null
[ "$kill_status" -eq 0 ] || die "vq-ascii kill run failed"
nkill=$(od -An -tu1 -v "$tmp/tx_kill" | tr ' ' '\n' | grep -v '^$' \
        | awk 'NR%6==1 && int($1/8)%2==1 {n++} END {print n+0}')
[ "$nkill" -eq 8 ] || die "kill run: expected 8 KILL packets, got $nkill"
echo "PASS: linux ascii (missile kills the remote player, KILL bit broadcast)"

# ── Part 1d: mines ────────────────────────────────────────────────────────────
# A crafted single-packet peer (as in Part 1c) with the MINE bit set does NOT
# work as an automated render check: race_update's incoming-mine spawn is a
# one-shot event gated on the SAME frame the packet is decoded, which for a
# static regular file is always frame 0 — while the game is still at the
# initial STATE_GATE (aliens/mines not drawn there, kStateFlags) and well
# before that race's own race_start().  race_start() unconditionally clears
# w->mines (by design — a new race must not carry over the previous one's
# hazards), so the mine is wiped before CRUISE ever begins and no ALINES
# delta is observable.  Verified empirically, not assumed: a debug print at
# the spawn site showed frame=0, state=STATE_GATE for this exact packet.
# A real peer sends every frame once paired (see race_update's beacon
# comment), so this is purely a single-shot-file test-harness artifact, not
# a product bug.
#
# The bot's own mine-drop uses the identical spawn code (race_update treats
# bot and wire peer identically) but decides to drop well after its own
# race_start, during an active RS_CRUISE — so Part 1b's bot run already
# exercises spawn + render + scroll + despawn, and — since the bot's mine
# lands at the BOT's own cam_x while our fixed-lane autopilot never steers —
# occasionally the field_hit_player collision path too, all under ASan/UBSan
# via `make test`'s 20000-frame soak.  What's left uncovered headlessly:
# the *outgoing* path (backend_ascii.c's autopilot never presses KEY_DOWN)
# and any visual/colour confirmation.  Both are manual `./vq-sdl` checks
# (see the plan's Verification section for Commit 6).

# ── Part 1e: off-grid anti-cheat ──────────────────────────────────────────────
# Aliens/mines never spawn past GRID_XHALF (render.c), so parking outside it
# would dodge every hazard forever; vquest.c's main loop hard-clamps
# cam_zspeed to CAM_ZSPEED_MIN whenever off-grid, applied last (after
# drafting/catch-up/throttle) so nothing that frame can leave it faster.
# Linux-only: exercised via backend_ascii.c's VQ_AUTOPILOT_OFFGRID=1 hook
# (holds RIGHT+UP in addition to FIRE), which getenv()s an env var hatari's
# TOS runs have no way to receive.
#
# This used to be inferred indirectly, by racing an in-grid "control"
# autopilot against the off-grid one and comparing which finished a full
# race first. That broke once aliens started drifting toward whichever
# racer is nearest (physics.c's update_alien_drift): a scripted autopilot
# can't react to individual aliens the way a human or the bot can, so a
# literally static in-grid lane became a guaranteed target, at which point
# its race-completion time says more about how punishing homing aliens are
# to a non-reactive input than about the off-grid clamp being tested. Rather
# than teach the fake player to dodge (a scripted, deterministic dodge is
# still not a reactive one, and measured no better — see the branch history)
# check the clamp directly instead: the off-grid run's own cam_zspeed once
# it is actually off-grid, not a race-completion time built on top of it.
# VQ_DEBUG_OVERLAY=1 latches the debug overlay on (backend_ascii.c), so
# every frame carries a "DBG Z <cam_zspeed>" / "DBG C <cam_x>" pair.
CAM_ZSPEED_MIN=$(awk '/^#define CAM_ZSPEED_MIN /{print $3}' tuning.h)
case $CAM_ZSPEED_MIN in
    (*[!0-9]*|'') die "cannot read CAM_ZSPEED_MIN from tuning.h";;
esac
# GRID_XHALF is "((int16_t)(N * FP_ONE))" in render.c; combine N with FP_ONE
# (1 << FP_SHIFT, vquest.h) rather than hardcode the product.
GRID_XHALF_N=$(grep -oE '#define GRID_XHALF *\(\(int16_t\)\([0-9]+ \* FP_ONE\)\)' render.c \
    | grep -oE '\([0-9]+ \*' | grep -oE '[0-9]+')
FP_SHIFT=$(awk '/^#define FP_SHIFT/{print $3}' vquest.h)
case $GRID_XHALF_N$FP_SHIFT in
    (*[!0-9]*|'') die "cannot read GRID_XHALF/FP_SHIFT from render.c/vquest.h";;
esac
GRID_XHALF=$((GRID_XHALF_N * (1 << FP_SHIFT)))

# check_offgrid_clamp <log> -> dies unless cam_zspeed reads exactly
# CAM_ZSPEED_MIN on every sampled frame from OFFGRID_SETTLE_FRAME on (comfortably
# after the measured ~frame 222 crossing of GRID_XHALF, so this isn't racing
# the transition) through the end of the run.
OFFGRID_SETTLE_FRAME=400
check_offgrid_clamp() {
    awk -v settle="$OFFGRID_SETTLE_FRAME" -v want="$CAM_ZSPEED_MIN" '
        /^FRAME /{f=$2}
        /^DBG Z /{z=$3}
        /^DBG C /{
            if (f >= settle) {
                n++
                if (z != want) { print "frame " f ": Z=" z " (want " want ")"; exit 1 }
            }
        }
        END { if (n == 0) { print "no DBG samples at/after frame " settle; exit 1 } }
    ' "$1"
}

# check_stays_ingrid <log> -> dies if |cam_x| ever exceeds GRID_XHALF. Sanity
# check for the control run: proves check_offgrid_clamp's pass on the test
# run is actually keyed off crossing GRID_XHALF, not something a normal
# (non-cheating) run would also trip — e.g. a non-reactive autopilot's
# cam_zspeed can independently settle at CAM_ZSPEED_MIN just from repeated
# alien-crash penalties, which would make a cam_zspeed-only sanity check
# pass vacuously regardless of cam_x.
check_stays_ingrid() {
    awk -v half="$GRID_XHALF" '
        /^FRAME /{f=$2}
        /^DBG C /{
            c = $3; if (c < 0) c = -c
            if (c > half) { print "frame " f ": |cam_x|=" c " exceeds GRID_XHALF=" half; exit 1 }
        }
    ' "$1"
}

OFFGRID_MAX_FRAME=600
VQ_DEBUG_OVERLAY=1 VQ_AUTOPILOT_OFFGRID=1 ./vq-ascii 0 $OFFGRID_MAX_FRAME /dev/null /dev/null nobot \
    > "$tmp/offgrid_test.log" || die "vq-ascii off-grid test run failed"
VQ_DEBUG_OVERLAY=1 ./vq-ascii 0 $OFFGRID_MAX_FRAME /dev/null /dev/null nobot \
    > "$tmp/offgrid_ctl.log" || die "vq-ascii off-grid control run failed"

offgrid_err=$(check_offgrid_clamp "$tmp/offgrid_test.log") \
    || die "off-grid run: cam_zspeed didn't stay pinned at CAM_ZSPEED_MIN ($CAM_ZSPEED_MIN) once off-grid — $offgrid_err"
ctl_err=$(check_stays_ingrid "$tmp/offgrid_ctl.log") \
    || die "in-grid control run: $ctl_err — off-grid detection isn't distinguishing in-grid from off-grid"
echo "PASS: linux ascii (off-grid anti-cheat clamp pins cam_zspeed to CAM_ZSPEED_MIN)"

# ── Part 1f: link-health indicator ────────────────────────────────────────────
# The static regular-file peers above (Parts 1/1c) don't exercise this: a
# regular file has no pacing, so serial_recv() drains the whole thing in one
# call on frame 0 — the link-health window sees one giant burst, not a
# realistic per-frame delivery rate. Pair two live instances over FIFOs
# instead, paced with VQ_FRAME_MS like the Makefile's race-ascii-a/b dev
# targets, so packets actually land one per real frame.
LINK_A2B="$tmp/link_a2b"
LINK_B2A="$tmp/link_b2a"
mkfifo "$LINK_A2B" "$LINK_B2A"
# > 2*LINK_WINDOW_FRAMES(64): window 1 covers the pairing beacon (mixed rate,
# reads as LINK_BAD); window 2 is a steady full-rate link (reads as LINK_OK).
LINK_FRAMES=160
VQ_FRAME_MS=20 ./vq-ascii 0 $LINK_FRAMES "$LINK_A2B" "$LINK_B2A" nobot > "$tmp/link_a.log" &
pid_link_a=$!
VQ_FRAME_MS=20 ./vq-ascii 0 $LINK_FRAMES "$LINK_B2A" "$LINK_A2B" nobot > "$tmp/link_b.log" &
pid_link_b=$!
wait $pid_link_a || die "linked ascii run (side A) failed"
wait $pid_link_b || die "linked ascii run (side B) failed"
grep -q '^LINK ok$' "$tmp/link_a.log" || die "linked ascii run: link health never reported ok"
echo "PASS: linux ascii (link-health indicator reports ok on a live paired link)"

# The bot run (Part 1b) never prints a LINK line at all: link_state starts at
# LINK_NONE (race_init's memset) and bot_active holds it there for the whole
# run, so link_changed never fires and hud_draw_link() is never called — the
# indicator box is never touched, which is the intended "hidden" behaviour
# for solo/bot play (see race_update's bot_active gate in physics.c).
grep -q '^LINK ' "$tmp/bot.log" \
    && die "bot run: link indicator drew something despite no serial peer"
echo "PASS: linux ascii (link-health indicator stays hidden in a bot-only race)"

# ── Part 2: Atari ascii under hatari ───────────────────────────────────────────
# Console output through the emulated VT52 is the bottleneck (~1 KB/s), so
# render only a short cruise window via min_frame/max_frame — serial runs
# every frame regardless.  The two runs are independent (the peer packet is
# Part 1's, valid here because game logic is platform-identical), so they run
# in parallel.
if ! command -v hatari-prg-args >/dev/null 2>&1; then
    echo "SKIP: hatari-prg-args not found — TOS serial path not tested" >&2
    exit 0
fi

# Cruise starts almost immediately (one gate release, no more takeoff delay):
# the crafted peer sits 800 units ahead, visible from the first cruise frame
# until our own progress passes 800 (~6 frames at base speed 128).  TOS_MIN=2
# skips just the single GATE-state frame (game frame 1): the gate's spinning
# logo is redrawn every frame it's shown (unlike the old static wait screen,
# which only drew once), so printing it would push ~300 extra lines through
# the ~1 KB/s console for no benefit to this test — the ghost window itself
# starts right after.
TOS_MIN=2
TOS_MAX=30

run_tos() { # $1=rs232-in $2=rs232-out $3=log
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    hatari-prg-args -q --conout 2 --fast-boot true --fast-forward on \
        --sound off --disable-video on \
        --rs232-in "$1" --rs232-out "$2" -- ./vq-ascii.tos $TOS_MIN $TOS_MAX - - nobot 2>&1 \
        | tr -d '\r\000' \
        | grep -aE '^(FRAME|ANGLES|LINES|BBOX|LINE|ALINES|ALINE|RLINES|RLINE|END_FRAME|DONE)' > "$3"
    grep -q '^DONE ' "$3" || die "$3: TOS run did not complete (no DONE line)"
}

# hatari delivers --rs232-in bytes at the emulated baud rate starting at
# boot, so a single packet is consumed before serial_init runs.  Repeat it
# to ~24 KiB (≈26 s at 9600 baud), spanning boot + intro + the test window.
# If the TOS run starts failing to see the peer, add a 13th doubling (48 KiB,
# ≈51 s) rather than hunting elsewhere.
cp "$tmp/peer" "$tmp/peer_tos"
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    cat "$tmp/peer_tos" "$tmp/peer_tos" > "$tmp/peer_dbl" && mv "$tmp/peer_dbl" "$tmp/peer_tos"
done

run_tos /dev/null        "$tmp/tx_ctl_tos"  "$tmp/ctl_tos.log"  & pid_ctl=$!
run_tos "$tmp/peer_tos"  "$tmp/tx_test_tos" "$tmp/test_tos.log" & pid_test=$!
wait $pid_ctl  || die "TOS control run failed"
wait $pid_test || die "TOS test run failed"

check_tx "$tmp/tx_ctl_tos"
check_tx "$tmp/tx_test_tos"
check_logs "$tmp/ctl_tos.log" "$tmp/test_tos.log"
echo "PASS: atari ascii (TOS serial via hatari + remote player rendered)"
