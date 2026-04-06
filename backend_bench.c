#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include "backend.h"
#include "stars.h"

/* Benchmark backend for Atari ST (libcmini clock() backed by TOS _hz_200).
 * Hatari reports 100 Hz OS clock → 1 tick ≈ 80,000 cycles at 8 MHz.
 * A 50 Hz frame budget = 2 ticks.
 *
 * To get sub-tick resolution we accumulate total ticks over BENCH_FRAMES
 * and report in milli-ticks (total*1000/N), giving ~80-cycle resolution.
 * Skips rendering; game logic and math run at full emulated speed.
 * Exits automatically after BENCH_FRAMES measured frames. */

#define BENCH_FRAMES  5000
#define WARMUP_FRAMES   50   /* skip first N frames (cold cache / init noise) */

static clock_t gLastPresent;
static clock_t gMinTime, gMaxTime, gTotalTime;
static int     gFrame;

void backend_draw_star(uint16_t x __attribute__((unused)),
                       uint16_t y __attribute__((unused))) {}

void backend_hud_begin(void) {}
void backend_hud_line(int16_t x0 __attribute__((unused)), int16_t y0 __attribute__((unused)),
                      int16_t x1 __attribute__((unused)), int16_t y1 __attribute__((unused))) {}
void backend_alien_begin(void) {}
void backend_alien_line(int16_t x0 __attribute__((unused)), int16_t y0 __attribute__((unused)),
                        int16_t x1 __attribute__((unused)), int16_t y1 __attribute__((unused))) {}

void backend_init(void) {
    gFrame     = 0;
    gMinTime   = (clock_t)0x7FFFFFFF;
    gMaxTime   = 0;
    gTotalTime = 0;
    gLastPresent = clock();
    stars_init();
}

void backend_clear(void) {}

void backend_draw_lines(Line *lines __attribute__((unused)),
                        int count  __attribute__((unused))) {}

void backend_present(int16_t angleY __attribute__((unused)),
                     int16_t angleX __attribute__((unused))) {
    clock_t now = clock();
    clock_t dt  = now - gLastPresent;
    gLastPresent = now;

    if (gFrame >= WARMUP_FRAMES) {
        if (dt < gMinTime) gMinTime = dt;
        if (dt > gMaxTime) gMaxTime = dt;
        gTotalTime += dt;
    }

    if (++gFrame >= BENCH_FRAMES + WARMUP_FRAMES) {
        /* milli-ticks per frame: total*1000/N gives sub-tick resolution */
        long mtpf = (long)(gTotalTime * 1000L / BENCH_FRAMES);
        /* 1 tick ≈ 80,000 cycles; 50 Hz frame budget = 2 ticks = 2000 mt */
        printf("total=%ld ticks  avg=%ld.%03ld ticks/frame  min=%ld max=%ld\n",
               (long)gTotalTime,
               mtpf / 1000, mtpf % 1000,
               (long)gMinTime, (long)gMaxTime);
        exit(0);
    }
}

void backend_cleanup(void) {}

/* Always hold UP to drive the game through takeoff → landing → SUCCESS,
 * exercising all states including the expensive logo rotation. */
uint8_t backend_get_keys(void) { return KEY_UP; }

int  backend_check_input(void)              { return 0; }
void backend_set_flash(int on __attribute__((unused))) {}
