/* gen_tables.c — host-side build tool: writes gen_tables.h to stdout.
 *
 * Bakes at build time what the game used to compute with soft-float at
 * startup (≈2.5KB of libgcc float routines in the m68k binary), in packed
 * form (the game decodes into RAM once at startup — BSS is free, binary
 * bytes are not):
 *   kSinQuarterNib[] — 4-bit deltas of sin(i·2π/LUT_SIZE)·FP_ONE, quarter
 *                      wave; vquest.c integrates and expands by symmetry.
 *   kModelVertsPacked[] — bias-packed 13-bit x / 11-bit y per vertex in 3
 *                      bytes; z is constant (MODEL_Z).  render.c decodes.
 *   kModelEdges[]    — vquest_edges[] as uint8_t index pairs.
 * The game includes gen_tables.h instead of vquest_model.h, so the float
 * model data never reaches the target binary.
 *
 * Every encoding is verified by round-trip here: if a future model or LUT
 * change breaks a packing assumption, the build fails loudly.
 */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include "vquest.h"        /* FP_ONE, LUT_SIZE, LOGO_SCALE, Point3DFloat */
#include "vquest_model.h"  /* vquest_vertices[], vquest_edges[] */

#define NUM_VERTICES (sizeof(vquest_vertices) / sizeof(vquest_vertices[0]))
#define NUM_EDGES    (sizeof(vquest_edges)    / sizeof(vquest_edges[0]))

static int16_t q[LUT_SIZE / 4 + 1];
static uint8_t nib[LUT_SIZE / 8];
static int16_t vx[NUM_VERTICES], vy[NUM_VERTICES], vz[NUM_VERTICES];
static uint8_t vpack[NUM_VERTICES][3];

int main(void)
{
    unsigned i;

    printf("/* generated — do not edit. Regenerate with: make gen_tables.h */\n");
    printf("#ifndef GEN_TABLES_H\n#define GEN_TABLES_H\n\n");

    /* ── sin quarter wave, nibble-packed deltas ─────────────────────────── */
    for (i = 0; i <= LUT_SIZE / 4; i++)
        q[i] = (int16_t)(sinf((float)i * 2.0f * (float)M_PI / LUT_SIZE)
                         * FP_ONE + 0.5f);
    if (q[0] != 0) {
        fprintf(stderr, "gen_tables: sin[0] != 0\n");
        return 1;
    }
    for (i = 1; i <= LUT_SIZE / 4; i++) {
        int d = q[i] - q[i - 1];
        unsigned j = i - 1;              /* delta index, 0-based */
        if (d < 0 || d > 15) {
            fprintf(stderr, "gen_tables: sin delta %d not a nibble "
                            "(LUT_SIZE/FP_ONE changed?)\n", d);
            return 1;
        }
        if (j & 1) nib[j >> 1] |= (uint8_t)(d << 4);
        else       nib[j >> 1]  = (uint8_t)d;
    }
    {   /* round-trip */
        int16_t v = 0;
        for (i = 1; i <= LUT_SIZE / 4; i++) {
            uint8_t b = nib[(i - 1) >> 1];
            v = (int16_t)(v + (((i - 1) & 1) ? (b >> 4) : (b & 15)));
            if (v != q[i]) {
                fprintf(stderr, "gen_tables: sin round-trip mismatch at %u\n", i);
                return 1;
            }
        }
    }
    printf("/* sin(i*2pi/%d)*%d, first quarter wave, delta-packed: sin[0]=0 is\n"
           " * implicit; byte k holds delta[2k] in the low nibble and delta[2k+1]\n"
           " * in the high nibble (deltas are 0..4, max slope pi).  vquest.c's\n"
           " * lut_init() integrates while expanding to the full wave. */\n",
           LUT_SIZE, FP_ONE);
    printf("static const uint8_t kSinQuarterNib[%d] = {", LUT_SIZE / 8);
    for (i = 0; i < LUT_SIZE / 8; i++) {
        if (i % 12 == 0) printf("\n    ");
        printf("%u,", nib[i]);
    }
    printf("\n};\n\n");

    printf("#define MODEL_NUM_VERTICES %u\n", (unsigned)NUM_VERTICES);
    printf("#define MODEL_NUM_EDGES    %u\n\n", (unsigned)NUM_EDGES);

    /* ── vertices, bias-packed x/y + constant z ─────────────────────────── */
    for (i = 0; i < NUM_VERTICES; i++) {
        vx[i] = (int16_t)(vquest_vertices[i].x * LOGO_SCALE * FP_ONE);
        vy[i] = (int16_t)(vquest_vertices[i].y * LOGO_SCALE * FP_ONE);
        vz[i] = (int16_t)(vquest_vertices[i].z * LOGO_SCALE * FP_ONE);
    }
    {
        int16_t xmin = vx[0], xmax = vx[0], ymin = vy[0], ymax = vy[0];
        for (i = 1; i < NUM_VERTICES; i++) {
            if (vx[i] < xmin) xmin = vx[i];
            if (vx[i] > xmax) xmax = vx[i];
            if (vy[i] < ymin) ymin = vy[i];
            if (vy[i] > ymax) ymax = vy[i];
            if (vz[i] != vz[0]) {
                fprintf(stderr, "gen_tables: model z not constant "
                                "(%d vs %d) — repack needed\n", vz[i], vz[0]);
                return 1;
            }
        }
        if (xmax - xmin >= (1 << 13) || ymax - ymin >= (1 << 11)) {
            fprintf(stderr, "gen_tables: vertex span exceeds 13/11-bit packing "
                            "(x %d..%d, y %d..%d)\n", xmin, xmax, ymin, ymax);
            return 1;
        }
        for (i = 0; i < NUM_VERTICES; i++) {
            unsigned xb = (unsigned)(vx[i] - xmin);   /* 13 bits */
            unsigned yb = (unsigned)(vy[i] - ymin);   /* 11 bits */
            vpack[i][0] = (uint8_t)(xb >> 5);
            vpack[i][1] = (uint8_t)(((xb & 31) << 3) | (yb >> 8));
            vpack[i][2] = (uint8_t)(yb & 255);
        }
        for (i = 0; i < NUM_VERTICES; i++) {   /* round-trip */
            unsigned xb = ((unsigned)vpack[i][0] << 5) | (vpack[i][1] >> 3);
            unsigned yb = ((unsigned)(vpack[i][1] & 7) << 8) | vpack[i][2];
            if ((int16_t)(xb + xmin) != vx[i] || (int16_t)(yb + ymin) != vy[i]) {
                fprintf(stderr, "gen_tables: vertex round-trip mismatch at %u\n", i);
                return 1;
            }
        }
        printf("/* vquest_vertices[] * LOGO_SCALE * FP_ONE, bias-packed: 3 bytes per\n"
               " * vertex = 13-bit (x - MODEL_X_BIAS) : 11-bit (y - MODEL_Y_BIAS);\n"
               " * z is the same for every vertex (MODEL_Z).  Decoded into RAM by\n"
               " * render.c's model_init(). */\n");
        printf("#define MODEL_X_BIAS (%d)\n", xmin);
        printf("#define MODEL_Y_BIAS (%d)\n", ymin);
        printf("#define MODEL_Z      (%d)\n", vz[0]);
        printf("static const uint8_t kModelVertsPacked[MODEL_NUM_VERTICES][3] = {");
        for (i = 0; i < NUM_VERTICES; i++) {
            if (i % 6 == 0) printf("\n    ");
            printf("{%u,%u,%u},", vpack[i][0], vpack[i][1], vpack[i][2]);
        }
        printf("\n};\n\n");
    }

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
