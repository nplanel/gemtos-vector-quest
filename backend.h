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

void backend_init(void);
void backend_clear(void);
void backend_draw_lines(Line *lines, int count);
void backend_present(int16_t angleY, int16_t angleX);
void backend_cleanup(void);
int  backend_check_input(void);

#endif /* BACKEND_H */
