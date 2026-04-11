#ifndef HUD_H
#define HUD_H

#include <stdint.h>

#define HUD_NCHARS         12  /* characters in "VECTOR QUEST" (letters + space) */
#define HUD_NCHARS_VIS     11  /* non-space characters in "VECTOR QUEST"         */
#define HUD_NSUB_CHARS     19  /* characters in "GEMTOS 2026 EDITION"            */
/* Max backend_hud_line() calls in one full hud_draw(): title(55)+subtitle(89)+tally(9) */
#define HUD_MAX_LINES  153

void hud_begin(void);                /* clear HUD plane only */
int  hud_draw_letter(int8_t i);      /* draw i-th title char; returns 1 if drawn, 0 if space */
int  hud_draw_subletter(int8_t i);   /* draw i-th subtitle char; returns 1 if drawn, 0 if space */
void hud_draw_tally(int round);      /* draw tally marks only (no clear) */
void hud_draw(int round);            /* begin + all letters + subtitle + tally */

#endif /* HUD_H */
