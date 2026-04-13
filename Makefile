
CC_ATARI  = m68k-atari-mint-gcc
CC_LINUX  = gcc
VASM      = vasm

LIBCMINI_DIR = /home/ben/src/atari/libcmini
LIBCMINI     = $(LIBCMINI_DIR)/build/mshort
CRT0         = $(LIBCMINI)/objs/crt0.o

VASMFLAGS = -Faout -quiet -x -m68000 -spaces -showopt

# ── Optimisation level — comment/uncomment to switch ───────────────────────────
OPT = -Ofast -DNDEBUG
#OPT = -Og

# ── Flags common to both targets ───────────────────────────────────────────────
CFLAGS_COMMON = $(OPT) -Wall -Wextra -Werror -g -std=gnu99

CFLAGS_ATARI  = $(CFLAGS_COMMON) -mshort -nostdlib -I$(LIBCMINI_DIR)/include -MMD -MP
CFLAGS_LINUX  = $(CFLAGS_COMMON) -fsanitize=address,undefined -MMD -MP

LDFLAGS_ATARI = -L$(LIBCMINI) -lcmini -lgcc -lcmini
LDFLAGS_LINUX = -lm -fsanitize=address,undefined

SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS   = $(shell pkg-config --libs sdl2)

OBJS_ASM = segline.o clipline.o

# Unity build: all modules + backend in one TU per binary
UNITY_DEPS = backend.h draw.c hud.c stars.c credits.c vquest.c render.c physics.c

all: vquest.tos vq-sdl vq-ascii vq-bench.tos

clean:
	rm -f *.o *.d *.tos *.sym vq-sdl vq-ascii vq-bench vq-bench.tos

.PHONY: run
run: vquest.tos
	hatari-prg-args -q --conout 2 --fast-boot true -- $<

.PHONY: bench
bench: vq-bench.tos
	SDL_VIDEODRIVER=dummy hatari-prg-args -q --conout 2 --fast-forward true --fast-boot true -- $<

# ── Per-binary unity compilation ───────────────────────────────────────────────

main_gemtos.o: main_gemtos.c $(UNITY_DEPS) backend_gemtos.c
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

main_bench.o: main_bench.c $(UNITY_DEPS) backend_bench.c
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

main_sdl.o: main_sdl.c $(UNITY_DEPS) backend_sdl.c
	$(CC_LINUX) $(CFLAGS_LINUX) $(SDL_CFLAGS) -c $< -o $@

main_ascii.o: main_ascii.c $(UNITY_DEPS) backend_ascii.c
	$(CC_LINUX) $(CFLAGS_LINUX) -c $< -o $@

# ── Assembly rules ─────────────────────────────────────────────────────────────

segline.o: segmented-line.git/segline.s
	$(VASM) $(VASMFLAGS) $< -o $@

clipline.o: segmented-line.git/clipline.s
	$(VASM) $(VASMFLAGS) $< -o $@

# ── Link targets ───────────────────────────────────────────────────────────────

vquest.tos: main_gemtos.o $(OBJS_ASM)
	$(CC_ATARI) -mshort -nostdlib $(CRT0) $^ -o $@ $(LDFLAGS_ATARI)
	gst2ascii $@ > $(@:.tos=.sym)

vq-bench.tos: main_bench.o
	$(CC_ATARI) -mshort -nostdlib $(CRT0) $^ -o $@ $(LDFLAGS_ATARI)
	gst2ascii $@ > $(@:.tos=.sym)

vq-sdl: main_sdl.o
	$(CC_LINUX) $^ -o $@ $(LDFLAGS_LINUX) $(SDL_LIBS)

vq-ascii: main_ascii.o
	$(CC_LINUX) $^ -o $@ $(LDFLAGS_LINUX)

-include $(wildcard *.d)
