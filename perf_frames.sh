#!/bin/sh
# CPU cost per frame of the real renderer under hatari, per game state.
#
# vquest-perf.tos (make perf / -DVQ_PERF) pins 50 Hz, forces the autopilot
# keys (hold Up+Fire) so runs are deterministic, and replaces backend_present's
# Vsync() with a counted busy-wait on _frclock.  For every frame it records
# the VBLs the frame spanned and the spins it burned waiting, summed per
# GameState; at exit it calibrates the spins of one idle VBL.  Work per frame
# is then
#
#   work = (vbls * cal - spins) / frames        (in idle-spin units)
#
# converted to cycles with the 160,256-cycle PAL frame.  Unlike a bare
# "spare spins" figure this stays exact when frames take more than one VBL
# (they do: a race frame is ~1.4 VBL), and "VBL/frame" is the real frame
# pacing — 1.00 is a solid 50 fps, 2.00 is 25 fps.
#
# Usage: ./perf_frames.sh [frames]     (default 1300)
set -u

N=${1:-1300}
FRAME_CYC=160256

command -v hatari-prg-args >/dev/null 2>&1 \
    || { echo "hatari-prg-args not found" >&2; exit 1; }
[ -f vquest-perf.tos ] || { echo "vquest-perf.tos missing (make perf)" >&2; exit 1; }

out=$(SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
      hatari-prg-args -q --conout 2 --fast-boot true --fast-forward on \
          --sound off --disable-video on -- ./vquest-perf.tos 0 "$N" 2>&1 \
      | tr -d '\r\000' | grep -a '^PERF ')
echo "$out" | grep -q 'cal=' || { echo "FAIL: no PERF output" >&2; exit 1; }

echo "$out" | awk -v fc="$FRAME_CYC" '
    /cal=/ { split($2, c, "="); cal = c[2] }
    /bucket=/ {
        for (i = 2; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
        b = v["bucket"]; f[b] = v["frames"]; vb[b] = v["vbls"]; sp[b] = v["spins"]
    }
    END {
        split("CRUISE CRASH GATE COUNTDOWN INTRO", name, " ")
        printf "%-10s %7s %13s %10s %6s\n", "state", "frames", "cycles/frame", "VBL/frame", "fps"
        for (b = 0; b <= 4; b++) {
            if (f[b] == 0) continue
            work = (vb[b] * cal - sp[b]) / f[b] / cal * fc
            printf "%-10s %7d %13d %10.2f %6.1f\n", name[b + 1], f[b], work,
                   vb[b] / f[b], 50 * f[b] / vb[b]
        }
        printf "(idle VBL = %d spins)\n", cal
    }'
