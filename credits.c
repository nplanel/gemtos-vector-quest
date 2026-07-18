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
    const int16_t col1_x = 68, col2_x = 136;
    int16_t row_y = 83;
    draw_text("CODE",     col1_x, row_y, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 0);
    draw_text("BENOU",    col2_x, row_y, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 0);
    /* × has SPACE_EXTRA padding on each side instead of the normal FONT_MED_STEP gap */
    font_draw(seg_times, col2_x + 5*FONT_MED_STEP + SPACE_EXTRA, row_y, FONT_MED_SX, FONT_MED_SY);
    draw_text("PUMP",     col2_x + 5*FONT_MED_STEP + 2*SPACE_EXTRA + FONT_MED_STEP,
                                     row_y, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 0);

    row_y += 20;
    draw_text("SOUND",    col1_x, row_y, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 0);
    draw_text("CYBERIC",  col2_x, row_y, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 0);

    row_y += 30;
    draw_text("THANKS",     col1_x, row_y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
    draw_text("KALMALYZER", col2_x, row_y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
    row_y += 10;
    draw_text("LEONARD",    col2_x, row_y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
    row_y += 10;
    draw_text("ANTHROPIC",  col2_x, row_y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
    row_y += 10;
    draw_text("FREEMINT",   col2_x, row_y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
}
