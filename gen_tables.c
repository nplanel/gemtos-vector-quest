/* gen_tables.c — host-side build tool: writes gen_tables.h to stdout.
 *
 * Bakes at build time what would otherwise need soft-float at startup
 * (≈2.5KB of libgcc float routines in the m68k binary), as plain const
 * tables the game reads in place.  (They used to be packed and decoded
 * into BSS at startup, which kept two copies in RAM: the image is loaded
 * whole, so packing saved floppy bytes, not memory.)
 *   kSinQuarter[] — sin(i·2π/LUT_SIZE)·FP_ONE, quarter wave; vquest.c's
 *                   fastSin folds the rest in by symmetry.
 *   kModelPairs[], kModelSingles[] — vquest_vertices[] · LOGO_SCALE ·
 *                   FP_ONE as int16_t, merged and paired by shared x (see
 *                   below).
 *   kModelEdges[] — vquest_edges[] as uint8_t indices of projected slots.
 * The game includes gen_tables.h instead of dna_helix.h, so the float
 * model data never reaches the target binary.
 */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include "vquest.h"        /* FP_ONE, LUT_SIZE */
#include "dna_helix.h"     /* vquest_vertices[], vquest_edges[],
                            * VQUEST_CAPTION_EDGES, LOGO_SCALE */

#define NUM_VERTICES (sizeof(vquest_vertices) / sizeof(vquest_vertices[0]))
#define NUM_EDGES    (sizeof(vquest_edges)    / sizeof(vquest_edges[0]))

/* Largest vertex norm: render.c draws the logo at 32 px per FP_ONE, so
 * 3072 keeps it within 96 px of the screen centre, unclamped. */
#define MODEL_MAX_NORM 3072

static int16_t q[LUT_SIZE / 4 + 1];
static int16_t  uniq[NUM_VERTICES][3];     /* distinct baked vertices        */
static unsigned vmap[NUM_VERTICES];        /* source vertex -> uniq index    */
static int      slot[NUM_VERTICES];        /* uniq index -> projected slot   */
static unsigned pair[NUM_VERTICES][2];     /* the uniq indices of each pair  */
static unsigned single[NUM_VERTICES];      /* the uniq indices left unpaired */
static unsigned nuniq, npairs, nsingles;

int main(void)
{
    unsigned i;

    printf("/* generated — do not edit. Regenerate with: make gen_tables.h */\n");
    printf("#ifndef GEN_TABLES_H\n#define GEN_TABLES_H\n\n");

    /* ── sin quarter wave ───────────────────────────────────────────── */
    for (i = 0; i <= LUT_SIZE / 4; i++)
        q[i] = (int16_t)(sinf((float)i * 2.0f * (float)M_PI / LUT_SIZE)
                         * FP_ONE + 0.5f);
    printf("/* sin(i*2pi/%d)*%d, first quarter wave (vquest.c's fastSin folds\n"
           " * in the rest). */\n", LUT_SIZE, FP_ONE);
    printf("static const int16_t kSinQuarter[%d] = {", LUT_SIZE / 4 + 1);
    for (i = 0; i <= LUT_SIZE / 4; i++) {
        if (i % 12 == 0) printf("\n    ");
        printf("%d,", q[i]);
    }
    printf("\n};\n\n");

    printf("#define MODEL_NUM_EDGES    %u\n", (unsigned)NUM_EDGES);
    if (VQUEST_CAPTION_EDGES > NUM_EDGES) {
        fprintf(stderr, "gen_tables: VQUEST_CAPTION_EDGES %u exceeds NUM_EDGES %u\n",
                (unsigned)VQUEST_CAPTION_EDGES, (unsigned)NUM_EDGES);
        return 1;
    }
    printf("/* Caption glyph edges, baked LAST in kModelEdges: render.c appends them\n"
           " * separately so vquest.c can place them in the yellow tail slice. */\n");
    printf("#define MODEL_CAPTION_EDGES %u\n", (unsigned)VQUEST_CAPTION_EDGES);
    printf("#define MODEL_MAX_NORM %d\n\n", MODEL_MAX_NORM);

    /* ── vertices, as pairs sharing x ────────────────────────────────── */
    /* render.c's render_logo() mixes the axes per output; each output is
     * bounded by the vertex norm, which MODEL_MAX_NORM keeps on-screen (the
     * edges are appended unclamped).
     *
     * Two of its five products per vertex depend on x alone, and the helix
     * is built of vertices that share their x: strand A's i-th point and
     * strand B's, and the two ends of each rung.  So the vertices are baked
     * as pairs {x, y0, z0, y1, z1} projected into slots 2k and 2k+1, which
     * computes those two products once per pair: 8 multiplies for two
     * vertices, not 10.  Identical vertices (the caption's glyph strokes
     * share their end points, a rung may land on a strand point) are merged
     * first.  A vertex with no partner goes to kModelSingles {x, y, z},
     * projected after the pairs at 5 multiplies, so any model costs at most
     * what the plain layout would.  The edges are remapped to the slots, in
     * their order. */
    for (i = 0; i < NUM_VERTICES; i++) {
        float x = vquest_vertices[i].x * LOGO_SCALE * FP_ONE;
        float y = vquest_vertices[i].y * LOGO_SCALE * FP_ONE;
        float z = vquest_vertices[i].z * LOGO_SCALE * FP_ONE;
        int16_t vx = (int16_t)x, vy = (int16_t)y, vz = (int16_t)z;
        unsigned j;
        if ((int32_t)vx * vx + (int32_t)vy * vy + (int32_t)vz * vz
                > (int32_t)MODEL_MAX_NORM * MODEL_MAX_NORM) {
            fprintf(stderr, "gen_tables: vertex %u norm exceeds %d\n", i, MODEL_MAX_NORM);
            return 1;
        }
        for (j = 0; j < nuniq; j++)
            if (uniq[j][0] == vx && uniq[j][1] == vy && uniq[j][2] == vz) break;
        if (j == nuniq) {
            uniq[j][0] = vx; uniq[j][1] = vy; uniq[j][2] = vz;
            slot[j] = -1;
            nuniq++;
        }
        vmap[i] = j;
    }
    for (i = 0; i < nuniq; i++) {
        unsigned j;
        if (slot[i] >= 0) continue;
        for (j = i + 1; j < nuniq; j++)
            if (slot[j] < 0 && uniq[j][0] == uniq[i][0]) break;
        if (j == nuniq) continue;
        pair[npairs][0] = i;
        pair[npairs][1] = j;
        slot[i] = (int)(2 * npairs);
        slot[j] = (int)(2 * npairs + 1);
        npairs++;
    }
    for (i = 0; i < nuniq; i++) {
        if (slot[i] >= 0) continue;
        single[nsingles] = i;
        slot[i] = (int)(2 * npairs + nsingles);
        nsingles++;
    }
    if (2 * npairs + nsingles > 256) {
        fprintf(stderr, "gen_tables: %u vertex slots exceed uint8_t edge indices\n",
                2 * npairs + nsingles);
        return 1;
    }
    printf("#define MODEL_NUM_PAIRS    %u\n", npairs);
    printf("#define MODEL_NUM_SINGLES  %u\n", nsingles);
    printf("#define MODEL_NUM_VERTICES %u   /* projected slots: pairs, then singles */\n\n",
           2 * npairs + nsingles);
    if (npairs) {
        printf("/* vquest_vertices[] * LOGO_SCALE * FP_ONE, as {x, y0, z0, y1, z1}. */\n");
        printf("static const int16_t kModelPairs[MODEL_NUM_PAIRS][5] = {");
        for (i = 0; i < npairs; i++) {
            const int16_t *a = uniq[pair[i][0]], *b = uniq[pair[i][1]];
            if (i % 3 == 0) printf("\n    ");
            printf("{%d,%d,%d,%d,%d},", a[0], a[1], a[2], b[1], b[2]);
        }
        printf("\n};\n\n");
    }
    if (nsingles) {
        printf("/* The vertices no other one shares x with, as {x, y, z}. */\n");
        printf("static const int16_t kModelSingles[MODEL_NUM_SINGLES][3] = {");
        for (i = 0; i < nsingles; i++) {
            const int16_t *a = uniq[single[i]];
            if (i % 4 == 0) printf("\n    ");
            printf("{%d,%d,%d},", a[0], a[1], a[2]);
        }
        printf("\n};\n\n");
    }

    /* ── edges, byte indices into the projected slots ───────────────────── */
    printf("static const uint8_t kModelEdges[MODEL_NUM_EDGES][2] = {");
    for (i = 0; i < NUM_EDGES; i++) {
        if (i % 8 == 0) printf("\n    ");
        printf("{%d,%d},", slot[vmap[vquest_edges[i][0]]], slot[vmap[vquest_edges[i][1]]]);
    }
    printf("\n};\n\n#endif /* GEN_TABLES_H */\n");
    return 0;
}
