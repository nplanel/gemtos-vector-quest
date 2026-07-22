#ifndef NARROW_H
#define NARROW_H

/* Explicit 16-bit narrowing, checked on the debug builds.
 *
 * The Atari target is built -mshort, so `int` is 16 bits and every integer
 * promotion of an int16_t operand lands back in 16 bits: `a - b - c` wraps
 * silently at 32768.  The Linux debug builds have a 32-bit int and compute
 * the same expression exactly, so an overflow that corrupts the ST picture is
 * invisible in vq-ascii / vq-sdl.
 *
 * S16()/U16() close that gap.  On m68k they are a plain cast — the expression
 * is already evaluated in 16 bits, so there is nothing to check and nothing to
 * pay.  Everywhere else the expression is evaluated in the host's wider int
 * and S16() asserts the result would have fit; if the assert never fires under
 * `make test`, the ST computes the same value.
 *
 * S16  — signed narrowing that must NOT lose information.  Use for screen
 *        coordinates, world positions, speeds: anything where a wrap is a bug.
 * U16W — unsigned narrowing that MAY wrap by design (the modular course
 *        counters: progress, next_alien_pos, alien_seq, the LCG).  Documents
 *        the intent and is never checked.
 */

#include <stdint.h>
#include <assert.h>

#ifdef __m68k__

#define S16(expr)  ((int16_t)(expr))

#else

static inline int16_t narrow_s16(long v) {
    assert(v >= -32768L && v <= 32767L);
    return (int16_t)v;
}
#define S16(expr)  narrow_s16((long)(expr))

#endif

#define U16W(expr) ((uint16_t)(expr))

#endif /* NARROW_H */
