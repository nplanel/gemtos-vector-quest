#include <stdint.h>
#include "backend.h"

#define NSTARS 96

static void stars_init(void) {
    uint16_t rng = 0xACE1u;
    int i;
    for (i = 0; i < NSTARS; i++) {
        rng = (uint16_t)((rng >> 1) ^ (uint16_t)(-(rng & 1u) & 0xB400u));
        uint16_t x = (uint16_t)(rng % SCREEN_WIDTH);
        rng = (uint16_t)((rng >> 1) ^ (uint16_t)(-(rng & 1u) & 0xB400u));
        uint16_t y = (uint16_t)(rng % SCREEN_HEIGHT);
        backend_draw_star(x, y);
    }
}
