/*
 * credits.c — Credits screen layout.
 *
 * Two-column layout (scale sx=sy=2, char step=11px, space extra=5px):
 *   col1: THANKS(63) → col1_width = 63px
 *   col2: BENOU×PUMP(117) → col2_width = 117px
 *   col_gap = 16px; total = 63+16+117 = 196px
 *   col1_x = (320−196)/2 = 62;  col2_x = 62+63+16 = 141
 *   block height = 4×14+3×6 = 74px; row1_y = (200−74)/2 = 63
 *   rows: 83 / 103 / 123 / 143
 */

#include <stdint.h>

#define CREDITS_SX 2
#define CREDITS_SY 2
#define STEP 11
#define SPACE_EXTRA 5
#define CREDITS_ROW_SY 20

static void credits_render(void) {
    const int16_t col1_x = 62, col2_x = 141;
    const int16_t row1_y = 83,
          row2_y = row1_y + CREDITS_ROW_SY,
          row3_y = row2_y + CREDITS_ROW_SY,
          row4_y = row3_y + CREDITS_ROW_SY;

    static const Seg * const kCode[]       = { seg_C, seg_O, seg_D, seg_E };
    static const Seg * const kBenou[]      = { seg_B, seg_E, seg_N, seg_O, seg_U };
    static const Seg * const kPump[]       = { seg_P, seg_U, seg_M, seg_P };
    static const Seg * const kSound[]      = { seg_S, seg_O, seg_U, seg_N, seg_D };
    static const Seg * const kCyberic[]    = { seg_C, seg_Y, seg_B, seg_E, seg_R, seg_I, seg_C };
    static const Seg * const kThanks[]     = { seg_T, seg_H, seg_A, seg_N, seg_K, seg_S };
    static const Seg * const kKalmalyzer[] = { seg_K, seg_A, seg_L, seg_M, seg_A,
                                               seg_L, seg_Y, seg_Z, seg_E, seg_R };
    static const Seg * const kAnthropic[]  = { seg_A, seg_N, seg_T, seg_H, seg_R,
                                               seg_O, seg_P, seg_I, seg_C };

    draw_seg_string(kCode,       4,  col1_x, row1_y, CREDITS_SX, CREDITS_SY, STEP, 0);
    draw_seg_string(kBenou,      5,  col2_x, row1_y, CREDITS_SX, CREDITS_SY, STEP, 0);
    /* × has SPACE_EXTRA padding on each side instead of the normal STEP gap */
    font_draw(seg_times, col2_x + 5*STEP + SPACE_EXTRA, row1_y, CREDITS_SX, CREDITS_SY);
    draw_seg_string(kPump,       4,  col2_x + 5*STEP + 2*SPACE_EXTRA + STEP,
                                     row1_y, CREDITS_SX, CREDITS_SY, STEP, 0);

    draw_seg_string(kSound,      5,  col1_x, row2_y, CREDITS_SX, CREDITS_SY, STEP, 0);
    draw_seg_string(kCyberic,    7,  col2_x, row2_y, CREDITS_SX, CREDITS_SY, STEP, 0);
    draw_seg_string(kThanks,     6,  col1_x, row3_y, CREDITS_SX, CREDITS_SY, STEP, 0);
    draw_seg_string(kKalmalyzer, 10, col2_x, row3_y, CREDITS_SX, CREDITS_SY, STEP, 0);
    draw_seg_string(kAnthropic,  9,  col2_x, row4_y, CREDITS_SX, CREDITS_SY, STEP, 0);
}
