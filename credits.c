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

#define SPACE_EXTRA 5

static void credits_render(void) {
    const Seg * const kCode[]       = { seg_C, seg_O, seg_D, seg_E };
    const Seg * const kBenou[]      = { seg_B, seg_E, seg_N, seg_O, seg_U };
    const Seg * const kPump[]       = { seg_P, seg_U, seg_M, seg_P };
    const Seg * const kSound[]      = { seg_S, seg_O, seg_U, seg_N, seg_D };
    const Seg * const kCyberic[]    = { seg_C, seg_Y, seg_B, seg_E, seg_R, seg_I, seg_C };
    const Seg * const kThanks[]     = { seg_T, seg_H, seg_A, seg_N, seg_K, seg_S };
    const Seg * const kKalmalyzer[] = { seg_K, seg_A, seg_L, seg_M, seg_A,
                                        seg_L, seg_Y, seg_Z, seg_E, seg_R };
    const Seg * const kLeonard[]    = { seg_L, seg_E, seg_O, seg_N, seg_A,
                                        seg_R, seg_D  };
    const Seg * const kAnthropic[]  = { seg_A, seg_N, seg_T, seg_H, seg_R,
                                        seg_O, seg_P, seg_I, seg_C };
    const Seg * const kFreemint[]   = { seg_F, seg_R, seg_E, seg_E, seg_M,
                                        seg_I, seg_N, seg_T };

    const int16_t col1_x = 68, col2_x = 136;
    int16_t row_y = 83;
    draw_seg_array(kCode,       col1_x, row_y, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 0);
    draw_seg_array(kBenou,      col2_x, row_y, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 0);
    /* × has SPACE_EXTRA padding on each side instead of the normal FONT_MED_STEP gap */
    font_draw(seg_times, col2_x + 5*FONT_MED_STEP + SPACE_EXTRA, row_y, FONT_MED_SX, FONT_MED_SY);
    draw_seg_array(kPump,       col2_x + 5*FONT_MED_STEP + 2*SPACE_EXTRA + FONT_MED_STEP,
                                     row_y, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 0);

    row_y += 20;
    draw_seg_array(kSound,      col1_x, row_y, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 0);
    draw_seg_array(kCyberic,    col2_x, row_y, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 0);

    row_y += 30;
    draw_seg_array(kThanks,     col1_x, row_y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
    draw_seg_array(kKalmalyzer, col2_x, row_y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
    row_y += 10;
    draw_seg_array(kLeonard,    col2_x, row_y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
    row_y += 10;
    draw_seg_array(kAnthropic,  col2_x, row_y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
    row_y += 10;
    draw_seg_array(kFreemint,   col2_x, row_y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
}
