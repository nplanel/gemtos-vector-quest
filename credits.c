/*
 * credits.c — Credits screen layout.
 *
 * Two columns: the role at x=68, the names at x=136.  Rows 83/103 at the
 * medium font (x2, 11-px step), then the THANKS list at the small font
 * (x1, 6-px step) from y=133 on 10-px rows.
 */

#include <stdint.h>

#define SPACE_EXTRA 5
#define CRED_COL1 68
#define CRED_COL2 136

typedef struct {
    const char *s;
    int16_t     x;
    uint8_t     y;
    const Font *font;
} CreditText;

/* Table-driven: one draw_text call site instead of one per string. */
static const CreditText kCredits[] = {
    { "CODE",       CRED_COL1, 83, &kFontMed },
    { "BENOU",      CRED_COL2, 83, &kFontMed },
    /* × has SPACE_EXTRA padding on each side instead of the normal step */
    { "PUMP",       CRED_COL2 + 6 * FONT_MED_STEP + 2 * SPACE_EXTRA, 83, &kFontMed },
    { "SOUND",      CRED_COL1, 103, &kFontMed },
    { "CYBERIC",    CRED_COL2, 103, &kFontMed },
    { "THANKS",     CRED_COL1, 133, &kFontSml },
    { "KALMALYZER", CRED_COL2, 133, &kFontSml },
    { "LEONARD",    CRED_COL2, 143, &kFontSml },
    { "ANTHROPIC",  CRED_COL2, 153, &kFontSml },
    { "FREEMINT",   CRED_COL2, 163, &kFontSml },
};

static void credits_render(void) {
    const CreditText *c;
    for (c = kCredits; c < kCredits + sizeof(kCredits) / sizeof(kCredits[0]); c++) {
        draw_text(c->s, c->x, c->y, c->font);
    }
    font_draw(seg_times, CRED_COL2 + 5 * FONT_MED_STEP + SPACE_EXTRA, 83, &kFontMed);
}
