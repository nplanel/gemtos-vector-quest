/* gen_tables.c — host-side build tool: writes gen_tables.h to stdout.
 *
 * Bakes at build time what would otherwise need soft-float at startup
 * (≈2.5KB of libgcc float routines in the m68k binary), as plain const
 * tables the game reads in place.  (They used to be packed and decoded
 * into BSS at startup, which kept two copies in RAM: the image is loaded
 * whole, so packing saved floppy bytes, not memory.)
 *   kSinQuarter[] — sin(i·2π/LUT_SIZE)·FP_ONE, quarter wave; vquest.c's
 *                   fastSin folds the rest in by symmetry.
 *   kModelVerts[] — vquest_vertices[] · LOGO_SCALE · FP_ONE as int16_t x/y/z.
 *   kModelEdges[] — vquest_edges[] as uint8_t index pairs.
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

static int16_t q[LUT_SIZE / 4 + 1];

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

    printf("#define MODEL_NUM_VERTICES %u\n", (unsigned)NUM_VERTICES);
    printf("#define MODEL_NUM_EDGES    %u\n", (unsigned)NUM_EDGES);
    if (VQUEST_CAPTION_EDGES > NUM_EDGES) {
        fprintf(stderr, "gen_tables: VQUEST_CAPTION_EDGES %u exceeds NUM_EDGES %u\n",
                (unsigned)VQUEST_CAPTION_EDGES, (unsigned)NUM_EDGES);
        return 1;
    }
    printf("/* Caption glyph edges, baked LAST in kModelEdges: render.c appends them\n"
           " * separately so vquest.c can place them in the yellow tail slice. */\n");
    printf("#define MODEL_CAPTION_EDGES %u\n\n", (unsigned)VQUEST_CAPTION_EDGES);

    /* ── vertices ───────────────────────────────────────────────────── */
    /* render.c's rotate() mixes two axes per output; each output is bounded
     * by the vertex norm, which must therefore stay inside int16_t. */
    printf("/* vquest_vertices[] * LOGO_SCALE * FP_ONE. */\n");
    printf("static const int16_t kModelVerts[MODEL_NUM_VERTICES][3] = {");
    for (i = 0; i < NUM_VERTICES; i++) {
        float x = vquest_vertices[i].x * LOGO_SCALE * FP_ONE;
        float y = vquest_vertices[i].y * LOGO_SCALE * FP_ONE;
        float z = vquest_vertices[i].z * LOGO_SCALE * FP_ONE;
        int16_t vx = (int16_t)x, vy = (int16_t)y, vz = (int16_t)z;
        if ((int32_t)vx * vx + (int32_t)vy * vy + (int32_t)vz * vz > 32767L * 32767L) {
            fprintf(stderr, "gen_tables: vertex %u norm exceeds int16_t\n", i);
            return 1;
        }
        if (i % 4 == 0) printf("\n    ");
        printf("{%d,%d,%d},", vx, vy, vz);
    }
    printf("\n};\n\n");

    /* ── edges, byte indices ────────────────────────────────────────────── */
    if (NUM_VERTICES > 256) {
        fprintf(stderr, "gen_tables: %u vertices exceed uint8_t edge indices\n",
                (unsigned)NUM_VERTICES);
        return 1;
    }
    printf("static const uint8_t kModelEdges[MODEL_NUM_EDGES][2] = {");
    for (i = 0; i < NUM_EDGES; i++) {
        if (i % 8 == 0) printf("\n    ");
        printf("{%u,%u},", vquest_edges[i][0], vquest_edges[i][1]);
    }
    printf("\n};\n\n#endif /* GEN_TABLES_H */\n");
    return 0;
}
