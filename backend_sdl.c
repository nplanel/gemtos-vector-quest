#include <SDL2/SDL.h>
#include "backend.h"
#include "stars.h"

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
    stars_init();
}

static uint16_t gStarX[NSTARS], gStarY[NSTARS];
static uint8_t  gNStars = 0;

#define MAX_HUD_LINES 256
static Line     gHudLines[MAX_HUD_LINES];
static uint16_t gNHudLines = 0;

static int gFlash;

void backend_set_flash(int on) { gFlash = on; }

void backend_draw_star(uint16_t x, uint16_t y) {
    gStarX[gNStars] = x;
    gStarY[gNStars] = y;
    gNStars++;
}

void backend_hud_begin(void) { gNHudLines = 0; }

void backend_hud_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    if (gNHudLines < MAX_HUD_LINES) {
        gHudLines[gNHudLines].p0.x = x0;
        gHudLines[gNHudLines].p0.y = y0;
        gHudLines[gNHudLines].p1.x = x1;
        gHudLines[gNHudLines].p1.y = y1;
        gNHudLines++;
    }
}

void backend_clear(void) {
    uint8_t  si;
    uint16_t hi;
    {
        int bg = gFlash ? PAL_FLASH : PAL_BG;
        SDL_SetRenderDrawColor(gRenderer, PAL_R(bg), PAL_G(bg), PAL_B(bg), 255);
        SDL_RenderClear(gRenderer);
        SDL_SetRenderDrawColor(gRenderer, PAL_R(PAL_LINE), PAL_G(PAL_LINE), PAL_B(PAL_LINE), 255);
    }
    SDL_SetRenderDrawColor(gRenderer, PAL_R(PAL_STAR), PAL_G(PAL_STAR), PAL_B(PAL_STAR), 255);
    for (si = 0; si < gNStars; si++)
        SDL_RenderDrawPoint(gRenderer, gStarX[si], gStarY[si]);
    SDL_SetRenderDrawColor(gRenderer, PAL_R(PAL_HUD), PAL_G(PAL_HUD), PAL_B(PAL_HUD), 255);
    for (hi = 0; hi < gNHudLines; hi++)
        SDL_RenderDrawLine(gRenderer,
                           gHudLines[hi].p0.x, gHudLines[hi].p0.y,
                           gHudLines[hi].p1.x, gHudLines[hi].p1.y);
    SDL_SetRenderDrawColor(gRenderer, PAL_R(PAL_LINE), PAL_G(PAL_LINE), PAL_B(PAL_LINE), 255);
}

void backend_draw_lines(Line *lines, int count) {
    uint16_t i;
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
