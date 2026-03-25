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

void backend_clear(void) {
    /* background: dark blue like the Atari palette (0x007 = RGB ~0,0,119) */
    SDL_SetRenderDrawColor(gRenderer, 0, 0, 119, 255);
    SDL_RenderClear(gRenderer);
    /* set foreground color for lines: red (0x700 = RGB ~119,0,0) */
    SDL_SetRenderDrawColor(gRenderer, 119, 0, 0, 255);
}

void backend_draw_lines(Line *lines, int count) {
    int i;
    for (i = 0; i < count; ++i) {
        SDL_RenderDrawLine(gRenderer,
                           lines[i].p0.x, lines[i].p0.y,
                           lines[i].p1.x, lines[i].p1.y);
    }
}

void backend_present(short angleY __attribute__((unused)),
                     short angleX __attribute__((unused))) {
    SDL_RenderPresent(gRenderer);
}

void backend_cleanup(void) {
    if (gRenderer) SDL_DestroyRenderer(gRenderer);
    if (gWindow)   SDL_DestroyWindow(gWindow);
    SDL_Quit();
}

int backend_check_input(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            return 1;
        if (e.type == SDL_KEYDOWN &&
            (e.key.keysym.sym == SDLK_ESCAPE ||
             e.key.keysym.sym == SDLK_SPACE))
            return 1;
    }
    return 0;
}
