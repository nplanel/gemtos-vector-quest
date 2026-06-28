#ifndef BACKEND_H
#define BACKEND_H

#include <stdint.h>

/* Palette — Atari ST 0xRGB format (3 bits per channel, 0-7). 4 bitplanes = 16 colours.
   Bit layout: colour index = plane3|plane2|plane1|plane0.
   Plane 0 = grid/lines (dynamic, cleared every frame)
   Plane 1 = aliens     (dynamic, cleared every frame — adjacent to plane 0 for 32-bit clear)
   Plane 2 = HUD        (semi-static, cleared on round transitions only)
   Plane 3 = stars      (draw-once at init, never cleared)
   Remote player = planes 0+1 (index 3, glowing yellow): the same triangle is
   drawn into both planes, so the regular plane 0+1 clear erases it for free.
   The takeoff/landing strip shares index 3 (its edges ride grid lines by
   design), so it renders mostly yellow too — accepted. */
#define PAL_BG    0x000  /* black       — index 0                        */
#define PAL_LINE  0x55F  /* light blue  — index 1  (plane 0, grid lines) */
#define PAL_ALIEN 0x744  /* light red   — index 2  (plane 1, aliens)     */
#define PAL_HUD   0x070  /* alien green — index 4  (plane 2, HUD)        */
#define PAL_STAR  0x555  /* grey        — index 8  (plane 3, stars)      */
#define PAL_FLASH 0x700  /* red         — crash background flash          */

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
#define KEY_FIRE  0x20

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
void    backend_draw_alien_lines(Line *lines, int count); /* draw alien/missile lines (plane 1) */
/* Draw remote-player lines into plane 0; the same lines are the tail of the
   alien batch already drawn into plane 1, so their pixels get index 3.
   Like the other draw calls, lines[count] must be the zero-sentinel. */
void    backend_draw_remote_lines(Line *lines, int count);

/* Sound slot IDs */
#define SND_INTRO    0
#define SND_MAIN     1
#define SND_FIRE     2
#define SND_GAMEOVER 3
#define SND_ENMYHIT  4

/* Sound entry points — part of the backend contract, called from vquest.c and
 * physics.c.  backend_snd_switch starts a music track and returns its length in
 * game frames; backend_snd_sfx triggers a one-shot effect. */
uint16_t backend_snd_switch(int slot);
void     backend_snd_sfx(int slot);


#endif /* BACKEND_H */
