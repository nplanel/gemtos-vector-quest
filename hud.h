#ifndef HUD_H
#define HUD_H

#include <stdint.h>

#define HUD_NCHARS 12  /* characters in "VECTOR QUEST" (letters + space) */

void hud_begin(void);               /* clear HUD plane only */
void hud_draw_letter(int8_t i);     /* draw i-th title character (no clear) */
void hud_draw_tally(int round);     /* draw tally marks only (no clear) */
void hud_draw(int round);           /* begin + all letters + tally */

#endif /* HUD_H */
