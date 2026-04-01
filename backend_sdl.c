#include <SDL2/SDL.h>
#include "backend.h"

static SDL_Window   *gWindow;
static SDL_Renderer *gRenderer;

void backend_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return;
    }
    gWindow = SDL_CreateWindow("GemTOS Vector Quest",
                               SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED,
                               SCREEN_WIDTH, SCREEN_HEIGHT,
                               SDL_WINDOW_SHOWN);
    if (!gWindow) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return;
    }
    gRenderer = SDL_CreateRenderer(gWindow, -1,
                                   SDL_RENDERER_ACCELERATED |
                                   SDL_RENDERER_PRESENTVSYNC);
    if (!gRenderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(gWindow);
        SDL_Quit();
        return;
    }
}

static int gFlash;

void backend_set_flash(int on) { gFlash = on; }

void backend_clear(void) {
    if (gFlash) {
        SDL_SetRenderDrawColor(gRenderer, 119, 0, 0, 255); /* red bg */
        SDL_RenderClear(gRenderer);
        SDL_SetRenderDrawColor(gRenderer, 0, 0, 119, 255); /* blue fg */
    } else {
        SDL_SetRenderDrawColor(gRenderer, 0, 0, 119, 255); /* dark blue bg */
        SDL_RenderClear(gRenderer);
        SDL_SetRenderDrawColor(gRenderer, 119, 0, 0, 255); /* red fg */
    }
}

void backend_draw_lines(Line *lines, int count) {
    int i;
    for (i = 0; i < count; ++i) {
        SDL_RenderDrawLine(gRenderer,
                           lines[i].p0.x, lines[i].p0.y,
                           lines[i].p1.x, lines[i].p1.y);
    }
}

void backend_present(int16_t angleY __attribute__((unused)),
                     int16_t angleX __attribute__((unused))) {
    SDL_RenderPresent(gRenderer);
}

void backend_cleanup(void) {
    if (gRenderer) SDL_DestroyRenderer(gRenderer);
    if (gWindow)   SDL_DestroyWindow(gWindow);
    SDL_Quit();
}

uint8_t backend_get_keys(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT) return KEY_QUIT;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint8_t m = 0;
    if (ks[SDL_SCANCODE_ESCAPE] || ks[SDL_SCANCODE_SPACE]) m |= KEY_QUIT;
    if (ks[SDL_SCANCODE_UP])    m |= KEY_UP;
    if (ks[SDL_SCANCODE_DOWN])  m |= KEY_DOWN;
    if (ks[SDL_SCANCODE_LEFT])  m |= KEY_LEFT;
    if (ks[SDL_SCANCODE_RIGHT]) m |= KEY_RIGHT;
    return m;
}

int backend_check_input(void) { return (backend_get_keys() & KEY_QUIT) != 0; }
