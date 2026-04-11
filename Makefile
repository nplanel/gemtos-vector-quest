
CC_ATARI  = m68k-atari-mint-gcc
CC_LINUX  = gcc
VASM      = vasm

LIBCMINI_DIR = /home/ben/src/atari/libcmini
LIBCMINI     = $(LIBCMINI_DIR)/build/mshort
CRT0         = $(LIBCMINI)/objs/crt0.o

VASMFLAGS = -Faout -quiet -x -m68000 -spaces -showopt

# ── Optimisation level — comment/uncomment to switch ───────────────────────────
OPT = -Ofast
#OPT = -Og

# ── Debug / assert control — comment out for release ───────────────────────────
NDEBUG = -DNDEBUG

# ── Flags common to both targets ───────────────────────────────────────────────
CFLAGS_COMMON = $(OPT) $(NDEBUG) -Wall -Wextra -Werror -g -std=gnu99

CFLAGS_ATARI  = $(CFLAGS_COMMON) -mshort -nostdlib -I$(LIBCMINI_DIR)/include
CFLAGS_LINUX  = $(CFLAGS_COMMON) -fsanitize=address,undefined

LDFLAGS_ATARI = -L$(LIBCMINI) -lcmini -lgcc -lcmini
LDFLAGS_LINUX = -lm -fsanitize=address,undefined

SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS   = $(shell pkg-config --libs sdl2)

all: vquest.tos vq-sdl vq-ascii vq-bench.tos

clean:
	rm -f *.o *.tos *.sym vq-sdl vq-ascii vq-bench vq-bench.tos

.PHONY: run
run: vquest.tos
	hatari-prg-args -q --conout 2 --fast-boot true -- $<

.PHONY: bench
bench: vq-bench.tos
	SDL_VIDEODRIVER=dummy hatari-prg-args -q --conout 2 --fast-forward true --fast-boot true -- $<

# ── Atari object files ─────────────────────────────────────────────────────────

vquest_atari.o: vquest.c backend.h vquest.h draw.h credits.h hud.h
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

draw_atari.o: draw.c draw.h
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

credits_atari.o: credits.c credits.h draw.h
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

backend_gemtos.o: backend_gemtos.c backend.h stars.h
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

backend_ascii_atari.o: backend_ascii.c backend.h stars.h hud.h
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

segline.o: segmented-line.git/segline.s
	$(VASM) $(VASMFLAGS) $< -o $@

clipline.o: segmented-line.git/clipline.s
	$(VASM) $(VASMFLAGS) $< -o $@

# ── Linux object files ─────────────────────────────────────────────────────────

vquest_linux.o: vquest.c backend.h vquest.h draw.h credits.h hud.h
	$(CC_LINUX) $(CFLAGS_LINUX) -c $< -o $@

draw_linux.o: draw.c draw.h
	$(CC_LINUX) $(CFLAGS_LINUX) -c $< -o $@

credits_linux.o: credits.c credits.h draw.h
	$(CC_LINUX) $(CFLAGS_LINUX) -c $< -o $@

backend_sdl.o: backend_sdl.c backend.h hud.h stars.h
	$(CC_LINUX) $(CFLAGS_LINUX) $(SDL_CFLAGS) -c $< -o $@

backend_ascii_linux.o: backend_ascii.c backend.h stars.h hud.h
	$(CC_LINUX) $(CFLAGS_LINUX) -c $< -o $@

# ── Link targets ───────────────────────────────────────────────────────────────

vquest.tos: vquest_atari.o backend_gemtos.o stars_atari.o hud_atari.o draw_atari.o credits_atari.o segline.o clipline.o
	$(CC_ATARI) -mshort -nostdlib $(CRT0) $^ -o $@ $(LDFLAGS_ATARI)
	gst2ascii $@ > vquest.sym

vq-sdl: vquest_linux.o backend_sdl.o stars_linux.o hud_linux.o draw_linux.o credits_linux.o
	$(CC_LINUX) $^ -o $@ $(LDFLAGS_LINUX) $(SDL_LIBS)

vq-ascii: vquest_linux.o backend_ascii_linux.o stars_linux.o hud_linux.o draw_linux.o credits_linux.o
	$(CC_LINUX) $^ -o $@ $(LDFLAGS_LINUX)

stars_atari.o: stars.c stars.h backend.h
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

stars_linux.o: stars.c stars.h backend.h
	$(CC_LINUX) $(CFLAGS_LINUX) -c $< -o $@

hud_atari.o: hud.c hud.h backend.h draw.h
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

hud_linux.o: hud.c hud.h backend.h draw.h
	$(CC_LINUX) $(CFLAGS_LINUX) -c $< -o $@

backend_bench.o: backend_bench.c backend.h stars.h hud.h
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

vq-bench.tos: vquest_atari.o backend_bench.o stars_atari.o hud_atari.o draw_atari.o credits_atari.o
	$(CC_ATARI) -mshort -nostdlib $(CRT0) $^ -o $@ $(LDFLAGS_ATARI)
	gst2ascii $@ > vq-bench.sym
