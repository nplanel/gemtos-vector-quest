/* gen_tables.c — host-side build tool: writes gen_tables.h to stdout.
 *
 * Bakes at build time what the game used to compute with soft-float at
 * startup (≈2.5KB of libgcc float routines in the m68k binary):
 *   kSinQuarter[]  — sin(i·2π/LUT_SIZE)·FP_ONE for the first quarter wave;
 *                    vquest.c expands it to the full sinLUT by symmetry.
 *   kModelVerts[]  — vquest_vertices[] · LOGO_SCALE · FP_ONE as int16_t.
 *   kModelEdges[]  — vquest_edges[] verbatim (uint16_t indices).
 * The game includes gen_tables.h instead of vquest_model.h, so the float
 * model data never reaches the target binary.
 */
#include <stdio.h>
#include <math.h>
#include "vquest.h"        /* FP_ONE, LUT_SIZE, LOGO_SCALE, Point3DFloat */
#include "vquest_model.h"  /* vquest_vertices[], vquest_edges[] */

#define NUM_VERTICES (sizeof(vquest_vertices) / sizeof(vquest_vertices[0]))
#define NUM_EDGES    (sizeof(vquest_edges)    / sizeof(vquest_edges[0]))

int main(void)
{
    unsigned i;

    printf("/* generated — do not edit. Regenerate with: make gen_tables.h */\n");
    printf("#ifndef GEN_TABLES_H\n#define GEN_TABLES_H\n\n");

    printf("/* sin(i*2pi/%d)*%d, first quarter wave (see gen_tables.c) */\n",
           LUT_SIZE, FP_ONE);
    printf("static const int16_t kSinQuarter[%d] = {", LUT_SIZE / 4 + 1);
    for (i = 0; i <= LUT_SIZE / 4; i++) {
        if (i % 12 == 0) printf("\n    ");
        printf("%d,", (int)(sinf((float)i * 2.0f * (float)M_PI / LUT_SIZE)
                            * FP_ONE + 0.5f));
    }
    printf("\n};\n\n");

    printf("#define MODEL_NUM_VERTICES %u\n", (unsigned)NUM_VERTICES);
    printf("#define MODEL_NUM_EDGES    %u\n\n", (unsigned)NUM_EDGES);

    printf("/* vquest_vertices[] * LOGO_SCALE * FP_ONE (x, y, z triplets) */\n");
    printf("static const int16_t kModelVerts[MODEL_NUM_VERTICES][3] = {");
    for (i = 0; i < NUM_VERTICES; i++) {
        if (i % 4 == 0) printf("\n    ");
        printf("{%d,%d,%d},",
               (int)(int16_t)(vquest_vertices[i].x * LOGO_SCALE * FP_ONE),
               (int)(int16_t)(vquest_vertices[i].y * LOGO_SCALE * FP_ONE),
               (int)(int16_t)(vquest_vertices[i].z * LOGO_SCALE * FP_ONE));
    }
    printf("\n};\n\n");

    printf("static const uint16_t kModelEdges[MODEL_NUM_EDGES][2] = {");
    for (i = 0; i < NUM_EDGES; i++) {
        if (i % 8 == 0) printf("\n    ");
        printf("{%d,%d},", vquest_edges[i][0], vquest_edges[i][1]);
    }
    printf("\n};\n\n#endif /* GEN_TABLES_H */\n");
    return 0;
}
