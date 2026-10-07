#ifndef MODEL_SRC_H
#define MODEL_SRC_H

/* Host-only: the float source model, read by gen_tables.c (via dna_helix.h).
 * The game binary never sees it — only the baked int16 tables in
 * gen_tables.h — so it stays out of vquest.h. */
typedef struct {
    float x, y, z;
} Point3DFloat;

#define LOGO_SCALE (2.0f/230.0f)   /* model units → world units */

#endif /* MODEL_SRC_H */
