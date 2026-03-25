#ifndef BACKEND_H
#define BACKEND_H

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 200

typedef struct {
    short x, y;
} Point2D;

typedef struct {
    Point2D p0;
    Point2D p1;
} Line;

void backend_init(void);
void backend_clear(void);
void backend_draw_lines(Line *lines, int count);
void backend_present(short angleY, short angleX);
void backend_cleanup(void);
int  backend_check_input(void);

#endif /* BACKEND_H */
