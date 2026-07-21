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

# check_tx <file> — sent stream is non-empty, 7-byte framed, 0xAA-synced,
# and no payload byte has the high bit set (the framing invariant).
check_tx() {
    size=$(stat -c%s "$1")
    [ "$size" -gt 0 ]            || die "$1: nothing transmitted"
    [ $((size % 7)) -eq 0 ]      || die "$1: size $size not a multiple of 7"
    [ "$(od -An -tu1 -N1 "$1" | tr -d ' ')" = 170 ] \
                                 || die "$1: first byte is not 0xAA"
    od -An -tu1 -v "$1" | tr ' ' '\n' | grep -v '^$' \
        | awk 'NR % 7 == 1 { if ($1 != 170) exit 1; next }
               $1 >= 128   { exit 1 }' \
                                 || die "$1: framing invariant broken (stray sync or 8-bit payload)"
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
    # Control run must NEVER render remote-player lines.
    grep '^RLINES ' "$1" | grep -qv '^RLINES 0$' \
        && die "$1: control run drew remote-player lines without a peer"
    # Test run must render the ghost triangle (RLINES 3) at least once.
    grep -q '^RLINES 3$' "$2"    || die "$2: no frame with the 3 remote-triangle RLINEs"
    # Both runs must have drawn alien-plane lines (the game rendered gameplay).
    grep -q '^ALINES 1$\|^ALINES [2-9]' "$1" \
        || die "$1: control run drew no alien-plane lines (never reached gameplay?)"
    grep -q '^ALINES 1$\|^ALINES [2-9]' "$2" \
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
# Byte [6] = \000: lap=1, no mine.
printf '\252\001\110\000\002\167\000' > "$tmp/peer"

: > "$tmp/tx_test"
./vq-ascii 0 $MAX_FRAME "$tmp/tx_test" "$tmp/peer" nobot > "$tmp/test.log" || die "vq-ascii test run failed"
check_tx "$tmp/tx_test"
check_logs "$tmp/ctl.log" "$tmp/test.log"
echo "PASS: linux ascii (posix serial + remote player rendered)"

# ── Part 1b: computer opponent ────────────────────────────────────────────────
# Without "nobot" and without a peer, the bot must fill the remote slot: the
# ghost triangle (RLINES 3) and at least one bot missile tick (RLINES 1).
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
grep -q '^RLINES 3$' "$tmp/bot.log" || die "bot run: ghost triangle never rendered"
grep -q '^RLINES 1$' "$tmp/bot.log" || die "bot run: bot never fired a visible missile"
echo "PASS: linux ascii (computer opponent renders and fires)"

# ── Part 1c: PvP kill path ────────────────────────────────────────────────────
# A crafted cruise-state peer sits 800 units ahead in the autopilot's lane
# (cam_x=512=CAM_X_INIT, progress=800 → wire 800>>2=200). Byte [6] = \000:
# lap=1, no mine.  The autopilot fires continuously, so a missile must hit
# the ghost and the next KILL_REPEAT=8 transmitted packets must carry the
# KILL bit (byte 1, bit 3).
printf '\252\001\104\000\001\110\000' > "$tmp/peer_ahead"
: > "$tmp/tx_kill"
./vq-ascii 0 200 "$tmp/tx_kill" "$tmp/peer_ahead" nobot > "$tmp/kill.log" \
    || die "vq-ascii kill run failed"
nkill=$(od -An -tu1 -v "$tmp/tx_kill" | tr ' ' '\n' | grep -v '^$' \
        | awk 'NR%7==2 && int($1/8)%2==1 {n++} END {print n+0}')
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
# Detection: the between-race GATE screen renders credits (~300 world-plane
# lines) vs plain cruise's ~19 grid lines, so a LINES jump to >=100 marks a
# GATE episode; the very first episode (frame 0) is just the pre-race "PRESS
# FIRE" screen.  Races are LAPS_PER_RACE=5 laps and GATE is only re-entered
# once a full race finishes (mid-race lap crossings stay in CRUISE), so a
# SECOND episode means a complete race.  A normal (in-grid) nobot autopilot
# finishes its first race by ~frame 1204 (measured); the off-grid autopilot,
# floored at CAM_ZSPEED_MIN the entire time it's off-grid, must not finish
# within the same budget, or the clamp isn't costing enough race time to
# matter.
count_gate_episodes() { # $1=log $2=frame ceiling -> GATE episodes at/before it
    awk -v ceil="$2" '
        /^FRAME /{f=$2}
        /^LINES /{
            cur = ($2 >= 100) ? 1 : 0
            if (cur==1 && prev==0 && f<=ceil) n++
            prev=cur
        }
        END { print n+0 }
    ' "$1"
}

OFFGRID_MAX_FRAME=1600
./vq-ascii 0 $OFFGRID_MAX_FRAME /dev/null /dev/null nobot > "$tmp/offgrid_ctl.log" \
    || die "vq-ascii off-grid control run failed"
VQ_AUTOPILOT_OFFGRID=1 ./vq-ascii 0 $OFFGRID_MAX_FRAME /dev/null /dev/null nobot \
    > "$tmp/offgrid_test.log" || die "vq-ascii off-grid test run failed"

ctl_episodes=$(count_gate_episodes "$tmp/offgrid_ctl.log" $OFFGRID_MAX_FRAME)
test_episodes=$(count_gate_episodes "$tmp/offgrid_test.log" $OFFGRID_MAX_FRAME)
[ "$ctl_episodes" -ge 2 ] \
    || die "off-grid control run: expected a completed race (>=2 GATE episodes) within $OFFGRID_MAX_FRAME frames, got $ctl_episodes"
[ "$test_episodes" -lt 2 ] \
    || die "off-grid run: race completed within $OFFGRID_MAX_FRAME frames despite the anti-cheat clamp (got $test_episodes GATE episodes)"
echo "PASS: linux ascii (off-grid anti-cheat clamp measurably slows a full race)"

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
# to ~28 KiB (≈30 s at 9600 baud), spanning boot + intro + the test window.
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
