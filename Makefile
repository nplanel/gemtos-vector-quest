
CC_ATARI  = m68k-atari-mint-gcc
CC_LINUX  = gcc
VASM      = vasm

LIBCMINI_DIR ?= ../libcmini
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
	rm -f *.o *.d *.tos *.st *.sym vq-sdl vq-ascii vq-bench vq-bench.tos snd_data.h

vquest.st: vquest.tos
	dd if=/dev/zero of=$@ bs=1k count=720
	mformat -a -f 720 -i $@ ::
	MTOOLS_NO_VFAT=1 mmd -i $@ ::AUTO
	MTOOLS_NO_VFAT=1 mcopy -i $@ -spmv $< ::AUTO/VQUEST.PRG

.PHONY: run
run: vquest.tos
	hatari-prg-args -q --conout 2 --fast-boot true -- $<

.PHONY: floppy
floppy: vquest.st
	hatari $<

.PHONY: bench
bench: vq-bench.tos
	SDL_VIDEODRIVER=dummy hatari-prg-args -q --conout 2 --fast-forward true --fast-boot true -- $<

# ── Per-binary unity compilation ───────────────────────────────────────────────

snd_data.h: sound/intro.ym sound/main.ym sound/fire.ym
	@echo "/* generated — do not edit. Regenerate with: make snd_data.h */" > $@
	@(cd sound && xxd -i intro.ym) | sed 's/^unsigned char/static const unsigned char/' | sed 's/^unsigned int/static const unsigned int/' | sed 's/intro_ym\b/kZikIntro/g' >> $@
	@(cd sound && xxd -i main.ym)  | sed 's/^unsigned char/static const unsigned char/' | sed 's/^unsigned int/static const unsigned int/' | sed 's/main_ym\b/kZikMain/g'   >> $@
	@(cd sound && xxd -i fire.ym)  | sed 's/^unsigned char/static const unsigned char/' | sed 's/^unsigned int/static const unsigned int/' | sed 's/fire_ym\b/kZikFire/g'   >> $@

main_gemtos.o: main_gemtos.c $(UNITY_DEPS) backend_gemtos.c snd_data.h
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
