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

# check_logs <control.log> <test.log> — the only difference the peer packet
# may cause is the remote triangle: ALINES/ALINE (plane 1) plus its RLINES/
# RLINE plane-0 copy.
check_logs() {
    cmp -s "$1" "$2" && die "$2: no difference vs control — remote player never rendered"
    if diff "$1" "$2" | grep '^[<>]' | grep -Eqv '^[<>] (ALINES?|RLINES?) '; then
        diff "$1" "$2" | grep '^[<>]' | grep -Ev '^[<>] (ALINES?|RLINES?) ' | head -5 >&2
        die "$2: differs from control beyond ALINE(S)/RLINE(S) (see above)"
    fi
    grep '^ALINES ' "$1" | awk '{print $2}' > "$tmp/n_ctl"
    grep '^ALINES ' "$2" | awk '{print $2}' > "$tmp/n_test"
    [ -s "$tmp/n_ctl" ]          || die "$1: no ALINES records — game never rendered"
    paste "$tmp/n_ctl" "$tmp/n_test" | awk '$2 == $1 + 3 {found=1} END {exit !found}' \
        || die "$2: no frame gained exactly the 3 remote-triangle ALINEs"
    awk '$1 > 0 {found=1} END {exit !found}' "$tmp/n_ctl" \
        || die "$1: control run drew no alien-plane lines at all (never reached gameplay?)"
    grep -q '^RLINES 3$' "$2"    || die "$2: no frame with the 3 remote-triangle RLINEs"
    grep '^RLINES ' "$1" | grep -qv '^RLINES 0$' \
        && die "$1: control run drew remote-player lines without a peer"
}

# ── Part 1: Linux ascii ────────────────────────────────────────────────────────
# Control/test runs disable the computer opponent ("nobot") so the only remote
# ghost can come from the injected peer packet.
: > "$tmp/tx_ctl"
./vq-ascii 0 $MAX_FRAME "$tmp/tx_ctl" /dev/null nobot > "$tmp/ctl.log" || die "vq-ascii control run failed"
check_tx "$tmp/tx_ctl"

# Crafted peer packet — a ghost level with us or behind is not drawn (only
# the chaser sees the leader), so the takeoff-state packet the control run
# sends (progress 0 = level) would never render.  Cruise-state, no fire/kill
# bits, cam_x=512=CAM_X_INIT (the autopilot's lane, so the triangle lands at
# screen center), progress=800 (wire 800>>2=200) placing it ahead of us, and
# alt=0 so it sits at eye level while the autopilot is still low in takeoff.
printf '\252\001\104\000\001\110\000' > "$tmp/peer"

: > "$tmp/tx_test"
./vq-ascii 0 $MAX_FRAME "$tmp/tx_test" "$tmp/peer" nobot > "$tmp/test.log" || die "vq-ascii test run failed"
check_tx "$tmp/tx_test"
check_logs "$tmp/ctl.log" "$tmp/test.log"
echo "PASS: linux ascii (posix serial + remote player rendered)"

# ── Part 1b: computer opponent ────────────────────────────────────────────────
# Without "nobot" and without a peer, the bot must fill the remote slot: the
# ghost triangle (RLINES 3) and at least one bot missile tick (RLINES 1).
# Longer window than MAX_FRAME: with the fixed alien spread (LCG mask, commit
# after 710a141) the deterministic autopilot's first visible bot shot lands
# around frame 1490.  Linux-only segment, so the extra frames are cheap.
BOT_MAX_FRAME=2000
./vq-ascii 0 $BOT_MAX_FRAME /dev/null /dev/null > "$tmp/bot.log" || die "vq-ascii bot run failed"
grep -q '^RLINES 3$' "$tmp/bot.log" || die "bot run: ghost triangle never rendered"
grep -q '^RLINES 1$' "$tmp/bot.log" || die "bot run: bot never fired a visible missile"
echo "PASS: linux ascii (computer opponent renders and fires)"

# ── Part 1c: PvP kill path ────────────────────────────────────────────────────
# A crafted cruise-state peer sits 800 units ahead in the autopilot's lane
# (cam_x=512=CAM_X_INIT, progress=800 → wire 800>>2=200, alt=2048=CRUISE_ALT).
# The autopilot fires continuously, so a missile must hit the ghost and the
# next KILL_REPEAT=8 transmitted packets must carry the KILL bit (byte 1, bit 3).
printf '\252\001\104\000\001\110\100' > "$tmp/peer_ahead"
: > "$tmp/tx_kill"
./vq-ascii 0 200 "$tmp/tx_kill" "$tmp/peer_ahead" nobot > "$tmp/kill.log" \
    || die "vq-ascii kill run failed"
nkill=$(od -An -tu1 -v "$tmp/tx_kill" | tr ' ' '\n' | grep -v '^$' \
        | awk 'NR%7==2 && int($1/8)%2==1 {n++} END {print n+0}')
[ "$nkill" -eq 8 ] || die "kill run: expected 8 KILL packets, got $nkill"
echo "PASS: linux ascii (missile kills the remote player, KILL bit broadcast)"

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

# The ghost is visible during early takeoff: the crafted peer sits 800 units
# ahead at ground altitude, until the autopilot's climb pushes the triangle
# below the screen (~game frame 20).  Once in cruise our progress passes 800
# and the ghost, then behind us, is hidden.  So the window covers early takeoff.
TOS_MIN=1
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
