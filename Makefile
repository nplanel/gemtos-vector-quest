
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

# ── Common source files (compiled for both platforms) ──────────────────────────
SRCS_COMMON = vquest.c draw.c credits.c stars.c hud.c

OBJS_ATARI_COMMON = $(SRCS_COMMON:.c=_atari.o)
OBJS_LINUX_COMMON = $(SRCS_COMMON:.c=_linux.o)

OBJS_ASM = segline.o clipline.o

all: vquest.tos vq-sdl vq-ascii vq-bench.tos

clean:
	rm -f *.o *.d *.tos *.sym vq-sdl vq-ascii vq-bench vq-bench.tos

.PHONY: run
run: vquest.tos
	hatari-prg-args -q --conout 2 --fast-boot true -- $<

.PHONY: bench
bench: vq-bench.tos
	SDL_VIDEODRIVER=dummy hatari-prg-args -q --conout 2 --fast-forward true --fast-boot true -- $<

# ── Compilation pattern rules ──────────────────────────────────────────────────

%_atari.o: %.c
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

%_linux.o: %.c
	$(CC_LINUX) $(CFLAGS_LINUX) -c $< -o $@

# ── Platform-specific backend rules ───────────────────────────────────────────

backend_gemtos.o: backend_gemtos.c
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

backend_bench.o: backend_bench.c
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

backend_sdl.o: backend_sdl.c
	$(CC_LINUX) $(CFLAGS_LINUX) $(SDL_CFLAGS) -c $< -o $@

# ── Assembly rules ─────────────────────────────────────────────────────────────

segline.o: segmented-line.git/segline.s
	$(VASM) $(VASMFLAGS) $< -o $@

clipline.o: segmented-line.git/clipline.s
	$(VASM) $(VASMFLAGS) $< -o $@

# ── Link targets ───────────────────────────────────────────────────────────────

vquest.tos: $(OBJS_ATARI_COMMON) backend_gemtos.o $(OBJS_ASM)
	$(CC_ATARI) -mshort -nostdlib $(CRT0) $^ -o $@ $(LDFLAGS_ATARI)
	gst2ascii $@ > $(@:.tos=.sym)

vq-bench.tos: $(OBJS_ATARI_COMMON) backend_bench.o
	$(CC_ATARI) -mshort -nostdlib $(CRT0) $^ -o $@ $(LDFLAGS_ATARI)
	gst2ascii $@ > $(@:.tos=.sym)

vq-sdl: $(OBJS_LINUX_COMMON) backend_sdl.o
	$(CC_LINUX) $^ -o $@ $(LDFLAGS_LINUX) $(SDL_LIBS)

vq-ascii: $(OBJS_LINUX_COMMON) backend_ascii_linux.o
	$(CC_LINUX) $^ -o $@ $(LDFLAGS_LINUX)

-include $(wildcard *.d)
