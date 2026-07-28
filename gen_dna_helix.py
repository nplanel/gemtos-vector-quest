#!/usr/bin/env python3
"""gen_dna_helix.py — convert doublehelix6.stl into dna_helix.h (game model).

Measures the mesh (strand radius, twist rate, groove offset, rung layout)
and re-emits it as a clean parametric wireframe:

  * two helical strands as 3-D polylines (constant radius, constant twist,
    strands exactly the measured groove angle apart — 135°, i.e. the
    major/minor-groove geometry of real DNA that the STL encodes)
  * one chord rung per measured rung z position
  * base slab dropped (it is display stand, not logo)

STL z (helix axis) maps to model x, scaled to X_SPAN so the model fills the
same slot as the old logo; STL x/y (radial plane) maps to model y/z with a
cyclic permutation (chirality-preserving).  The previous hand-traced
dna_helix.h was a flat side projection: each strand squashed into its own
y half-plane (radius 12..30 instead of constant), 29 rungs instead of 10,
no consistent twist — it could not tumble in 3-D convincingly.

Vertex budget: MODEL_NUM_VERTICES must stay <= 256 (uint8 edge indices in
gen_tables.c).  Span budget after LOGO_SCALE*FP_ONE (gen_tables packing):
x < 4096 (12 bits), y/z < 1024 (10 bits) — checked below, fails loudly.

Usage: python3 gen_dna_helix.py > dna_helix.h   (or make gen-dna)

doublehelix6.stl is optional: without it the cached MEASURED parameters (fitted
from the generated header, exact to 7e-4 model units) are used instead, so the
display knobs below can be retuned with no STL in the tree.
"""

import math
import os
import struct
import sys
import collections

STL_PATH = "doublehelix6.stl"

# ── Model layout constants ──────────────────────────────────────────────────
X_SPAN      = 440.0   # model x extent (fits 12-bit packing at LOGO_SCALE 2/230:
                      # 440 * 2/230 * 1024 = 3922 < 4096, 4% margin)
N_STRAND    = 96      # vertices per strand polyline
R_MODEL     = 28.0    # model strand radius: STL R (~21.7) x 1.29 for screen
                      # legibility (radial-only scale; axial proportions kept)
TURNS       = 2       # twist multiplier: the STL has exactly 1 turn over its
                      # length (10 base pairs, authentic DNA pitch) which at
                      # logo size reads as a single crossing; 2 turns is the
                      # same helix wound tighter — instantly readable as DNA.

# Caption written inside the helix cage: the game's 5x8 vector glyphs (same
# shapes as draw.c's seg_*) laid flat in the x-y plane at z=0, centred at
# the middle of the molecule.  TEXT_HEIGHT is deliberately ~1.4x the helix
# diameter so the glyph tops/bottoms clear the strand envelope (y +/-R_MODEL)
# and the letters stay readable where the strands weave through the middle.
# The text tumbles with the logo: readable when the ladder is side-on,
# edge-on when the helix turns end-on.  Set TEXT to "" to disable.
TEXT        = "ADN"
TEXT_HEIGHT = 80.0    # model units (helix diameter is 2*R_MODEL = 56)
TEXT_SCALE  = TEXT_HEIGHT / 7.0     # glyph grid unit -> model units
GLYPHS = {            # (x0,y0,x1,y1) on a 4x7 grid, y down — mirror of draw.c
    'A': [(0,7,2,0), (4,7,2,0), (1,4,3,4)],
    'D': [(0,0,0,7), (0,0,3,0), (3,0,4,1), (4,1,4,6), (4,6,3,7), (3,7,0,7)],
    'N': [(0,7,0,0), (0,0,4,7), (4,7,4,0)],
}

FP_ONE      = 1024
LOGO_SCALE  = 2.0 / 230.0   # vquest.h — used here only for the span checks

# ── Cached STL measurement ──────────────────────────────────────────────────
# measure() needs doublehelix6.stl, which is not in the repo (it lives with the
# original artwork).  These are its outputs, recovered by least-squares fit from
# the generated header and exact to 7e-4 model units.  With them, `make gen-dna`
# reproduces the model from the display knobs alone (N_STRAND, R_MODEL, TURNS,
# TEXT) with no STL.  Drop the STL next to this script to re-measure instead;
# that path stays authoritative if the artwork ever changes.
#
# Frame: STL z origin shifted to the base (base_z = 0); only differences of z
# matter downstream (model_x centres on the midpoint, theta is affine in z), so
# the shift is free.  z_top is derived from the fitted total twist and the
# measured 2.400 deg/z rate: 695.857 / (2 * 2.400) = 144.9703.
MEASURED = {
    "base_z":    0.0,
    "z_top":     144.9703,
    "r_stl":     21.0,
    "omega":     math.radians(2.400),    # STL twist, deg per STL z unit
    "groove":    math.radians(135.0),
    "theta_ref": 0.343967739,            # strand-A angle at z_ref
    "z_ref":     0.0,
    "rung_z": [8.6554, 22.1330, 35.9988, 49.3436, 63.3471,
               76.6270, 90.5906, 104.0099, 117.8384, 131.2063],
}


def read_stl(path):
    """Returns (sorted unique vertices, set of undirected edges)."""
    with open(path, "rb") as f:
        f.read(80)
        (ntri,) = struct.unpack("<I", f.read(4))
        verts = set()
        edges = set()
        for _ in range(ntri):
            d = struct.unpack("<12fH", f.read(50))
            vs = [(d[i], d[i + 1], d[i + 2]) for i in (3, 6, 9)]
            verts.update(vs)
            for a, b in ((0, 1), (1, 2), (2, 0)):
                edges.add((vs[a], vs[b]) if vs[a] < vs[b]
                          else (vs[b], vs[a]))
    return sorted(verts), edges


def circular_mean(angles):
    s = sum(math.sin(a) for a in angles)
    c = sum(math.cos(a) for a in angles)
    return math.atan2(s, c)


def angle_clusters(angles, gap=0.7):
    """Cluster a list of angles (rad); returns circular means, sorted."""
    a = sorted(x % (2 * math.pi) for x in angles)
    cl = []
    for x in a:
        if cl and x - cl[-1][-1] < gap:
            cl[-1].append(x)
        else:
            cl.append([x])
    if len(cl) > 1 and (a[0] + 2 * math.pi) - a[-1] < gap:
        cl[0] = cl.pop() + cl[0]
    return sorted(circular_mean(c) for c in cl)


def measure(verts, edges):
    """Extract helix parameters from the mesh.  Returns a dict.

    Rung detection: a rung is a horizontal cylinder; its two cap rings sit
    at exact z values and its cap edges are long (chord-length) horizontal
    segments — long |dz|==0 edges cluster tightly around each rung z.
    Each rung's chord direction then gives the strand bisector at that z
    (bisector = chord_dir + 90deg), and the rung-to-rung rotation of the
    chord gives the twist rate — both much cleaner signals than clustering
    strand tube cross-sections.
    """
    # Base slab: radially large verts (base radius >> tube outer radius).
    r_max = max(math.hypot(x, y) for x, y, z in verts)
    slab_z = max(z for x, y, z in verts if math.hypot(x, y) > 0.75 * r_max)
    z_top = max(z for x, y, z in verts)
    z0 = slab_z + 5.0          # rim/taper allowance above the slab
    helix = [(x, y, z) for x, y, z in verts if z > z0]

    # Strand centerline radius: mid between inner and outer tube surface.
    rs = sorted(math.hypot(x, y) for x, y, z in helix)
    r_center = (rs[len(rs) // 20] + rs[-len(rs) // 20]) / 2.0  # ~p5/p95 mid

    # Rung z bands from long horizontal edges: rung cap chords are ~34 long
    # (centerline chord 2*R*sin(groove/2) ~ 39 minus tube radius at each
    # end), strand-tube edges are < ~15; 1.25*R sits between the clusters.
    hz = [e for e in edges
          if abs(e[0][2] - e[1][2]) < 0.01 and e[0][2] > z0
          and math.dist(*e) > 1.25 * r_center]
    by_z = collections.defaultdict(list)
    for e in hz:
        by_z[round(e[0][2], 1)].append(e)
    bands = []
    for z in sorted(by_z):
        if bands and z - bands[-1][-1] < 6.0:
            bands[-1].append(z)
        else:
            bands.append([z])
    rung_z = [sum(b) / len(b) for b in bands]
    if len(rung_z) < 2:
        sys.exit("gen_dna_helix: found %d rungs, expected several — "
                 "mesh layout changed?" % len(rung_z))

    # Chord direction per rung: its longest horizontal edge.
    rung_dir = []
    for b in bands:
        es = [e for z in b for e in by_z[z]]
        (a, c) = max(es, key=lambda e: math.dist(*e))
        rung_dir.append(math.atan2(c[1] - a[1], c[0] - a[0]))

    # Twist rate: linear fit of unwrapped chord directions vs rung z.
    dirs = [rung_dir[0]]
    for d in rung_dir[1:]:
        while d < dirs[-1] - math.pi / 2.0:
            d += math.pi
        while d > dirs[-1] + math.pi / 2.0:
            d -= math.pi
        dirs.append(d)
    n = len(rung_z)
    mz = sum(rung_z) / n
    md = sum(dirs) / n
    omega = sum((z - mz) * (d - md) for z, d in zip(rung_z, dirs)) / \
        sum((z - mz) ** 2 for z in rung_z)

    # Strand angles + groove, anchored on one clean cross-section midway
    # between rungs (exactly two tube clusters there).  Anchoring both
    # strand phases on the measured clusters — instead of deriving them
    # from the chord direction, which is mod-pi ambiguous — is what keeps
    # the model's chirality identical to the STL's (right-handed helix).
    z_ref = None
    cl = None
    za = (rung_z[0] + rung_z[1]) / 2.0
    for dz in (0.0, 1.0, -1.0, 2.0, -2.0):
        angs = [math.atan2(y, x) for x, y, z in helix
                if abs(z - (za + dz)) < 1.5]
        cl = angle_clusters(angs)
        if len(cl) == 2:
            z_ref = za + dz
            break
    if z_ref is None:
        sys.exit("gen_dna_helix: no clean 2-strand cross-section found")
    c0, c1 = cl
    d = (c1 - c0) % (2 * math.pi)
    if d > math.pi:            # order A->B counterclockwise = +groove
        c0, c1 = c1, c0
        d = 2 * math.pi - d
    groove = d
    theta_ref = c0             # strand-A angle at z_ref; B = A + groove

    return {
        "base_z": z0, "z_top": z_top,
        "r_stl": r_center, "omega": omega, "groove": groove,
        "theta_ref": theta_ref, "z_ref": z_ref,
        "rung_z": rung_z,
    }


def generate(m):
    """Build (vertices, edges) in model space from the measured parameters."""
    z0, z1 = m["base_z"], m["z_top"]
    height = z1 - z0
    scale_x = X_SPAN / height

    def model_x(z):
        return (z - (z0 + z1) / 2.0) * scale_x

    def theta(z):
        return m["theta_ref"] + TURNS * m["omega"] * (z - m["z_ref"])

    verts = []
    edges = []

    # Two strands: y = R cos(theta), z = R sin(theta) (cyclic map STL
    # x,y,z -> model y,z,x preserves right-handedness).
    for strand in range(2):
        base_idx = len(verts)
        off = strand * m["groove"]
        for i in range(N_STRAND):
            z = z0 + height * i / (N_STRAND - 1)
            t = theta(z) + off
            verts.append((model_x(z),
                          R_MODEL * math.cos(t),
                          R_MODEL * math.sin(t)))
            if i:
                edges.append((base_idx + i - 1, base_idx + i))

    # Rungs: chord between the two strand centrelines at each measured z.
    for zc in m["rung_z"]:
        t = theta(zc)
        a = (model_x(zc), R_MODEL * math.cos(t), R_MODEL * math.sin(t))
        b = (model_x(zc),
             R_MODEL * math.cos(t + m["groove"]),
             R_MODEL * math.sin(t + m["groove"]))
        i = len(verts)
        verts.extend((a, b))
        edges.append((i, i + 1))

    # Caption glyphs: 4x7 grid cells, 2-unit gap, centred at x=0, z=0.
    # Model +y is down on screen (project(): screen_y += p.y>>5), so grid y
    # maps straight through with a -3.5 vertical centring offset.
    if TEXT:
        gap = 2.0
        cell_w = 4.0 + gap
        x0 = -((len(TEXT) * cell_w - gap) / 2.0) * TEXT_SCALE
        for ci, ch in enumerate(TEXT):
            if ch == ' ':
                continue
            if ch not in GLYPHS:
                sys.exit("gen_dna_helix: no glyph for %r — mirror it from "
                         "draw.c's seg_* tables into GLYPHS" % ch)
            gx0 = x0 + ci * cell_w * TEXT_SCALE
            for sx0, sy0, sx1, sy1 in GLYPHS[ch]:
                i = len(verts)
                verts.append((gx0 + sx0 * TEXT_SCALE,
                              (sy0 - 3.5) * TEXT_SCALE, 0.0))
                verts.append((gx0 + sx1 * TEXT_SCALE,
                              (sy1 - 3.5) * TEXT_SCALE, 0.0))
                edges.append((i, i + 1))

    return verts, edges


def emit(verts, edges, m):
    # Span checks against the gen_tables.c packing limits (fail loudly).
    xs = [v[0] for v in verts]
    ys = [v[1] for v in verts]
    zs = [v[2] for v in verts]
    sx = (max(xs) - min(xs)) * LOGO_SCALE * FP_ONE
    sy = (max(ys) - min(ys)) * LOGO_SCALE * FP_ONE
    sz = (max(zs) - min(zs)) * LOGO_SCALE * FP_ONE
    if len(verts) > 256:
        sys.exit("gen_dna_helix: %d vertices exceed uint8 edge indices"
                 % len(verts))
    if sx >= 4096 or sy >= 1024 or sz >= 1024:
        sys.exit("gen_dna_helix: span %.0f/%.0f/%.0f exceeds 12/10/10-bit "
                 "packing" % (sx, sy, sz))

    print("""#ifndef DNA_HELIX_H
#define DNA_HELIX_H

#include "vquest.h"  /* Point3DFloat */

/* DNA double-helix logo — generated by gen_dna_helix.py from doublehelix6.stl
 * (do not edit by hand; regenerate with: make gen-dna).
 * True 3-D parametric wireframe measured from the mesh:
 *   strand radius %.1f (STL %.1f, x1.29 radial for screen), axis -> model x
 *   twist %.3f deg/unit (STL %.3f x TURNS=%d), %.0f deg total, groove %.0f deg
 *   %d rungs at the STL's measured positions, base slab removed
 *   "%s" caption: 5x8 vector glyphs (draw.c shapes) flat at z=0, mid-helix
 * %d vertices: %d-pt strand A, %d-pt strand B, %d rungs, %d glyph pts. */
""" % (R_MODEL, m["r_stl"], TURNS * math.degrees(m["omega"]),
       math.degrees(m["omega"]), TURNS,
       TURNS * math.degrees(m["omega"]) * (m["z_top"] - m["base_z"]),
       math.degrees(m["groove"]), len(m["rung_z"]), TEXT,
       len(verts), N_STRAND, N_STRAND, len(m["rung_z"]),
       len(verts) - 2 * N_STRAND - 2 * len(m["rung_z"])))

    print("const Point3DFloat vquest_vertices[] = {")
    for i, v in enumerate(verts):
        tag = ""
        if i == 0:
            tag = "  /* strand A */"
        elif i == N_STRAND:
            tag = "  /* strand B */"
        elif i == 2 * N_STRAND:
            tag = "  /* rungs */"
        print("    {%.3ff, %.3ff, %.3ff},%s" % (v[0], v[1], v[2], tag))
    print("};\n")

    print("const int vquest_edges[][2] = {")
    for e in edges:
        print("    {%d, %d}," % e)
    print("};")
    print("#endif /* DNA_HELIX_H */")


def main():
    if os.path.exists(STL_PATH):
        verts, edges = read_stl(STL_PATH)
        m = measure(verts, edges)
        sys.stderr.write(
            "measured: base z<%.1f, top %.1f, R=%.2f, twist=%.3f deg/z "
            "(%.0f deg total), groove=%.1f deg, %d rungs at %s\n"
            % (m["base_z"], m["z_top"], m["r_stl"], math.degrees(m["omega"]),
               math.degrees(m["omega"]) * (m["z_top"] - m["base_z"]),
               math.degrees(m["groove"]), len(m["rung_z"]),
               ["%.0f" % z for z in m["rung_z"]]))
    else:
        m = MEASURED
        sys.stderr.write("gen_dna_helix: %s not found — using cached "
                         "MEASURED parameters\n" % STL_PATH)
    mv, me = generate(m)
    emit(mv, me, m)


if __name__ == "__main__":
    main()
