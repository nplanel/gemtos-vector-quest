#ifndef BACKEND_H
#define BACKEND_H

#include <stdint.h>

/* Palette — Atari ST 0xRGB format (3 bits per channel, 0-7).
   Bit layout: color index = plane3|plane2|plane1|plane0.
   Index 0 = background, 1 = plane 0 (lines), 2 = plane 1 (stars), 3 = planes 0+1,
             4 = plane 2 (HUD),   5 = planes 0+2,  6 = planes 1+2, 7 = planes 0+1+2. */
#define PAL_BG      0x000  /* black           — index 0 */
#define PAL_LINE    0x55F  /* light blue      — index 1  (plane 0)     */
#define PAL_STAR    0x555  /* medium grey     — index 2  (plane 1)     */
#define PAL_BLEND   0x55F  /* line over star  — index 3  (planes 0+1)  */
#define PAL_HUD     0x070  /* alien green     — index 4  (plane 2)     */
#define PAL_MIX_02  PAL_LINE  /* line over HUD   — index 5  (planes 0+2)  */
#define PAL_MIX_12  PAL_HUD   /* star over HUD   — index 6  (planes 1+2)  */
#define PAL_MIX_012 PAL_LINE  /* all three       — index 7  (planes 0+1+2) */
#define PAL_FLASH   0x700     /* red             — crash background flash   */

/* Convert a PAL_* constant to an 8-bit channel value (for SDL etc.). */
static inline uint8_t pal_component(int st_color, int shift) {
    return (uint8_t)(((st_color >> shift) & 7) * 255 / 7);
}
#define PAL_R(c) pal_component((c), 8)
#define PAL_G(c) pal_component((c), 4)
#define PAL_B(c) pal_component((c), 0)

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 200

typedef struct {
    int16_t x, y;
} Point2D;

typedef struct {
    Point2D p0;
    Point2D p1;
} Line;

/* Key bitmask bits for backend_get_keys() */
#define KEY_UP    0x01
#define KEY_DOWN  0x02
#define KEY_LEFT  0x04
#define KEY_RIGHT 0x08
#define KEY_QUIT  0x10

void    backend_init(void);
void    backend_clear(void);
void    backend_draw_lines(Line *lines, int count);
void    backend_draw_star(uint16_t x, uint16_t y); /* called by stars_init() */
void    backend_hud_begin(void);                   /* clear HUD plane; called by hud_draw() */
void    backend_hud_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1); /* draw into HUD plane */
void    backend_present(int16_t angleY, int16_t angleX);
void    backend_cleanup(void);
int     backend_check_input(void);
uint8_t backend_get_keys(void);    /* bitmask of held keys this frame  */
void    backend_set_flash(int on); /* 1 = invert bg/fg for crash flash */

#endif /* BACKEND_H */
