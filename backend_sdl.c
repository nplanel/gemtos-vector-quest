#include <SDL2/SDL.h>
#include <assert.h>
#include "backend.h"

static SDL_Window   *gWindow;
static SDL_Renderer *gRenderer;

static uint16_t gStarX[NSTARS], gStarY[NSTARS];
static uint8_t  gNStars = 0;
static SDL_Texture *gStarTexture;   /* stars baked once at init — plane 3 semantics */
static Line     gHudLines[MAX_DRAW_LINES];
static uint16_t gNHudLines = 0;

static SDL_Joystick *gJoystick;

static int     gFlash;
static uint8_t gGlowFrame;

static const uint16_t kGlowAlien[16] = {
    0x411, 0x522, 0x522, 0x633, 0x744, 0x755, 0x755, 0x766,
    0x766, 0x755, 0x755, 0x744, 0x633, 0x522, 0x522, 0x411
};
static const uint16_t kGlowStar[16]  = {
    0x222, 0x333, 0x333, 0x444, 0x555, 0x555, 0x666, 0x666,
    0x666, 0x666, 0x555, 0x555, 0x444, 0x333, 0x333, 0x222
};

static uint16_t sdl_glow_alien(void) { return kGlowAlien[(gGlowFrame >> 1) & 15]; }
static uint16_t sdl_glow_star(void)  { return kGlowStar [(gGlowFrame >> 2) & 15]; }

void backend_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return;
    }
    gWindow = SDL_CreateWindow("GemTOS Vector Quest",
                               SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED,
                               SCREEN_WIDTH * 2, SCREEN_HEIGHT * 2,
                               SDL_WINDOW_SHOWN);
    if (!gWindow) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return;
    }
    /* Linear filtering: bilinear interpolation when scaling up the 320×200
     * logical surface — smoother than nearest-neighbour at non-integer scales. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    gRenderer = SDL_CreateRenderer(gWindow, -1,
                                   SDL_RENDERER_ACCELERATED |
                                   SDL_RENDERER_PRESENTVSYNC);
    if (!gRenderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(gWindow);
        SDL_Quit();
        return;
    }
    /* Logical size keeps all rendering in 320×200 coordinates regardless of
     * window size; SDL scales and letterboxes automatically on each present. */
    SDL_RenderSetLogicalSize(gRenderer, SCREEN_WIDTH, SCREEN_HEIGHT);
    stars_init();
    if (SDL_NumJoysticks() > 0)
        gJoystick = SDL_JoystickOpen(0);
    /* Bake stars into a texture once — mirrors Atari plane 3 draw-once semantics. */
    gStarTexture = SDL_CreateTexture(gRenderer, SDL_PIXELFORMAT_RGBA8888,
                                     SDL_TEXTUREACCESS_TARGET,
                                     SCREEN_WIDTH, SCREEN_HEIGHT);
    SDL_SetTextureBlendMode(gStarTexture, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(gRenderer, gStarTexture);
    SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 0);  /* transparent background */
    SDL_RenderClear(gRenderer);
    /* Draw stars white so SDL_SetTextureColorMod can tint them to any shade. */
    SDL_SetRenderDrawColor(gRenderer, 255, 255, 255, 255);
    {
        uint8_t si;
        for (si = 0; si < gNStars; si++)
            SDL_RenderDrawPoint(gRenderer, gStarX[si], gStarY[si]);
    }
    SDL_SetRenderTarget(gRenderer, NULL);
}

void backend_set_flash(int on) { gFlash = on; }


void backend_draw_star(uint16_t x, uint16_t y) {
    assert(gNStars < NSTARS);
    gStarX[gNStars] = x;
    gStarY[gNStars] = y;
    gNStars++;
}

void backend_hud_begin(void) { gNHudLines = 0; }

/* Draw immediately (mirrors Atari plane-2 draw-to-both-buffers semantics) and
 * store so backend_clear() can re-composite HUD with the background each frame. */
void backend_hud_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    assert(gNHudLines < MAX_DRAW_LINES);
    gHudLines[gNHudLines].p0.x = x0;
    gHudLines[gNHudLines].p0.y = y0;
    gHudLines[gNHudLines].p1.x = x1;
    gHudLines[gNHudLines].p1.y = y1;
    gNHudLines++;
    SDL_SetRenderDrawColor(gRenderer, PAL_R(PAL_HUD), PAL_G(PAL_HUD), PAL_B(PAL_HUD), 255);
    SDL_RenderDrawLine(gRenderer, x0, y0, x1, y1);
}

void backend_clear(void) {
    uint16_t i;
    /* Plane bg: clear to background (or crash flash red) */
    {
        int bg = gFlash ? PAL_FLASH : PAL_BG;
        SDL_SetRenderDrawColor(gRenderer, PAL_R(bg), PAL_G(bg), PAL_B(bg), 255);
        SDL_RenderClear(gRenderer);
    }
    /* Plane 3: stars — blit pre-baked texture with current glow tint */
    {
        uint16_t sc = sdl_glow_star();
        SDL_SetTextureColorMod(gStarTexture, PAL_R(sc), PAL_G(sc), PAL_B(sc));
    }
    SDL_RenderCopy(gRenderer, gStarTexture, NULL, NULL);
    /* Plane 2: HUD lines (redrawn only on transitions) */
    SDL_SetRenderDrawColor(gRenderer, PAL_R(PAL_HUD), PAL_G(PAL_HUD), PAL_B(PAL_HUD), 255);
    for (i = 0; i < gNHudLines; i++)
        SDL_RenderDrawLine(gRenderer,
                           gHudLines[i].p0.x, gHudLines[i].p0.y,
                           gHudLines[i].p1.x, gHudLines[i].p1.y);
    /* Plane 0: set colour for backend_draw_lines() that follows */
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

/* Plane 1: alien/missile lines drawn after grid (plane 0) so they appear on top. */
void backend_draw_alien_lines(Line *lines, int count) {
    uint16_t i;
    {
        uint16_t ac = sdl_glow_alien();
        SDL_SetRenderDrawColor(gRenderer, PAL_R(ac), PAL_G(ac), PAL_B(ac), 255);
    }
    for (i = 0; i < (uint16_t)count; i++)
        SDL_RenderDrawLine(gRenderer,
                           lines[i].p0.x, lines[i].p0.y,
                           lines[i].p1.x, lines[i].p1.y);
}

void backend_present(int16_t angleY __attribute__((unused)),
                     int16_t angleX __attribute__((unused))) {
    gGlowFrame++;
    SDL_RenderPresent(gRenderer);
}

void backend_cleanup(void) {
    if (gJoystick)    { SDL_JoystickClose(gJoystick); gJoystick = NULL; }
    if (gStarTexture) SDL_DestroyTexture(gStarTexture);
    if (gRenderer)    SDL_DestroyRenderer(gRenderer);
    if (gWindow)      SDL_DestroyWindow(gWindow);
    SDL_Quit();
}

uint8_t backend_get_keys(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT) return KEY_QUIT;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint8_t m = 0;
    if (ks[SDL_SCANCODE_ESCAPE]) m |= KEY_QUIT;
    if (ks[SDL_SCANCODE_SPACE])  m |= KEY_FIRE;
    if (ks[SDL_SCANCODE_UP])    m |= KEY_UP;
    if (ks[SDL_SCANCODE_DOWN])  m |= KEY_DOWN;
    if (ks[SDL_SCANCODE_LEFT])  m |= KEY_LEFT;
    if (ks[SDL_SCANCODE_RIGHT]) m |= KEY_RIGHT;
    if (gJoystick) {
        const int DEAD = 8192;  /* 25% of ±32767 */
        Sint16 ax = SDL_JoystickGetAxis(gJoystick, 0);
        Sint16 ay = SDL_JoystickGetAxis(gJoystick, 1);
        if (ax < -DEAD) m |= KEY_LEFT;
        if (ax >  DEAD) m |= KEY_RIGHT;
        if (ay < -DEAD) m |= KEY_UP;    /* pull back  → nose up   (aviation) */
        if (ay >  DEAD) m |= KEY_DOWN;  /* push fwd   → nose down (aviation) */
        if (SDL_JoystickGetButton(gJoystick, 0)) m |= KEY_FIRE;
    }
    return m;
}

int backend_check_input(void) { return (backend_get_keys() & KEY_QUIT) != 0; }

static void backend_snd_switch(int slot) { (void)slot; }
static void backend_snd_sfx(int slot)    { (void)slot; }
