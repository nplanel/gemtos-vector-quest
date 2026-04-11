/*
 * credits.c — Credits screen layout.
 *
 * Two-column layout (scale sx=sy=2, char step=11px, space extra=5px):
 *   col1: CODE(41) / SOUND(52)  → col1_width = 52px
 *   col2: BENOU/PUMP(117) / CYBERIC(74) → col2_width = 117px
 *   col_gap = 16px; total = 52+16+117 = 185px
 *   col1_x = (320−185)/2 = 68;  col2_x = 68+52+16 = 136
 *   block height = 14+6+14 = 34px; row1_y = (200−34)/2 = 83; row2_y = 103
 */

#include "credits.h"
#include "draw.h"

#define SX 2
#define SY 2
#define STEP 11
#define SPACE_EXTRA 5

void credits_render(void) {
    static const int16_t col1_x = 68, col2_x = 136;
    static const int16_t row1_y = 83, row2_y = 103;
    int16_t x;

    /* row 1, col 1: CODE */
    x = col1_x;
    font_draw(seg_C, x, row1_y, SX, SY); x += STEP;
    font_draw(seg_O, x, row1_y, SX, SY); x += STEP;
    font_draw(seg_D, x, row1_y, SX, SY); x += STEP;
    font_draw(seg_E, x, row1_y, SX, SY);

    /* row 1, col 2: BENOU / PUMP */
    x = col2_x;
    font_draw(seg_B, x, row1_y, SX, SY); x += STEP;
    font_draw(seg_E, x, row1_y, SX, SY); x += STEP;
    font_draw(seg_N, x, row1_y, SX, SY); x += STEP;
    font_draw(seg_O, x, row1_y, SX, SY); x += STEP;
    font_draw(seg_U, x, row1_y, SX, SY); x += STEP + SPACE_EXTRA;
    font_draw(seg_slash, x, row1_y, SX, SY); x += STEP + SPACE_EXTRA;
    font_draw(seg_P, x, row1_y, SX, SY); x += STEP;
    font_draw(seg_U, x, row1_y, SX, SY); x += STEP;
    font_draw(seg_M, x, row1_y, SX, SY); x += STEP;
    font_draw(seg_P, x, row1_y, SX, SY);

    /* row 2, col 1: SOUND */
    x = col1_x;
    font_draw(seg_S, x, row2_y, SX, SY); x += STEP;
    font_draw(seg_O, x, row2_y, SX, SY); x += STEP;
    font_draw(seg_U, x, row2_y, SX, SY); x += STEP;
    font_draw(seg_N, x, row2_y, SX, SY); x += STEP;
    font_draw(seg_D, x, row2_y, SX, SY);

    /* row 2, col 2: CYBERIC */
    x = col2_x;
    font_draw(seg_C, x, row2_y, SX, SY); x += STEP;
    font_draw(seg_Y, x, row2_y, SX, SY); x += STEP;
    font_draw(seg_B, x, row2_y, SX, SY); x += STEP;
    font_draw(seg_E, x, row2_y, SX, SY); x += STEP;
    font_draw(seg_R, x, row2_y, SX, SY); x += STEP;
    font_draw(seg_I, x, row2_y, SX, SY); x += STEP;
    font_draw(seg_C, x, row2_y, SX, SY);
}
