#ifndef BACKEND_H
#define BACKEND_H

#include <stdint.h>

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
void    backend_present(int16_t angleY, int16_t angleX);
void    backend_cleanup(void);
int     backend_check_input(void);
uint8_t backend_get_keys(void);    /* bitmask of held keys this frame  */
void    backend_set_flash(int on); /* 1 = invert bg/fg for crash flash */

#endif /* BACKEND_H */
