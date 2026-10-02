/*
 * credits.c — Credits screen layout.
 *
 * Two columns: the role at x=68, the names at x=136.  Rows 83/103 at the
 * medium font (sx=sy=2, 11-px step), then the THANKS list at the small font
 * (sx=sy=1, 6-px step) from y=133 on 10-px rows.
 */

#include <stdint.h>

#define SPACE_EXTRA 5
#define CRED_COL1 68
#define CRED_COL2 136

typedef struct {
    const char *s;
    int16_t     x;
    uint8_t     y;
    bool        med;   /* medium font, else small */
} CreditText;

/* Table-driven: one draw_text call site instead of one per string. */
static const CreditText kCredits[] = {
    { "CODE",       CRED_COL1, 83,  true  },
    { "BENOU",      CRED_COL2, 83,  true  },
    /* × has SPACE_EXTRA padding on each side instead of the normal step */
    { "PUMP",       CRED_COL2 + 6 * FONT_MED_STEP + 2 * SPACE_EXTRA, 83, true },
    { "SOUND",      CRED_COL1, 103, true  },
    { "CYBERIC",    CRED_COL2, 103, true  },
    { "THANKS",     CRED_COL1, 133, false },
    { "KALMALYZER", CRED_COL2, 133, false },
    { "LEONARD",    CRED_COL2, 143, false },
    { "ANTHROPIC",  CRED_COL2, 153, false },
    { "FREEMINT",   CRED_COL2, 163, false },
};

static void credits_render(void) {
    const CreditText *c;
    for (c = kCredits; c < kCredits + sizeof(kCredits) / sizeof(kCredits[0]); c++) {
        if (c->med)
            draw_text(c->s, c->x, c->y, FONT_MED_SX, FONT_MED_SY, FONT_MED_STEP, 0);
        else
            draw_text(c->s, c->x, c->y, FONT_SML_SX, FONT_SML_SY, FONT_SML_STEP, 0);
    }
    font_draw(seg_times, CRED_COL2 + 5 * FONT_MED_STEP + SPACE_EXTRA, 83,
              FONT_MED_SX, FONT_MED_SY);
}
