#!/bin/sh
# Cycle-headroom measurement for the real renderer under hatari.
#
# vquest-perf.tos (make perf / -DVQ_PERF) replaces backend_present's Vsync()
# with a counted busy-wait on _frclock and forces the autopilot keys, so runs
# are deterministic.  Two runs with different max_frame difference out the
# fixed cost of boot, intro and title:
#
#   spare spins/frame = (spins(N2) - spins(N1)) / (frames(N2) - frames(N1))
#
# Each spin of the wait loop is SPIN_CYC CPU cycles (from the perf_wait_vbl
# disassembly: move.l $466.w,d0 / cmp.l / bne / addq ≈ 40); a 50 Hz PAL frame
# is 160,256 CPU cycles.  Raw spins are the exact A/B metric; the cycle
# conversion is an estimate.  overruns counts frames that missed their VBL
# deadline (zero spins) — a growing delta means dropped frames.
#
# Usage: ./perf_frames.sh [N1] [N2]     (defaults 300 1300)
set -u

N1=${1:-300}
N2=${2:-1300}
SPIN_CYC=40
FRAME_CYC=160256

command -v hatari-prg-args >/dev/null 2>&1 \
    || { echo "hatari-prg-args not found" >&2; exit 1; }
[ -f vquest-perf.tos ] || { echo "vquest-perf.tos missing (make perf)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

run() { # $1=max_frame $2=out — captures the PERF line printed at exit
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    hatari-prg-args -q --conout 2 --fast-boot true --fast-forward on \
        --sound off --disable-video on -- ./vquest-perf.tos 0 "$1" 2>&1 \
      | tr -d '\r\000' | grep -a '^PERF ' > "$2"
    grep -q 'frames=' "$2" || { echo "FAIL: run($1): no PERF line" >&2; exit 1; }
}

run "$N1" "$tmp/a"
run "$N2" "$tmp/b"
run "$N2" "$tmp/b2"

field() { sed -n "s/.*$2=\([0-9]*\).*/\1/p" "$1"; }

# TOS boot state jitters a few spins run-to-run (measured ±2 in 1.4M);
# warn only on drift that would actually skew a comparison.
Sb=$(field "$tmp/b" spins); Sb2=$(field "$tmp/b2" spins)
d=$((Sb - Sb2)); [ "${d#-}" -le 200 ] \
    || echo "WARNING: repeat run differs by $d spins — nondeterministic" >&2

F1=$(field "$tmp/a" frames);   F2=$(field "$tmp/b" frames)
S1=$(field "$tmp/a" spins);    S2=$(field "$tmp/b" spins)
O1=$(field "$tmp/a" overruns); O2=$(field "$tmp/b" overruns)

awk -v f1="$F1" -v f2="$F2" -v s1="$S1" -v s2="$S2" -v o1="$O1" -v o2="$O2" \
    -v sc="$SPIN_CYC" -v fc="$FRAME_CYC" 'BEGIN {
    df = f2 - f1; ds = s2 - s1; do_ = o2 - o1
    if (df <= 0) { print "FAIL: no frame delta (" f1 " -> " f2 ")"; exit 1 }
    spf   = ds / df
    spare = spf * sc
    busy  = fc - spare
    printf "window: %d frames   spare: %.1f spins/frame (~%d cycles)\n", df, spf, spare
    printf "busy:   ~%d cycles/frame  (%.1f%% of the %d-cycle budget)\n", busy, 100*busy/fc, fc
    printf "overruns in window: %d\n", do_
}'
