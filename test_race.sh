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

# check_tx <file> — sent stream is non-empty, 3-byte framed, 0xAA-synced
check_tx() {
    size=$(stat -c%s "$1")
    [ "$size" -gt 0 ]            || die "$1: nothing transmitted"
    [ $((size % 3)) -eq 0 ]      || die "$1: size $size not a multiple of 3"
    [ "$(od -An -tu1 -N1 "$1" | tr -d ' ')" = 170 ] \
                                 || die "$1: first byte is not 0xAA"
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
: > "$tmp/tx_ctl"
./vq-ascii 0 $MAX_FRAME "$tmp/tx_ctl" /dev/null > "$tmp/ctl.log" || die "vq-ascii control run failed"
check_tx "$tmp/tx_ctl"

# Peer packet = the first packet the control run itself sent (cam_x never
# changes under autopilot, so the remote triangle lands at screen center).
head -c3 "$tmp/tx_ctl" > "$tmp/peer"

: > "$tmp/tx_test"
./vq-ascii 0 $MAX_FRAME "$tmp/tx_test" "$tmp/peer" > "$tmp/test.log" || die "vq-ascii test run failed"
check_tx "$tmp/tx_test"
check_logs "$tmp/ctl.log" "$tmp/test.log"
echo "PASS: linux ascii (posix serial + remote player rendered)"

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

TOS_MIN=60   # cruise starts around frame 40 under autopilot
TOS_MAX=90

run_tos() { # $1=rs232-in $2=rs232-out $3=log
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    hatari-prg-args -q --conout 2 --fast-boot true --fast-forward on \
        --sound off --disable-video on \
        --rs232-in "$1" --rs232-out "$2" -- ./vq-ascii.tos $TOS_MIN $TOS_MAX 2>&1 \
        | tr -d '\r\000' \
        | grep -aE '^(FRAME|ANGLES|LINES|BBOX|LINE|ALINES|ALINE|RLINES|RLINE|END_FRAME|DONE)' > "$3"
    grep -q '^DONE ' "$3" || die "$3: TOS run did not complete (no DONE line)"
}

# hatari delivers --rs232-in bytes at the emulated baud rate starting at
# boot, so a single packet is consumed before serial_init runs.  Repeat it
# to ~12 KiB (≈13 s at 9600 baud), spanning boot + intro + the test window.
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
